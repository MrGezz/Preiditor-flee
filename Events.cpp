#include "Events.h"
#include "Settings.h"
#include "logger.h"

namespace PFF
{
    FleeManager* FleeManager::GetSingleton()
    {
        static FleeManager singleton;
        return &singleton;
    }

    // Catches actors that are mid-unload or partially torn down.  The cell's
    // BSSpinLock keeps the NiPointer alive, but the actor's own fields (parent
    // cell, 3D, base form) may already be invalidated during a cell transition
    // or save load.  Returning false for any of these means the caller skips
    // the actor rather than dereferencing garbage.
    bool FleeManager::IsActorValid(RE::Actor* actor)
    {
        if (!actor) return false;
        if (actor->IsDeleted() || actor->IsDisabled()) return false;
        if (!actor->GetParentCell()) return false;
        if (!actor->Get3D()) return false;
        return true;
    }

    // A torch or lantern in Skyrim is a TESObjectLIGH equipped directly (not a weapon with a
    // light attached) -- checking both hands for an equipped Light covers vanilla torches and
    // any lantern mod that follows the same wieldable-light convention. This deliberately does
    // NOT check inventory -- only what's actually equipped in a hand counts as "held".
    RE::TESObjectLIGH* FleeManager::GetHeldLight(RE::Actor* actor)
    {
        if (!actor) return nullptr;
        if (auto* right = actor->GetEquippedObject(false)) {
            if (auto* light = right->As<RE::TESObjectLIGH>()) return light;
        }
        if (auto* left = actor->GetEquippedObject(true)) {
            if (auto* light = left->As<RE::TESObjectLIGH>()) return light;
        }
        return nullptr;
    }

    // Actively invoking Destruction magic of a given element in a hand -- not merely having it
    // equipped/selected (that's MagicCaster::State::kReady, which sits idle any time a spell is
    // favorited/readied). Charging or casting is what actually produces the visible/audible
    // effect, matching the "held lit torch" theme. Elemental type is read the same way the engine
    // itself does: the effect's own EffectSetting.resistVariable (the field Flames/Firebolt/
    // Fireball/Lightning Bolt/Poison Spray etc. all carry, tagged to their matching resist AV).
    bool FleeManager::IsCastingElementalSpell(RE::Actor* actor, RE::ActorValue resistType)
    {
        if (!actor) return false;
        for (auto source : { RE::MagicSystem::CastingSource::kLeftHand, RE::MagicSystem::CastingSource::kRightHand }) {
            auto* caster = actor->GetMagicCaster(source);
            if (!caster || !caster->currentSpell) continue;

            auto state = caster->state.get();
            if (state != RE::MagicCaster::State::kCharging && state != RE::MagicCaster::State::kCasting) continue;

            for (auto* effect : caster->currentSpell->effects) {
                if (effect && effect->baseEffect && effect->baseEffect->data.resistVariable == resistType) {
                    return true;
                }
            }
        }
        return false;
    }

    bool FleeManager::HasDeterrent(RE::Actor* actor)
    {
        auto* settings = Settings::GetSingleton();
        if (GetHeldLight(actor) != nullptr) return true;
        if (settings->bAffectFireSpells && IsCastingElementalSpell(actor, RE::ActorValue::kResistFire)) return true;
        if (settings->bAffectLightningSpells && IsCastingElementalSpell(actor, RE::ActorValue::kResistShock)) return true;
        if (settings->bAffectPoisonSpells && IsCastingElementalSpell(actor, RE::ActorValue::kPoisonResist)) return true;
        return false;
    }

    // SkyPatcher's keyword-framework rules (RKF_ActorType*, and vanilla ActorTypeHorse/
    // ActorTypeDragon) patch the RACE record's keyword list, not the individual actor/NPC_
    // record -- confirmed directly: e.g. the Helgen tutorial bear's own NPC_ record carries
    // ZERO keywords at all, only its race (BearBrownRace) does. Actor and TESRace are separate
    // BGSKeywordForm objects with independently-checked keyword lists, so actor->HasKeyword()
    // alone silently misses every race-level keyword -- which is most creature classification
    // in Skyrim. Must explicitly check both.
    bool FleeManager::HasKeywordCascade(RE::Actor* actor, RE::BGSKeyword* keyword)
    {
        if (!actor || !keyword) return false;
        if (actor->HasKeyword(keyword)) return true;
        if (auto* race = actor->GetRace()) {
            if (race->HasKeyword(keyword)) return true;
        }
        return false;
    }

