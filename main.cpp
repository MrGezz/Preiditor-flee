#include "Menu.h"
#include "Events.h"
#include "Settings.h"
#include "logger.h"
#include <thread>
#include <chrono>
#include <string>
#include <Windows.h>

namespace
{
    // Dependency-free marker write -- no SKSE::Init, no PluginDeclaration, no spdlog.
    // If this file never appears after a real game session, the exported
    // SKSEPluginLoad body is not running at all despite skse64.log saying the DLL
    // loaded -- point at a loader-level cause instead of anything in this file.
    void WriteRawMarker(const char* stage)
    {
        char pathBuf[MAX_PATH]{};
        if (!GetTempPathA(MAX_PATH, pathBuf)) {
            return;
        }
        std::string fullPath = std::string(pathBuf) + "PFF_marker.txt";
        HANDLE h = CreateFileA(fullPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
        SetFilePointer(h, 0, nullptr, FILE_END);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char line[256];
        int len = wsprintfA(line, "%04d-%02d-%02d %02d:%02d:%02d stage=%s\r\n", st.wYear, st.wMonth, st.wDay,
                             st.wHour, st.wMinute, st.wSecond, stage);
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }

    // Set on kPreLoadGame / before-new-game; cleared on kPostLoadGame / kNewGame.
    // While true the tick thread skips posting tasks, preventing OnTick from
    // running against a partially-torn-down game world.
    void SetShutdownFlag(bool value)
    {
        PFF::FleeManager::GetSingleton()->shutdownFlag.store(value, std::memory_order_release);
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    WriteRawMarker("entry");
    SKSE::Init(a_skse);
    WriteRawMarker("after_SKSE_Init");
    try {
        SetupLog();
        WriteRawMarker("after_SetupLog_ok");
    } catch (const std::exception& e) {
        WriteRawMarker((std::string("SetupLog_threw:") + e.what()).c_str());
    } catch (...) {
        WriteRawMarker("SetupLog_threw_unknown");
    }

    auto* messaging = SKSE::GetMessagingInterface();
    messaging->RegisterListener([](SKSE::MessagingInterface::Message* msg) {
        switch (msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            PFF::Settings::GetSingleton()->Load();
            PFF::Menu::Register();
            logger::info("PFF: kDataLoaded -- settings loaded, menu registered, starting tick thread");

            // Pace OnTick (twice a second) from an independent worker thread that posts
            // one-shot tasks onto the game thread. Never self-requeue an AddTask from
            // inside its own callback -- the task queue drains pop-until-empty each
            // pass, so a self-requeued task reruns in the SAME drain and hard-freezes
            // the main thread.
            std::thread([]() {
                while (true) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    if (PFF::FleeManager::GetSingleton()->shutdownFlag.load(std::memory_order_acquire)) {
                        continue; // game world is being torn down; skip this tick
                    }
                    SKSE::GetTaskInterface()->AddTask([]() {
                        PFF::FleeManager::GetSingleton()->OnTick();
                    });
                }
            }).detach();
            break;

        // Suppress OnTick while the game world is being rebuilt.  kPreLoadGame fires
        // before the engine tears down the current world for a save-load; kNewGame and
        // kPostLoadGame fire once the new world is ready.
        case SKSE::MessagingInterface::kPreLoadGame:
            logger::info("PFF: kPreLoadGame -- suppressing OnTick");
            SetShutdownFlag(true);
            break;

        case SKSE::MessagingInterface::kNewGame:
            logger::info("PFF: kNewGame -- resuming OnTick");
            SetShutdownFlag(false);
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
            logger::info("PFF: kPostLoadGame -- resuming OnTick");
            SetShutdownFlag(false);
            break;
        }
    });

    return true;
}
