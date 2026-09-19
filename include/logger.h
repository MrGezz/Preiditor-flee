#pragma once
#include <spdlog/sinks/basic_file_sink.h>


namespace logger = SKSE::log;

inline void SetupLog() {
    auto logsFolder = SKSE::log::log_directory();
    if (!logsFolder) SKSE::stl::report_and_fail("SKSE log_directory not provided, logs disabled.");
    auto pluginName = SKSE::PluginDeclaration::GetSingleton()->GetName();
    auto logFilePath = *logsFolder / std::format("{}.log", pluginName);
    auto fileLoggerPtr = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.string(), true);
    auto loggerPtr = std::make_shared<spdlog::logger>("log", std::move(fileLoggerPtr));
    spdlog::set_default_logger(std::move(loggerPtr));
    // Was forced to trace while diagnosing the flee logic, with a note to dial it back once the
    // behaviour was confirmed end to end. It was, in game, on 2026-09-13 -- and at trace level
    // OnTick wrote a line every 500 ms plus one per nearby actor, each flushed to disk: 2,735
    // trace lines and 259,380 bytes in a 20-minute session, growing without bound. Builds with NDEBUG
    // defined (Release, RelWithDebInfo) now log at info and flush each info line, which are rare
    // (load and save-game events) and worth keeping if the game dies. A Debug build keeps trace.
#ifndef NDEBUG
    spdlog::set_level(spdlog::level::trace);
    spdlog::flush_on(spdlog::level::trace);
#else
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
#endif
    logger::info("Name of the plugin is {}.", pluginName);
    // NOTE: logging SKSE::PluginDeclaration::GetSingleton()->GetVersion() directly fails to
    // compile against this fmt v12 -- REL::Version's custom formatter isn't const-qualified the
    // way fmt v12 requires. Not essential; skip it rather than fight the library version.
}