    // Confirmed directly (2026-09-06 test session): SkyPatcher Keyword Framework's RKF_ActorType*
    // keywords only get applied to races its own rules recognize by 3D model path. A creature
    // VARIETY/replacer mod (e.g. Animallica's "Grey Wolf") can supply a custom race SkyPatcher's
    // rules never match -- confirmed the actor carried zero RKF keywords, only that mod's own
    // OCF_RaceAnimal* tags. RKF is therefore NOT reliable as the primary yes/no gate across a
    // modded creature roster.
    //
    // The robust, native-vanilla signal instead: virtually every hostile wild creature (wolves,
    // bears, sabre cats, trolls, frostbite spiders, ice wraiths) is a member of the vanilla
    // "PredatorFaction" DIRECTLY on its own actor record (not just its race) -- confirmed via a
    // live debug-HUD read showing both the Helgen tutorial bear AND the Helgen frostbite spider
    // carry it. Faction membership is an AI-behavior assignment, not a visual one, so a creature-
    // variety/reskin mod has no reason to strip it. This is the PRIMARY gate. RKF keywords are
    // then used only as a best-effort REFINEMENT to route an already-confirmed predator into the
    // right per-species toggle (Wolves/Bears/...) -- if that refinement can't identify the exact
    // species (keyword missing, as with modded variants), default to "affected" rather than
    // silently dropping a creature we already know is a predator.
    SpeciesCategory FleeManager::GetSpeciesCategory(RE::Actor* actor)
    {
        auto* settings = Settings::GetSingleton();
        if (!actor) return SpeciesCategory::kNone;

        if (settings->bUseKeywordFilter) {
            static auto* customKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("PFF_PredatorFireFlee");
            return HasKeywordCascade(actor, customKeyword) ? SpeciesCategory::kPredator : SpeciesCategory::kNone;
        }

        auto hasRKF = [this, actor](const char* editorID) {
            auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(editorID);
            return HasKeywordCascade(actor, kw);
        };

        // Spriggans are plant/magic-type hostiles -- confirmed via live load-order read that the
        // base Spriggan actor carries CreatureFaction + SprigganFaction + SprigganPredatorFaction,
        // but NOT plain PredatorFaction. Gated separately since the generic PredatorFaction check
        // below would otherwise silently miss every spriggan.
        static auto* sprigganFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("SprigganFaction");
        if (sprigganFaction && actor->IsInFaction(sprigganFaction)) {
            return settings->bAffectSpriggans ? SpeciesCategory::kPredator : SpeciesCategory::kNone;
        }

        static auto* predatorFaction = RE::TESForm::LookupByEditorID<RE::TESFaction>("PredatorFaction");
        bool isPredatorFaction = predatorFaction && actor->IsInFaction(predatorFaction);

        if (isPredatorFaction) {
            bool isSpider = hasRKF("RKF_ActorTypeFrostSpider");
            if (isSpider) return settings->bAffectSpiders ? SpeciesCategory::kSpider : SpeciesCategory::kNone;

            bool identified = false, allowed = false;
            auto check = [&](bool toggle, const char* editorID) {
                if (hasRKF(editorID)) {
                    identified = true;
                    allowed = allowed || toggle;
                }
            };
            check(settings->bAffectWolves, "RKF_ActorTypeWolf");
            check(settings->bAffectBears, "RKF_ActorTypeBear");
            check(settings->bAffectSabreCats, "RKF_ActorTypeSabreCat");
            check(settings->bAffectTrolls, "RKF_ActorTypeTroll");
            check(settings->bAffectIceWraiths, "RKF_ActorTypeIceWraith");

            if (!identified) {
                // Couldn't name the exact species (e.g. a creature-variety mod's custom race) --
                // still a confirmed predator, so fall back to "affected" as long as the user
                // hasn't disabled every predator species.
                allowed = settings->bAffectWolves || settings->bAffectBears || settings->bAffectSabreCats ||
                          settings->bAffectTrolls || settings->bAffectIceWraiths;
            }
            return allowed ? SpeciesCategory::kPredator : SpeciesCategory::kNone;
        }

        // Prey: no vanilla "PreyFaction" exists, so fall back to the broad vanilla
        // ActorTypeAnimal keyword (race-cascaded) for anything that isn't already a recognized
        // predator/spider/excluded actor -- catches deer/goat/horker/mammoth/skeever and any
        // similar creature-variety replacement without needing per-species keyword coverage.
        static auto* animalKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeAnimal");
        if (HasKeywordCascade(actor, animalKeyword)) {
            bool identified = false, allowed = false;
            auto check = [&](bool toggle, const char* editorID) {
                if (hasRKF(editorID)) {
                    identified = true;
                    allowed = allowed || toggle;
                }
            };
            check(settings->bAffectDeer, "RKF_ActorTypeDeer");
            check(settings->bAffectGoats, "RKF_ActorTypeGoat");
            check(settings->bAffectHorkers, "RKF_ActorTypeHorker");
            check(settings->bAffectMammoths, "RKF_ActorTypeMammoth");
            check(settings->bAffectSkeevers, "RKF_ActorTypeSkeever");

            if (!identified) {
                allowed = settings->bAffectDeer || settings->bAffectGoats || settings->bAffectHorkers ||
                          settings->bAffectMammoths || settings->bAffectSkeevers;
            }
            return allowed ? SpeciesCategory::kPrey : SpeciesCategory::kNone;
        }

        return SpeciesCategory::kNone;
    }

