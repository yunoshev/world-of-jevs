#include "WojCombatLog.h"
#include "WojAI.h"
#include "WojConfig.h"
#include "WojBridge.h"
#include "WojHelp.h"
#include "WojRegistry.h"
#include "WojRuntimeIdentity.h"
#include "WojRuntimeBinding.h"
#include "WojTypes.h"
#include "AllCreatureScript.h"
#include "AllSpellScript.h"
#include "Creature.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "TemporarySummon.h"
#include "Unit.h"
#include "UnitScript.h"
#include "json.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
    constexpr float ENEMY_RANGE_YARDS = 20.0f;

    std::chrono::steady_clock::time_point gWorldStarted;
    std::unordered_set<uint32_t> gSeenInRange;
    std::unordered_map<uint64_t, bool> gFleeing;
    // Game-thread-only observers; never installed as Creature::AI and never
    // permitted to execute an action. Their state is independent per life.
    std::unordered_map<WojActorKey, std::unique_ptr<WojAI>, WojActorKeyHash> gOriginalShadows;

    char const* RoutineProfile(Creature const* creature)
    {
        if (!creature)
            return "idle";
        switch (creature->GetDefaultMovementType())
        {
            case RANDOM_MOTION_TYPE: return "random";
            case WAYPOINT_MOTION_TYPE: return "waypoint";
            default: return "idle";
        }
    }

    json Point(float x, float y, float z)
    {
        return {{"x", x}, {"y", y}, {"z", z}};
    }


    uint64_t MonotonicMs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gWorldStarted).count());
    }

    uint32_t DurationMs(std::chrono::steady_clock::time_point end,
                        std::chrono::steady_clock::time_point begin)
    {
        int64_t const value = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
        return static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(value, 0), std::numeric_limits<uint32_t>::max()));
    }

    std::string UtcNow()
    {
        auto const now = std::chrono::system_clock::now();
        auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::time_t const raw = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &raw);
#else
        gmtime_r(&raw, &utc);
#endif
        std::ostringstream out;
        out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
            << std::setw(3) << std::setfill('0') << milliseconds.count() << 'Z';
        return out.str();
    }

    json Entity(Unit const* unit)
    {
        if (!unit)
            return nullptr;

        json value = {
            {"id", std::to_string(unit->GetGUID().GetRawValue())},
            {"guid_spawn", nullptr},
            {"map_id", unit->GetMapId()},
            {"instance_id", unit->GetInstanceId()},
            {"raw_guid", std::to_string(unit->GetGUID().GetRawValue())},
            {"entry", unit->GetEntry()},
            {"name", unit->GetName()}
        };
        if (Creature const* creature = unit->ToCreature())
        {
            if (uint32_t const policyActorId = WojRuntimeBinding::Instance().Resolve(creature))
                value["guid_spawn"] = policyActorId;
        }
        return value;
    }

    json TransportEntity(WojTransportEvent const& event)
    {
        return {
            {"id", event.actorId},
            {"guid_spawn", event.actorGuidSpawn},
            {"map_id", event.actorMapId},
            {"instance_id", event.actorInstanceId},
            {"raw_guid", std::to_string(event.actorRawGuid)},
            {"entry", event.actorEntry},
            {"name", event.actorName}
        };
    }

    uint64_t MonotonicAt(std::chrono::steady_clock::time_point point)
    {
        if (point == std::chrono::steady_clock::time_point{} || point <= gWorldStarted)
            return 0;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            point - gWorldStarted).count());
    }

    json Envelope(char const* event, Unit const* src, Unit const* dst)
    {
        return {
            {"log_schema_version", "1.0"},
            {"ts", UtcNow()},
            {"t_ms", MonotonicMs()},
            {"server_boot_id", nullptr},
            {"event", event},
            {"mode", WojConfig::Instance().ModeName()},
            {"src", Entity(src)},
            {"dst", Entity(dst)},
            {"amount", nullptr},
            {"spell", nullptr}
        };
    }
}

WojCombatLog& WojCombatLog::Instance()
{
    static WojCombatLog instance;
    return instance;
}

void WojCombatLog::Initialize()
{
    gWorldStarted = std::chrono::steady_clock::now();
    _serverBootId = WojBridge::ServerBootId();
    _observationId.clear();
    _observationSequence = 0;
    gSeenInRange.clear();
    gFleeing.clear();
    {
        std::lock_guard<std::mutex> guard(_lifecycleLock);
        _lifecycleSeen.clear();
        _aiInitRolled.clear();
    }
}

