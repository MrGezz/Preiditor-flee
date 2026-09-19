#pragma once
#include <atomic>
#include <unordered_map>
#include <vector>

namespace PFF
{
    enum class SpeciesCategory
    {
        kNone,
        kPredator,
        kPrey,
        kSpider
    };

    enum class FleeBehaviorState
    {
        kNormal,
        kStalking
    };

    struct ActorTracker
    {
        FleeBehaviorState state = FleeBehaviorState::kNormal;
        RE::FormID         lightHolderID = 0;
        float              cooldownUntil = 0.0f; // game-time hours
        float              cachedConfidence = 2.0f; // Confidence AV to restore once the light is gone
        float              deterrentUntil = 0.0f;   // game-time hours; keeps "still deterred" true
                                                      // briefly past the last confirmed torch/fire tick
    };

    // Pre-collected deterrent holder from the first pass -- avoids nested
    // ForEachReferenceInRange on the same cell's BSSpinLock.
    struct DeterrentHolder
    {
        RE::Actor*   actor;
        RE::NiPoint3 position;
    };

    // Driven by a worker-thread-paced task (see main.cpp), not an event sink -- no
    // BSTEventSink base is needed here.
    class FleeManager
    {
    public:
        static FleeManager* GetSingleton();

        // Set by main.cpp during game-state transitions (save load, new game) and
        // checked by the tick thread to suppress OnTick while the world is torn down.
        std::atomic<bool> shutdownFlag{false};

        void OnTick();

    private:
        std::unordered_map<RE::FormID, ActorTracker> trackers;

        RE::TESObjectLIGH*  GetHeldLight(RE::Actor* actor);
        bool                IsCastingElementalSpell(RE::Actor* actor, RE::ActorValue resistType);
        bool                HasDeterrent(RE::Actor* actor);
        bool                HasKeywordCascade(RE::Actor* actor, RE::BGSKeyword* keyword);
        SpeciesCategory     GetSpeciesCategory(RE::Actor* actor);
        bool                IsExcluded(RE::Actor* actor);
        bool                IsOnCooldown(RE::Actor* actor);
        void                SetCooldown(RE::Actor* actor);
        void                ResumeAggression(RE::Actor* actor, SpeciesCategory category, float confidenceToRestore);

        // Returns true if the actor reference is safe to operate on: non-null, loaded
        // 3D, valid parent cell, not deleted/disabled.  Catches actors that are mid-
        // unload or partially torn down -- the cell's BSSpinLock keeps the NiPointer
        // alive but the actor's own fields may already be invalidated.
        static bool IsActorValid(RE::Actor* actor);
    };
}