    bool FleeManager::IsExcluded(RE::Actor* actor)
    {
        static auto* horseKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeHorse");
        static auto* dragonKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeDragon");
        static auto* bossKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ActorTypeDLC1Boss");

        if (HasKeywordCascade(actor, horseKeyword)) return true;
        if (HasKeywordCascade(actor, dragonKeyword)) return true;
        if (HasKeywordCascade(actor, bossKeyword)) return true;

        // ActorTypeDLC1Boss only covers Dawnguard-added bosses; the general "this NPC is
        // special" signal for everything else is the vanilla Unique flag on its base actor.
        if (auto* base = actor->GetActorBase()) {
            if (base->IsUnique()) return true;
        }

        return false;
    }

    bool FleeManager::IsOnCooldown(RE::Actor* actor)
    {
        auto now = RE::Calendar::GetSingleton()->GetCurrentGameTime() * 24.0f;
        auto it = trackers.find(actor->GetFormID());
        return it != trackers.end() && now < it->second.cooldownUntil;
    }

    void FleeManager::SetCooldown(RE::Actor* actor)
    {
        auto* settings = Settings::GetSingleton();
        auto now = RE::Calendar::GetSingleton()->GetCurrentGameTime() * 24.0f;
        trackers[actor->GetFormID()].cooldownUntil = now + settings->fCooldown / 3600.0f;
    }

    // Predators/spiders are already hostile to the player by their own vanilla faction
    // relationship -- restoring their cached Confidence is enough to let their own aggression
    // resume naturally (see the kNormal case in OnTick for why Confidence, not InitiateFlee, is
    // the actual flee mechanism). Prey species have no vanilla combat behavior at all, so
    // "attack" is approximated by forcing their Aggression actor value up; many prey races
    // still won't produce real attack animations since they were never authored with any,
    // which is a game-content limitation, not something fixable from this plugin alone.
    void FleeManager::ResumeAggression(RE::Actor* actor, SpeciesCategory category, float confidenceToRestore)
    {
        if (auto* avOwner = actor->AsActorValueOwner()) {
            avOwner->SetActorValue(RE::ActorValue::kConfidence, confidenceToRestore);
            if (category == SpeciesCategory::kPrey) {
                avOwner->SetActorValue(RE::ActorValue::kAggression, 2.0f); // Aggressive
            }
        }
        actor->EvaluatePackage(true, true);
    }