void WojCombatLog::WriteCreatureLifecycle(Creature const* creature, char const* stage)
{
    if (!creature || !creature->GetSpawnId())
        return;
    uint64_t const raw = creature->GetGUID().GetRawValue();
    WojActorBinding const* binding = WojConfig::Instance().FindActor(creature->GetSpawnId());
    bool const configured = binding && !binding->runtimeSummon && binding->hasAiInitDespawn &&
        binding->entry == creature->GetEntry() && binding->mapId == creature->GetMapId();
    bool log = configured;
    {
        std::lock_guard<std::mutex> guard(_lifecycleLock);
        if (std::string(stage) == "add_world")
        {
            // Core initializes AI before this hook. An actor already in the
            // world must not acquire a new AI_INIT roll on policy reload.
            _aiInitRolled.insert(raw);
            if (configured)
                _lifecycleSeen.insert(raw);
        }
        else if (std::string(stage) == "remove_world")
        {
            log = _lifecycleSeen.erase(raw) != 0 || configured;
            _aiInitRolled.erase(raw);
        }
    }
    if (!log)
        return;
    json value = Envelope("creature_lifecycle", creature, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["src"]["guid_spawn"] = creature->GetSpawnId();
    value["stage"] = stage;
    value["physical_spawn_id"] = creature->GetSpawnId();
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

bool WojCombatLog::ClaimAiInitDespawnRoll(Creature const* creature)
{
    if (!creature)
        return false;
    std::lock_guard<std::mutex> guard(_lifecycleLock);
    return _aiInitRolled.insert(creature->GetGUID().GetRawValue()).second;
}

void WojCombatLog::ResetAiInitDespawnRoll(Creature const* creature)
{
    if (!creature)
        return;
    std::lock_guard<std::mutex> guard(_lifecycleLock);
    _aiInitRolled.erase(creature->GetGUID().GetRawValue());
}

void WojCombatLog::WriteAiInitDespawnRoll(Creature const* creature, uint32_t chancePct,
                                          uint32_t delayMs, bool selected, uint32_t policyRevision)
{
    if (!creature)
        return;
    json value = Envelope("creature_lifecycle", creature, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["src"]["guid_spawn"] = creature->GetSpawnId();
    value["stage"] = "ai_init_roll";
    value["physical_spawn_id"] = creature->GetSpawnId();
    value["chance_pct"] = chancePct;
    value["delay_ms"] = delayMs;
    value["selected"] = selected;
    value["policy_revision"] = policyRevision;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteCombatEvent(char const* event, Unit const* src, Unit const* dst,
                                    uint32_t amount, bool hasAmount, uint32_t spell, bool melee)
{
    json value = Envelope(event, src, dst);
    value["server_boot_id"] = _serverBootId;
    if (hasAmount)
        value["amount"] = amount;
    if (melee)
        value["spell"] = "melee";
    else if (spell)
        value["spell"] = spell;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDecisionActivation(Unit const* actor, bool active,
                                           char const* reason, float radiusYards,
                                           Unit const* nearbyPlayer)
{
    json value = Envelope("decision_activation", actor, nearbyPlayer);
    value["server_boot_id"] = _serverBootId;
    value["active"] = active;
    value["reason"] = reason ? reason : "unknown";
    value["radius_yards"] = radiusYards;
    value["player_distance_yards"] = actor && nearbyPlayer
        ? json(actor->GetDistance(nearbyPlayer)) : json(nullptr);
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDungeonProgress(Creature const* actor, char const* encounter,
                                        bool applied, uint32_t objectSpawnId,
                                        uint32_t objectEntry, uint32_t objectState,
                                        uint32_t instanceData)
{
    json value = Envelope("dungeon_progress", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["encounter"] = encounter ? encounter : "unknown";
    value["applied"] = applied;
    value["object_spawn_id"] = objectSpawnId;
    value["object_entry"] = objectEntry;
    value["object_state"] = objectState;
    value["instance_data"] = instanceData == std::numeric_limits<uint32_t>::max()
        ? json(nullptr) : json(instanceData);
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDynamicActorBinding(Creature const* actor, Creature const* source,
                                            uint32_t policyActorId, uint32_t createdBySpell)
{
    json value = Envelope("dynamic_actor_bound", actor, source);
    value["server_boot_id"] = _serverBootId;
    value["policy_actor_id"] = policyActorId;
    value["physical_spawn_id"] = actor ? actor->GetSpawnId() : 0;
    value["created_by_spell"] = createdBySpell;
    value["policy_revision"] = WojConfig::Instance().Behavior.revision;
    if (WojActorBinding const* binding = WojConfig::Instance().FindActor(policyActorId))
    {
        value["binding_generation"] = binding->generation;
        value["group_id"] = binding->groupId;
        value["source_actor_id"] = binding->sourceActorId;
    }
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteGoDynamicActorBinding(Creature const* actor, GameObject const* source,
                                               uint32_t policyActorId, uint32_t createdBySpell,
                                               uint32_t summonIndex, bool reportUse)
{
    json value = Envelope("go_dynamic_actor_bound", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["policy_actor_id"] = policyActorId;
    value["physical_spawn_id"] = actor ? actor->GetSpawnId() : 0;
    value["created_by_spell"] = createdBySpell;
    value["summon_index"] = summonIndex;
    value["trigger"] = reportUse ? "report_use" : "item_cast";
    value["source_gameobject"] = source ? json{
        {"raw_guid", std::to_string(source->GetGUID().GetRawValue())},
        {"guid_spawn", source->GetSpawnId()},
        {"entry", source->GetEntry()},
        {"map_id", source->GetMapId()},
        {"instance_id", source->GetInstanceId()}}
        : json(nullptr);
    value["policy_revision"] = WojConfig::Instance().Behavior.revision;
    if (WojActorBinding const* binding = WojConfig::Instance().FindActor(policyActorId))
    {
        value["binding_generation"] = binding->generation;
        value["group_id"] = binding->groupId;
    }
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDynamicActorRejected(Creature const* actor)
{
    json value = Envelope("dynamic_actor_rejected", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["physical_spawn_id"] = actor ? actor->GetSpawnId() : 0;
    value["created_by_spell"] = actor ? actor->GetUInt32Value(UNIT_CREATED_BY_SPELL) : 0;
    value["policy_revision"] = WojConfig::Instance().Behavior.revision;
    LOG_ERROR("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDungeonDeathCast(Creature const* actor, char const* effectId,
                                         uint32_t spell, uint32_t expectedActorId,
                                         uint32_t castResult, bool exactSummons)
{
    json value = Envelope("dungeon_death_cast", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["effect_id"] = effectId ? effectId : "unknown";
    value["spell"] = spell;
    value["expected_actor_id"] = expectedActorId;
    value["cast_result"] = castResult;
    value["exact_summons"] = exactSummons;
    value["applied"] = castResult == static_cast<uint32_t>(SPELL_CAST_OK) && exactSummons;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteRoutineEvent(char const* event, Unit const* actor, uint32_t epoch,
                                     char const* profile, uint32_t pathId,
                                     std::string const& decisionId, char const* reason)
{
    json value = Envelope(event, actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["epoch"] = epoch;
    value["profile"] = profile;
    value["path_id"] = pathId ? json(pathId) : json(nullptr);
    value["decision_id"] = decisionId;
    value["reason"] = reason;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteSay(Unit const* actor, std::string const& decisionId, std::string const& text)
{
    json value = Envelope("say", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["decision_id"] = decisionId;
    value["text"] = text;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteFleeEmote(Unit const* actor, std::string const& decisionId, uint32_t epoch)
{
    json value = Envelope("flee_emote", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["broadcast_text_id"] = 1150;
    value["decision_id"] = decisionId;
    value["epoch"] = epoch;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteFleeStart(Unit const* actor, Unit const* target,
                                  std::string const& decisionId, uint32_t epoch,
                                  char const* mode, bool motionInstalled)
{
    json value = Envelope("flee_start", actor, target);
    value["server_boot_id"] = _serverBootId;
    value["decision_id"] = decisionId;
    value["epoch"] = epoch;
    value["mode"] = mode ? mode : "unknown";
    value["motion_installed"] = motionInstalled;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteStockTalk(Unit const* actor, std::string const& decisionId,
                                  uint32_t epoch, std::string const& triggerId,
                                  uint32_t generation, uint32_t textGroup)
{
    json value = Envelope("stock_talk", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["decision_id"] = decisionId;
    value["epoch"] = epoch;
    value["trigger_id"] = triggerId;
    value["generation"] = generation;
    value["text_group"] = textGroup;
    value["native_path"] = "CreatureTextMgr";
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteAggroEventEffect(Unit const* actor, Unit const* target,
                                         std::string const& eventId, std::string const& decisionId,
                                         uint32_t epoch, uint32_t generation, uint32_t effectIndex,
                                         char const* effect, uint32_t spell, uint32_t textGroup,
                                         char const* result, uint32_t coreResult, uint32_t attempt)
{
    json value = Envelope("aggro_event_effect", actor, target);
    value["server_boot_id"] = _serverBootId;
    value["event_id"] = eventId;
    value["decision_id"] = decisionId;
    value["trigger_id"] = "aggro_event_" + eventId;
    value["epoch"] = epoch;
    value["generation"] = generation;
    value["effect_index"] = effectIndex;
    value["attempt"] = attempt;
    value["effect"] = effect ? effect : "unknown";
    value["spell"] = spell;
    value["text_group"] = textGroup;
    value["result"] = result ? result : "unknown";
    value["core_result"] = coreResult;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteHealthPhase(Creature const* actor, WojHealthPhaseBehavior const& phase,
                                    char const* stage, uint32_t epoch, float healthPct,
                                    std::string const& decisionId, uint32_t generation,
                                    uint32_t decisionEpoch)
{
    json value = Envelope("health_phase", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["epoch"] = epoch;
    value["phase_id"] = phase.id;
    value["decision_id"] = decisionId;
    value["trigger_id"] = phase.id.empty() ? "" : "health_phase_" + phase.id;
    value["generation"] = generation;
    value["decision_epoch"] = decisionEpoch;
    value["stage"] = stage ? stage : "unknown";
    value["health_pct"] = healthPct;
    value["trigger_below_health_pct"] = phase.triggerBelowHealthPct;
    value["transition_spell"] = phase.transitionSpell;
    value["talk_group"] = phase.talkGroup;
    value["equipment_id"] = phase.equipmentId;
    value["dual_wield"] = phase.dualWield;
    value["equipment_delay_ms"] = phase.equipmentDelayMs;
    value["restore_combat_ms"] = phase.restoreCombatMs;
    value["tactical_delay_ms"] = phase.tacticalDelayMs;
    value["destination"] = {{"x", phase.destination.x}, {"y", phase.destination.y},
                              {"z", phase.destination.z}, {"o", phase.destination.o}};
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteHealingWaveProvenance(Unit const* actor, Unit const* target,
                                              uint32_t amount, uint32_t spell, std::string const& decisionId,
                                              uint32_t epoch, uint32_t generation)
{
    json value = Envelope("heal_provenance", actor, target);
    value["server_boot_id"] = _serverBootId;
    value["amount"] = amount;
    value["spell"] = spell;
    value["decision_id"] = decisionId;
    value["epoch"] = epoch;
    value["trigger_id"] = "spell_" + std::to_string(spell);
    value["generation"] = generation;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteTriggerTransition(Unit const* actor, uint32_t epoch,
                                          WojBehaviorEvent const& transition)
{
    json value = Envelope("trigger_transition", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["epoch"] = epoch;
    value["seq"] = transition.seq;
    value["trigger_id"] = transition.triggerId.empty() ? json(nullptr) : json(transition.triggerId);
    value["generation"] = transition.generation;
    value["kind"] = transition.kind;
    value["decision_id"] = transition.decisionId.empty() ? json(nullptr) : json(transition.decisionId);
    value["reason"] = transition.reason.empty() ? json(nullptr) : json(transition.reason);
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

bool WojCombatLog::StartSceneObservation(uint32_t durationMs, std::string& observationId)
{
    if (!_observationId.empty())
        StopSceneObservation("restart");

    _observationId = _serverBootId + "-observe-" + std::to_string(++_observationSequence);
    auto const now = std::chrono::steady_clock::now();
    _observationUntil = now + std::chrono::milliseconds(durationMs);
    _nextObservation = now;
    observationId = _observationId;

    json value = Envelope("scene_observe_start", nullptr, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["observation_id"] = _observationId;
    value["duration_ms"] = durationMs;
    value["expected"] = static_cast<uint32_t>(WojConfig::Instance().OwnedGuids.size());
    LOG_INFO("module.woj.combat", "{}", value.dump());
    return true;
}

bool WojCombatLog::StopSceneObservation(char const* reason)
{
    if (_observationId.empty())
        return false;

    json value = Envelope("scene_observe_end", nullptr, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["observation_id"] = _observationId;
    value["reason"] = reason;
    LOG_INFO("module.woj.combat", "{}", value.dump());
    _observationId.clear();
    return true;
}

bool WojCombatLog::IsOwned(Unit const* unit) const
{
    Creature const* creature = unit ? unit->ToCreature() : nullptr;
    return creature && WojConfig::Instance().OwnedGuids.count(
        WojRuntimeBinding::Instance().Resolve(creature));
}


bool WojCombatLog::IsTracked(Unit const* unit) const
{
    if (IsOwned(unit)
    )
        return true;
    if (!unit)
        return false;
    if (Creature const* creature = unit->ToCreature())
    {
        uint32_t const actorId = WojRuntimeBinding::Instance().Resolve(creature);
        if (WojConfig::Instance().IsRecordingOriginalActor(
                actorId, creature->GetEntry(), creature->GetMapId()))
            return true;
        return false;
    }
    return false;
}

void WojCombatLog::RecordOriginalJevRequest(Creature* creature)
{
    if (!creature || !creature->IsAlive())
        return;
    uint32_t const actorId = WojRuntimeBinding::Instance().Resolve(creature);
    WojConfig const& config = WojConfig::Instance();
    if (!config.IsRecordingOriginalActor(actorId, creature->GetEntry(), creature->GetMapId()))
        return;

    auto const now = std::chrono::steady_clock::now();
    WojActorKey const key = WojRuntimeKey(creature);
    {
        std::lock_guard<std::mutex> guard(_recordOriginalLock);
        auto& last = _lastOriginalObservation[key];
        if (last != std::chrono::steady_clock::time_point{} &&
            now - last < std::chrono::milliseconds(std::max<uint32_t>(500, config.TickMs)))
            return;
        last = now;
    }

    auto& shadow = gOriginalShadows[key];
    if (!shadow || shadow->PolicyActorId() != actorId)
        shadow = std::make_unique<WojAI>(creature, actorId, true);
    WojSnapshot snapshot;
    shadow->CaptureShadowSnapshot(snapshot);
    std::string const requestId = "shadow-" + std::to_string(key.rawGuid) + "-" +
        std::to_string(snapshot.seq);
    json value = Envelope("original_jev_request", creature, creature->GetVictim());
    value["server_boot_id"] = _serverBootId;
    value["control"] = "original";
    value["policy_revision"] = config.Behavior.revision;
    value["policy_actor_id"] = actorId;
    value["group_id"] = config.GroupId(actorId);
    value["request"] = json::parse(WojBridge::SerializeRequest(snapshot, requestId));
    value["label_status"] = "unlabelled_shadow_request";
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::ForgetOriginalJevRequest(Creature const* creature)
{
    if (!creature)
        return;
    std::lock_guard<std::mutex> guard(_recordOriginalLock);
    _lastOriginalObservation.erase(WojRuntimeKey(creature));
    gOriginalShadows.erase(WojRuntimeKey(creature));
}

void WojCombatLog::NoteOriginalShadowCast(Creature* creature, uint32_t spellId)
{
    if (!creature)
        return;
    auto const it = gOriginalShadows.find(WojRuntimeKey(creature));
    if (it != gOriginalShadows.end())
        it->second->ObserveShadowCast(spellId);
}

bool WojCombatLog::IsTrackedPair(Unit const* first, Unit const* second) const
{
    return IsTracked(first) || IsTracked(second);
}


void WojCombatLog::WriteMode(char const* previous, char const* current)
{
    json value = Envelope("mode", nullptr, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["previous"] = previous;
    value["current"] = current;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteSceneReset(uint32_t ready, uint32_t expected, uint32_t alive,
                                   uint32_t atHome, uint32_t outOfCombat)
{
    json value = Envelope("scene_reset", nullptr, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["ready"] = ready;
    value["expected"] = expected;
    value["alive"] = alive;
    value["at_home"] = atHome;
    value["out_of_combat"] = outOfCombat;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteSceneHealth(Unit const* actor, float requestedPct)
{
    json value = Envelope("scene_health", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["requested_pct"] = requestedPct;
    value["health"] = actor ? json(actor->GetHealth()) : json(nullptr);
    value["health_max"] = actor ? json(actor->GetMaxHealth()) : json(nullptr);
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDecisionTiming(Unit const* actor, WojAction const& action, char const* status,
                                       uint32_t freshnessLimitMs,
                                       std::chrono::steady_clock::time_point consumedAt)
{
    json value = Envelope("decision_timing", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["request_id"] = action.requestId;
    value["decision_id"] = action.decisionId;
    value["epoch"] = action.snapshotEpoch;
    value["snapshot_seq"] = action.snapshotSeq;
    value["policy_revision"] = action.policyRevision;
    value["binding_generation"] = action.bindingGeneration;
    value["status"] = status;
    value["capture_to_enqueue_ms"] = action.captureToEnqueueMs;
    value["age_at_send_ms"] = action.ageAtSendMs;
    value["http_roundtrip_ms"] = DurationMs(action.receivedAt, action.httpStartedAt);
    value["receive_to_consume_ms"] = DurationMs(consumedAt, action.receivedAt);
    value["total_age_ms"] = DurationMs(consumedAt, action.capturedAt);
    value["freshness_limit_ms"] = freshnessLimitMs;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDecisionTransport(WojTransportEvent const& event)
{
    char const* eventName = event.kind == WojTransportEventKind::Dispatch
        ? "decision_dispatch" : event.kind == WojTransportEventKind::Result
            ? "decision_transport" : "decision_terminal";
    json value = Envelope(eventName, nullptr, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["arena_id"] = event.arenaId.empty() ? json(nullptr) : json(event.arenaId);
    value["mode"] = "ours";
    value["src"] = TransportEntity(event);
    value["request_id"] = event.requestId;
    value["epoch"] = event.epoch;
    value["snapshot_seq"] = event.snapshotSeq;
    value["policy_revision"] = event.policyRevision;
    value["binding_generation"] = event.bindingGeneration;
    value["captured_t_ms"] = MonotonicAt(event.capturedAt);
    value["send_t_ms"] = MonotonicAt(event.sentAt);
    value["capture_to_enqueue_ms"] = event.captureToEnqueueMs;
    value["age_at_send_ms"] = event.ageAtSendMs;
    value["freshness_limit_ms"] = event.freshnessLimitMs;
    value["http_timeout_ms"] = event.httpTimeoutMs;
    if (event.kind != WojTransportEventKind::Dispatch)
    {
        value["received_t_ms"] = MonotonicAt(event.receivedAt);
        value["http_roundtrip_ms"] = DurationMs(event.receivedAt, event.sentAt);
        value["outcome"] = event.outcome;
        value["transport_error"] = event.transportError.empty()
            ? json(nullptr) : json(event.transportError);
        value["http_status"] = event.hasHttpStatus
            ? json(event.httpStatus) : json(nullptr);
    }
    if (event.kind == WojTransportEventKind::Terminal)
    {
        value["decision_id"] = event.decisionId.empty()
            ? json(nullptr) : json(event.decisionId);
        value["reason"] = event.terminalReason.empty()
            ? "unknown" : event.terminalReason;
    }
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteDecisionTransportOverflow(uint32_t dropped)
{
    json value = Envelope("decision_transport_overflow", nullptr, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["arena_id"] = nullptr;
    value["mode"] = "ours";
    value["dropped"] = dropped;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteActionRejected(Unit const* actor, std::string const& kind,
                                       std::string const& reason, std::string const& decisionId,
                                       std::string const& helpRequestId)
{
    json value = Envelope("action_rejected", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["kind"] = kind;
    value["reason"] = reason;
    value["decision_id"] = decisionId;
    if (!helpRequestId.empty())
        value["help_request_id"] = helpRequestId;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteActionApplied(Unit const* actor, std::string const& kind,
                                      std::string const& decisionId)
{
    json value = Envelope("action_applied", actor, nullptr);
    value["server_boot_id"] = _serverBootId;
    value["kind"] = kind;
    value["decision_id"] = decisionId;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteCastContinuation(Unit const* actor, Unit const* target, uint32_t spell,
                                         std::string const& decisionId)
{
    json value = Envelope("cast_continuation", actor, target);
    value["server_boot_id"] = _serverBootId;
    value["spell"] = spell;
    value["source_decision_id"] = decisionId;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteHelpRequest(Unit const* requester, Unit const* recipient, Unit const* target,
                                    uint32_t requesterEpoch, uint32_t recipientEpoch,
                                    std::string const& requestId, std::string const& decisionId, uint32_t ttlMs,
                                    bool recipientInCombat)
{
    json value = Envelope("help_request", requester, recipient);
    value["server_boot_id"] = _serverBootId;
    value["target"] = Entity(target);
    value["help_request_id"] = requestId;
    value["requester_epoch"] = requesterEpoch;
    value["recipient_epoch"] = recipientEpoch;
    value["request_decision_id"] = decisionId;
    value["decision_id"] = decisionId;
    value["ttl_ms"] = ttlMs;
    value["recipient_in_combat"] = recipientInCombat;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteHelpResponse(Unit const* recipient, Unit const* requester, Unit const* target,
                                     uint32_t requesterEpoch, uint32_t recipientEpoch,
                                     std::string const& requestId, std::string const& requestDecisionId,
                                     std::string const& decisionId, uint32_t latencyMs,
                                     bool recipientWasInCombat)
{
    json value = Envelope("help_response", recipient, requester);
    value["server_boot_id"] = _serverBootId;
    value["target"] = Entity(target);
    value["help_request_id"] = requestId;
    value["requester_epoch"] = requesterEpoch;
    value["recipient_epoch"] = recipientEpoch;
    value["request_decision_id"] = requestDecisionId;
    value["decision_id"] = decisionId;
    value["latency_ms"] = latencyMs;
    value["recipient_was_in_combat"] = recipientWasInCombat;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::WriteHelpExpired(Unit const* requester, Unit const* recipient, Unit const* target,
                                    uint32_t requesterEpoch, uint32_t recipientEpoch,
                                    std::string const& requestId, std::string const& requestDecisionId,
                                    std::string const& reason)
{
    json value = Envelope("help_expired", requester, recipient);
    value["server_boot_id"] = _serverBootId;
    value["target"] = Entity(target);
    value["help_request_id"] = requestId;
    value["requester_epoch"] = requesterEpoch;
    value["recipient_epoch"] = recipientEpoch;
    value["request_decision_id"] = requestDecisionId;
    value["decision_id"] = requestDecisionId;
    value["reason"] = reason;
    LOG_INFO("module.woj.combat", "{}", value.dump());
}

void WojCombatLog::Update()
{
    Map* map = sMapMgr->FindMap(WojConfig::Instance().SceneMapId, 0);
    if (!map)
        return;

    auto const now = std::chrono::steady_clock::now();
    if (!_observationId.empty() && now >= _observationUntil)
        StopSceneObservation("timeout");
    if (!_observationId.empty() && now >= _nextObservation)
    {
        _nextObservation = now + std::chrono::seconds(1);
        uint32_t const playersInMap = static_cast<uint32_t>(map->GetPlayers().getSize());
        for (uint32_t spawnId : WojConfig::Instance().OwnedGuids)
        {
            auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            Creature* actor = bounds.first == bounds.second ? nullptr : bounds.first->second;
            if (!actor)
                continue;

            float spawnX = 0.f, spawnY = 0.f, spawnZ = 0.f;
            actor->GetRespawnPosition(spawnX, spawnY, spawnZ);
            Position const& home = actor->GetHomePosition();
            float destinationX = 0.f, destinationY = 0.f, destinationZ = 0.f;
            bool const hasDestination = actor->GetMotionMaster()->GetDestination(
                destinationX, destinationY, destinationZ);
            uint32_t const pathId = actor->GetWaypointPath();

            json value = Envelope("scene_observation", actor, nullptr);
            value["server_boot_id"] = _serverBootId;
            value["observation_id"] = _observationId;
            value["epoch"] = WojConfig::Instance().Mode == WojMode::Ours
                ? WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(actor)) : 0;
            value["profile"] = RoutineProfile(actor);
            value["position"] = Point(actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ());
            value["spawn_origin"] = Point(spawnX, spawnY, spawnZ);
            value["dynamic_home"] = Point(home.GetPositionX(), home.GetPositionY(), home.GetPositionZ());
            value["destination"] = hasDestination
                ? Point(destinationX, destinationY, destinationZ) : json(nullptr);
            value["moving"] = actor->isMoving();
            value["walking"] = actor->IsWalking();
            value["motion"] = static_cast<uint32_t>(
                actor->GetMotionMaster()->GetCurrentMovementGeneratorType());
            value["path_id"] = pathId ? json(pathId) : json(nullptr);
            value["waypoint_id"] = pathId ? json(actor->GetCurrentWaypointID()) : json(nullptr);
            value["wander_radius"] = actor->GetDefaultMovementType() == RANDOM_MOTION_TYPE
                ? actor->GetWanderDistance() : 0.f;
            value["in_combat"] = actor->IsInCombat();
            value["players_in_map"] = playersInMap;
            LOG_INFO("module.woj.combat", "{}", value.dump());
        }
    }

    Creature* enemy = nullptr;

    for (uint32_t spawnId : WojConfig::Instance().OwnedGuids)
    {
        auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
        Creature* owned = bounds.first == bounds.second ? nullptr : bounds.first->second;
        if (!owned || !owned->IsAlive())
            continue;

        bool const fleeing = owned->HasUnitState(UNIT_STATE_FLEEING);
        uint64_t const raw = owned->GetGUID().GetRawValue();
        auto const previous = gFleeing.find(raw);
        if (previous != gFleeing.end() && previous->second != fleeing)
            WriteCombatEvent(fleeing ? "flee_start" : "flee_end", owned, enemy);
        gFleeing[raw] = fleeing;

        if (enemy && enemy->IsAlive() && !gSeenInRange.count(spawnId))
        {
            float const distance = owned->GetDistance(enemy);
            if (distance <= ENEMY_RANGE_YARDS)
            {
                json value = Envelope("enemy_in_range", owned, enemy);
                value["server_boot_id"] = _serverBootId;
                value["distance"] = distance;
                LOG_INFO("module.woj.combat", "{}", value.dump());
                gSeenInRange.insert(spawnId);
            }
        }
    }
}

class WojUnitCombatScript : public UnitScript
{
public:
    WojUnitCombatScript() : UnitScript("WojUnitCombatScript") { }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        (void)attacker;
        (void)damage;
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (WojCombatLog::Instance().IsTrackedPair(attacker, victim))
            // OnDamage не несёт SpellInfo. При нескольких ударах за тик
            // угадывать соответствие по последнему Modify* было бы ложью.
            WojCombatLog::Instance().WriteCombatEvent("damage", attacker, victim, damage, true);
    }

    void OnHeal(Unit* healer, Unit* receiver, uint32& gain) override
    {
        // OnHeal получает фактический прирост уже после ModifyHealth. Exact
        // 913 provenance ставится только в предшествующем SpellInfo hook.
        Creature* caster = healer ? healer->ToCreature() : nullptr;
        if (caster)
            if (WojAI* ai = dynamic_cast<WojAI*>(caster->AI()))
                ai->ConsumeHealingWaveProvenance(healer, receiver, gain);
        if (WojCombatLog::Instance().IsTrackedPair(healer, receiver))
            WojCombatLog::Instance().WriteCombatEvent("heal", healer, receiver, gain, true);
    }

    void ModifyHealReceived(Unit* first, Unit* second, uint32& /*gain*/, SpellInfo const* spell) override
    {
        if (!spell)
            return;
        // Direct and periodic core paths invert the nominal target/healer
        // callback arguments. Ask both possible owned casters instead.
        for (Unit* unit : {first, second})
            if (Creature* creature = unit ? unit->ToCreature() : nullptr)
                if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
                    ai->ArmHealingWaveProvenance(first, second, spell);
    }

    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        if (WojCombatLog::Instance().IsTrackedPair(unit, victim))
            WojCombatLog::Instance().WriteCombatEvent("enter_combat", unit, victim);
    }

    void OnUnitExitCombat(Unit* unit) override
    {
        if (WojCombatLog::Instance().IsTracked(unit))
            WojCombatLog::Instance().WriteCombatEvent("exit_combat", unit, nullptr);
    }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        // Map workers can run concurrently; WojHelp serializes its bounded
        // value store.  The composite target key prevents a death in another
        // copy of this map from cancelling this instance's help request.
        WojWriteHelpExpired(WojHelp::Instance().CancelTarget(WojRuntimeKey(unit)), "target_dead");
        bool const tracked = WojCombatLog::Instance().IsTracked(unit) ||
            WojCombatLog::Instance().IsTrackedPair(unit, killer);
        if (tracked)
            WojCombatLog::Instance().WriteCombatEvent("death", killer, unit);
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        uint32_t const policyActorId = WojRuntimeBinding::Instance().Resolve(creature);
        if (creature && WojConfig::Instance().IsJevActor(
                policyActorId, creature->GetEntry(), creature->GetMapId()))
        {
            for (WojDeathEffectBehavior const& effect :
                 WojConfig::Instance().DeathEffectsForActor(policyActorId))
            {
                if (effect.entry != creature->GetEntry())
                    continue;
                if (effect.kind == WojDeathEffectKind::CastSpell)
                {
                    bool const scoped = WojRuntimeBinding::Instance().BeginSummonPermit(
                        creature, effect.spell, effect.expectedSummonActorId,
                        effect.expectedSummonCount);
                    if (!scoped)
                    {
                        LOG_ERROR("module", "mod-world-of-jevs: refused death cast {} for actor {} because exact summon permit could not be armed",
                            effect.spell, policyActorId);
                        continue;
                    }
                    SpellCastResult const castResult = creature->CastSpell(creature, effect.spell, true);
                    bool const exact = WojRuntimeBinding::Instance().FinishSummonPermit(
                        creature, effect.spell, effect.expectedSummonActorId,
                        effect.expectedSummonCount);
                    WojCombatLog::Instance().WriteDungeonDeathCast(
                        creature, effect.id.c_str(), effect.spell, effect.expectedSummonActorId,
                        static_cast<uint32_t>(castResult), exact);
                    if (castResult != SPELL_CAST_OK || !exact)
                        LOG_ERROR("module", "mod-world-of-jevs: death cast {} for actor {} failed result={} exact_summons={}",
                            effect.spell, policyActorId, static_cast<uint32_t>(castResult), exact);
                    continue;
                }
                GameObject* object = nullptr;
                uint32_t objectMatches = 0;
                auto const bounds = creature->GetMap()->GetGameObjectBySpawnIdStore().equal_range(
                    effect.gameObjectSpawnId);
                for (auto it = bounds.first; it != bounds.second; ++it)
                    if (it->second && it->second->GetEntry() == effect.gameObjectEntry)
                    {
                        object = it->second;
                        ++objectMatches;
                    }
                InstanceScript* instance = creature->GetInstanceScript();
                bool const exactObject = objectMatches == 1;
                bool const correctInitial = exactObject &&
                    static_cast<uint32_t>(object->GetGoState()) == effect.initialObjectState;
                auto readInstanceValue = [instance, &effect](uint32_t& value)
                {
                    if (!instance || !effect.setInstanceData)
                        return false;
                    std::istringstream save(instance->GetSaveData());
                    std::string token;
                    for (uint32_t index = 0; index <= effect.instanceSaveTokenIndex; ++index)
                        if (!(save >> token))
                            return false;
                    auto const result = std::from_chars(token.data(), token.data() + token.size(), value);
                    return result.ec == std::errc() && result.ptr == token.data() + token.size();
                };
                uint32_t preflightInstanceData = std::numeric_limits<uint32_t>::max();
                bool const exactInstance = !effect.setInstanceData ||
                    (readInstanceValue(preflightInstanceData) &&
                     preflightInstanceData == effect.initialInstanceDataValue);
                // Never partially mutate the door when the required instance
                // script/save slot is unavailable or malformed.
                if (correctInitial && exactInstance)
                    object->UseDoorOrButton(0, false, creature);
                bool const objectTransitioned = correctInitial && exactInstance &&
                    static_cast<uint32_t>(object->GetGoState()) == effect.finalObjectState;
                if (objectTransitioned && instance && effect.setInstanceData)
                    instance->SetData(effect.instanceDataIndex, effect.instanceDataValue);
                uint32_t const objectState = object
                    ? static_cast<uint32_t>(object->GetGoState()) : std::numeric_limits<uint32_t>::max();
                uint32_t instanceData = std::numeric_limits<uint32_t>::max();
                if (effect.setInstanceData)
                    readInstanceValue(instanceData);
                bool const applied = objectTransitioned &&
                    (!effect.setInstanceData || (instance && instanceData == effect.instanceDataValue));
                WojCombatLog::Instance().WriteDungeonProgress(creature, effect.id.c_str(), applied,
                    object ? object->GetSpawnId() : 0, object ? object->GetEntry() : 0,
                    objectState, instanceData);
            }
        }
        if (!tracked)
            return;
    }
};

class WojAllSpellCombatScript : public AllSpellScript
{
public:
    WojAllSpellCombatScript() : AllSpellScript("WojAllSpellCombatScript") { }

    struct PendingCannonItemCast
    {
        uint64_t playerRawGuid = 0;
        uint64_t itemRawGuid = 0;
        uint64_t targetRawGuid = 0;
        uint32_t instanceId = 0;
        std::chrono::steady_clock::time_point capturedAt;
    };

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& result) override
    {
        // CheckCast still precedes the actual effect. Retain only scalar
        // identity here; the completed OnSpellCast below is the witness.
        if (result == SPELL_CAST_OK && spell && spell->GetSpellInfo() &&
            spell->GetSpellInfo()->Id == 6250 && spell->m_CastItem)
            if (WorldObject* caster = spell->GetCaster())
                if (caster->ToPlayer() && caster->GetMapId() == 36)
                    if (GameObject* target = spell->m_targets.GetGOTarget())
                        if (target->GetEntry() == 16398 && target->GetSpawnId() == 26205 &&
                            target->GetMap() == caster->GetMap() &&
                            target->GetInstanceId() == caster->GetInstanceId())
                        {
                            std::lock_guard<std::mutex> guard(_cannonItemCastLock);
                            auto const now = std::chrono::steady_clock::now();
                            for (auto it = _pendingCannonItemCasts.begin();
                                 it != _pendingCannonItemCasts.end();)
                                if (now - it->second.capturedAt > std::chrono::seconds(15))
                                    it = _pendingCannonItemCasts.erase(it);
                                else
                                    ++it;
                            if (_pendingCannonItemCasts.size() < 32 ||
                                _pendingCannonItemCasts.count(spell))
                                _pendingCannonItemCasts[spell] = {caster->GetGUID().GetRawValue(),
                                    spell->m_CastItem->GetGUID().GetRawValue(),
                                    target->GetGUID().GetRawValue(), caster->GetInstanceId(), now};
                        }
        if (spell && spell->GetSpellInfo())
            if (WorldObject* caster = spell->GetCaster())
                if (caster->ToPlayer())
                    if (GameObject* target = spell->m_targets.GetGOTarget())
                        if (caster->GetMapId() == 36 &&
                            ((target->GetSpawnId() == 26203 && target->GetEntry() == 17155) ||
                             (target->GetSpawnId() == 26205 && target->GetEntry() == 16398)))
                            LOG_INFO("module",
                                "cannon_go_spell_check instance={} player_raw={} go_raw={} spell={} result={}",
                                caster->GetInstanceId(), caster->GetGUID().GetRawValue(),
                                target->GetGUID().GetRawValue(), spell->GetSpellInfo()->Id,
                                static_cast<uint32_t>(result));
        if (result != SPELL_CAST_OK || !spell || !spell->GetSpellInfo())
            return;
        WorldObject* caster = spell->GetCaster();
        Creature* source = caster ? caster->ToCreature() : nullptr;
        (void)source;
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (caster && spellInfo)
            WojCombatLog::Instance().NoteOriginalShadowCast(caster->ToCreature(), spellInfo->Id);
        PendingCannonItemCast pending;
        bool captured = false;
        if (spell)
        {
            std::lock_guard<std::mutex> guard(_cannonItemCastLock);
            auto const it = _pendingCannonItemCasts.find(spell);
            if (it != _pendingCannonItemCasts.end())
            {
                pending = it->second;
                captured = true;
                _pendingCannonItemCasts.erase(it);
            }
        }
        GameObject* goTarget = spell ? spell->m_targets.GetGOTarget() : nullptr;
        if (captured && caster && spellInfo && spellInfo->Id == 6250 && goTarget &&
            caster->GetGUID().GetRawValue() == pending.playerRawGuid &&
            caster->GetMapId() == 36 && caster->GetInstanceId() == pending.instanceId &&
            goTarget->GetGUID().GetRawValue() == pending.targetRawGuid &&
            std::chrono::steady_clock::now() - pending.capturedAt <= std::chrono::seconds(15))
        {
            WojRuntimeBinding::Instance().ObserveGameObjectItemCast(
                spell, caster, spellInfo, pending.itemRawGuid);
        }
        if (!caster || !spellInfo || !WojCombatLog::Instance().IsTracked(caster))
            return;
        Unit* target = spell ? spell->GetOriginalTarget() : nullptr;
        WojCombatLog::Instance().WriteCombatEvent("cast", caster, target, 0, false, spellInfo->Id);
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo,
                           bool /*bySelf*/) override
    {
        if (spell)
        {
            std::lock_guard<std::mutex> guard(_cannonItemCastLock);
            _pendingCannonItemCasts.erase(spell);
        }
        Creature* creature = caster ? caster->ToCreature() : nullptr;
        if (!creature || !spellInfo)
            return;
        if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
            ai->NotifySpellCastCancelled(spellInfo->Id);
    }

private:
    std::mutex _cannonItemCastLock;
    std::unordered_map<Spell const*, PendingCannonItemCast> _pendingCannonItemCasts;
};

class WojArenaLevelScript : public AllCreatureScript
{
public:
    WojArenaLevelScript() : AllCreatureScript("WojArenaLevelScript") { }

    void OnCreatureAddWorld(Creature* creature) override
    {
        if (creature && creature->GetMapId() == 36 && !creature->GetSpawnId() &&
            creature->GetEntry() == 4417)
        {
            TempSummon* summon = creature->IsSummon() ? creature->ToTempSummon() : nullptr;
            GameObject* sourceGo = summon ? summon->GetSummonerGameObject() : nullptr;
            LOG_INFO("module",
                "cannon_report_use_child_birth map={} instance={} raw={} source_go={} "
                "source_entry={} x={} y={} z={}",
                creature->GetMapId(), creature->GetInstanceId(),
                creature->GetGUID().GetRawValue(),
                sourceGo ? sourceGo->GetSpawnId() : 0,
                sourceGo ? sourceGo->GetEntry() : 0,
                creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ());
        }
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        (void)creature;
    }

};

void AddWojCombatLogScripts()
{
    new WojUnitCombatScript();
    new WojAllSpellCombatScript();
    new WojArenaLevelScript();
}