    void FleeManager::OnTick()
    {
        auto* settings = Settings::GetSingleton();
        if (!settings->bEnabled) {
            logger::trace("PFF: OnTick called but bEnabled=false");
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || !player->GetParentCell()) {
            logger::trace("PFF: OnTick called but no player/3D/cell");
            return;
        }
        auto* cell = player->GetParentCell();
        auto playerPos = player->GetPosition(); // copy -- safe across iterations
        logger::trace("PFF: OnTick running, cell=0x{:X}", cell->GetFormID());

        // --- Phase 1: collect deterrent holders -------------------------------------------
        // A single ForEachReferenceInRange pass collects every actor in range that is
        // carrying a lit torch or actively casting an enabled spell.  This replaces the
        // old FindNearestLitLightHolder, which was called *inside* the creature scan's
        // own ForEachReferenceInRange lambda -- nesting two iterations on the same cell
        // under the same BSSpinLock.  BSSpinLock is reentrant so it didn't deadlock, but
        // the double lock-hold widened the window for another thread (the engine's own
        // cell-transition code) to leave a partially-torn-down reference in the list that
        // the inner iteration would then dereference.
        //
        // Collecting holders first and searching the vector in phase 2 eliminates the
        // nested iteration entirely.
        const float holderRadius = settings->fStalkDistance + settings->fDetectionRadius;
        std::vector<DeterrentHolder> holders;
        holders.reserve(8); // most cells have few torch-carriers

        // TESObjectCELL::ForEachReferenceInRange takes
        // std::function<BSContainer::ForEachResult(TESObjectREFR*)> -- a POINTER, which the
        // engine can hand us as null for a torn-down reference. Verified in
        // CommonLibSSE-NG/include/RE/T/TESObjectCELL.h:199.
        cell->ForEachReferenceInRange(playerPos, holderRadius, [&](RE::TESObjectREFR* ref) {
            if (!ref) return RE::BSContainer::ForEachResult::kContinue;
            if (ref->IsDeleted() || ref->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
            auto* actor = ref->As<RE::Actor>();
            if (!actor || actor->IsDead()) return RE::BSContainer::ForEachResult::kContinue;
            if (!IsActorValid(actor)) return RE::BSContainer::ForEachResult::kContinue;
            if (HasDeterrent(actor)) {
                holders.push_back({actor, actor->GetPosition()});
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // --- Phase 2: creature scan ------------------------------------------------------
        cell->ForEachReferenceInRange(playerPos, settings->fStalkDistance, [&](RE::TESObjectREFR* ref) {
            if (!ref) return RE::BSContainer::ForEachResult::kContinue;
            if (ref->IsDeleted() || ref->IsDisabled()) return RE::BSContainer::ForEachResult::kContinue;
            auto* actor = ref->As<RE::Actor>();
            if (!actor || actor->IsPlayerRef() || actor->IsDead()) return RE::BSContainer::ForEachResult::kContinue;
            if (!IsActorValid(actor)) return RE::BSContainer::ForEachResult::kContinue;
            if (IsExcluded(actor)) return RE::BSContainer::ForEachResult::kContinue;

            auto category = GetSpeciesCategory(actor);
            if (category == SpeciesCategory::kNone) return RE::BSContainer::ForEachResult::kContinue;

            auto& tracker = trackers[actor->GetFormID()];
            logger::trace("PFF: {} (0x{:X}) category={} state={}", actor->GetName(), actor->GetFormID(),
                          static_cast<int>(category), static_cast<int>(tracker.state));

            switch (tracker.state) {
            case FleeBehaviorState::kNormal: {
                if (IsOnCooldown(actor)) {
                    logger::trace("PFF: {} on cooldown, skipping", actor->GetName());
                    break;
                }
                // Find the nearest deterrent holder from the pre-collected vector
                RE::TESObjectREFR* holder = nullptr;
                float nearestDist = settings->fDetectionRadius;
                auto actorPos = actor->GetPosition();
                for (auto& h : holders) {
                    float dist = actorPos.GetDistance(h.position);
                    if (dist < nearestDist) {
                        holder = h.actor;
                        nearestDist = dist;
                    }
                }

                if (holder) {
                    // InitiateFlee (with or without StopCombat()) proved unreliable across two
                    // full test rounds -- confirmed via live log that the actor's own combat
                    // controller either kept re-closing distance every tick (no StopCombat) or,
                    // once StopCombat() was added, just went idle/frozen instead of fleeing
                    // (distance pinned exactly still for many ticks in a row). The real vanilla
                    // mechanism for "make a combatant flee right now" is the Confidence actor
                    // value: the engine's own combat controller reads it every AI think-cycle
                    // (this is how naturally cowardly creatures/NPCs flee) -- so drive that
                    // instead of fighting the controller with an externally injected package.
                    auto* avOwner = actor->AsActorValueOwner();
                    logger::info("PFF: {} detected lit light held by {} -- lowering Confidence to flee", actor->GetName(), holder->GetName());
                    tracker.lightHolderID = holder->GetFormID();
                    tracker.cachedConfidence = avOwner ? avOwner->GetActorValue(RE::ActorValue::kConfidence) : 2.0f;
                    tracker.deterrentUntil = RE::Calendar::GetSingleton()->GetCurrentGameTime() * 24.0f + settings->fSpellCastLinger / 3600.0f;
                    tracker.state = FleeBehaviorState::kStalking;
                    if (avOwner) {
                        avOwner->SetActorValue(RE::ActorValue::kConfidence, 0.0f); // Cowardly
                    }
                    actor->EvaluatePackage(true, true);
                } else {
                    logger::trace("PFF: {} no lit light holder within {} units", actor->GetName(), settings->fDetectionRadius);
                }
                break;
            }

            case FleeBehaviorState::kStalking: {
                auto* holderActor = RE::TESForm::LookupByID<RE::Actor>(tracker.lightHolderID);
                bool activeNow = holderActor && !holderActor->IsDead() &&
                                 IsActorValid(holderActor) && HasDeterrent(holderActor);
                auto now = RE::Calendar::GetSingleton()->GetCurrentGameTime() * 24.0f;
                if (activeNow) {
                    // Refreshes every tick for a continuously-held torch (no behavior change there)
                    // and re-extends the window on every fresh fire-spell cast caught mid-poll.
                    tracker.deterrentUntil = now + settings->fSpellCastLinger / 3600.0f;
                }
                bool stillLit = activeNow || now < tracker.deterrentUntil;
                logger::trace("PFF: {} stalking, holder={} stillLit={}", actor->GetName(),
                              holderActor ? holderActor->GetName() : "none", stillLit);

                if (!stillLit) {
                    // The light went out (or its holder is gone) -- the deterrent is gone.
                    logger::info("PFF: {} light gone -- resuming aggression", actor->GetName());
                    ResumeAggression(actor, category, tracker.cachedConfidence);
                    SetCooldown(actor);
                    tracker.state = FleeBehaviorState::kNormal;
                    tracker.lightHolderID = 0;
                }
                // No re-approach handling needed: Confidence stays at Cowardly for the whole
                // stalking state, so the engine's own combat AI keeps deciding to flee on its
                // own, continuously, every one of its own AI ticks -- not just our 500ms polls.
                break;
            }

            default:
                break;
            }

            return RE::BSContainer::ForEachResult::kContinue;
        });

        // --- Tracker pruning --------------------------------------------------------------
        // Remove entries for actors whose FormIDs are no longer loaded.  Without this the
        // map grows without bound across cell transitions, and a recycled FormID could
        // inherit a stale cooldown/state from a completely different actor.
        for (auto it = trackers.begin(); it != trackers.end(); ) {
            auto* form = RE::TESForm::LookupByID(it->first);
            if (!form || !form->Is(RE::FormType::ActorCharacter)) {
                it = trackers.erase(it);
            } else {
                ++it;
            }
        }
    }
}
