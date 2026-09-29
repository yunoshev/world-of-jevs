#ifndef WOJ_COMBAT_LOG_H
#define WOJ_COMBAT_LOG_H

#include "ObjectGuid.h"
#include "Position.h"
#include "WojConfig.h"
#include "WojTypes.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Map;
class Creature;
class GameObject;
class Player;
class TempSummon;
class Unit;
struct WojAction;
struct WojBehaviorEvent;
struct WojTransportEvent;
struct WojHealthPhaseBehavior;

class WojCombatLog
{
public:
    static WojCombatLog& Instance();

    void Initialize();
    void Update();


    bool IsOwned(Unit const* unit) const;
    bool IsTracked(Unit const* unit) const;
    bool IsTrackedPair(Unit const* first, Unit const* second) const;
    void RecordOriginalJevRequest(Creature* creature);
    void ForgetOriginalJevRequest(Creature const* creature);
    void NoteOriginalShadowCast(Creature* creature, uint32_t spellId);
    void WriteCreatureLifecycle(Creature const* creature, char const* stage);
    bool ClaimAiInitDespawnRoll(Creature const* creature);
    void ResetAiInitDespawnRoll(Creature const* creature);
    void WriteAiInitDespawnRoll(Creature const* creature, uint32_t chancePct,
                                uint32_t delayMs, bool selected, uint32_t policyRevision);

    void WriteCombatEvent(char const* event, Unit const* src, Unit const* dst,
                          uint32_t amount = 0, bool hasAmount = false,
                          uint32_t spell = 0, bool melee = false);
    void WriteMode(char const* previous, char const* current);
    void WriteSceneReset(uint32_t ready, uint32_t expected, uint32_t alive,
                         uint32_t atHome, uint32_t outOfCombat);
    void WriteSceneHealth(Unit const* actor, float requestedPct);
    void WriteDecisionTiming(Unit const* actor, WojAction const& action, char const* status,
                             uint32_t freshnessLimitMs,
                             std::chrono::steady_clock::time_point consumedAt);
    void WriteDecisionTransport(WojTransportEvent const& event);
    void WriteDecisionTransportOverflow(uint32_t dropped);
    void WriteRoutineEvent(char const* event, Unit const* actor, uint32_t epoch,
                           char const* profile, uint32_t pathId,
                           std::string const& decisionId, char const* reason);
    void WriteSay(Unit const* actor, std::string const& decisionId, std::string const& text);
    void WriteFleeEmote(Unit const* actor, std::string const& decisionId, uint32_t epoch);
    void WriteFleeStart(Unit const* actor, Unit const* target,
                        std::string const& decisionId, uint32_t epoch,
                        char const* mode, bool motionInstalled);
    void WriteStockTalk(Unit const* actor, std::string const& decisionId, uint32_t epoch,
                        std::string const& triggerId, uint32_t generation, uint32_t textGroup);
    void WriteAggroEventEffect(Unit const* actor, Unit const* target,
                               std::string const& eventId, std::string const& decisionId,
                               uint32_t epoch, uint32_t generation, uint32_t effectIndex,
                               char const* effect, uint32_t spell, uint32_t textGroup,
                               char const* result, uint32_t coreResult = 0,
                               uint32_t attempt = 1);
    void WriteHealingWaveProvenance(Unit const* actor, Unit const* target, uint32_t amount, uint32_t spell,
                                    std::string const& decisionId, uint32_t epoch,
                                    uint32_t generation);
    void WriteTriggerTransition(Unit const* actor, uint32_t epoch, WojBehaviorEvent const& transition);
    void WriteDecisionActivation(Unit const* actor, bool active, char const* reason,
                                 float radiusYards, Unit const* nearbyPlayer);
    void WriteDungeonProgress(Creature const* actor, char const* encounter, bool applied,
                              uint32_t objectSpawnId, uint32_t objectEntry,
                              uint32_t objectState, uint32_t instanceData);
    void WriteDynamicActorBinding(Creature const* actor, Creature const* source,
                                  uint32_t policyActorId, uint32_t createdBySpell);
    void WriteGoDynamicActorBinding(Creature const* actor, GameObject const* source,
                                    uint32_t policyActorId, uint32_t createdBySpell,
                                    uint32_t summonIndex, bool reportUse);
    void WriteDynamicActorRejected(Creature const* actor);
    void WriteDungeonDeathCast(Creature const* actor, char const* effectId,
                               uint32_t spell, uint32_t expectedActorId,
                               uint32_t castResult, bool exactSummons);
    void WriteHealthPhase(Creature const* actor, WojHealthPhaseBehavior const& phase,
                          char const* stage, uint32_t epoch, float healthPct,
                          std::string const& decisionId = "", uint32_t generation = 0,
                          uint32_t decisionEpoch = 0);

    bool StartSceneObservation(uint32_t durationMs, std::string& observationId);
    bool StopSceneObservation(char const* reason = "stop");

    // Задача 5 вызывает этот метод при отказе исполнителя. Отдельный JSONL,
    // а не Server.log, нужен задаче 7 для честного временного окна арены.
    void WriteActionRejected(Unit const* actor, std::string const& kind,
                             std::string const& reason, std::string const& decisionId,
                             std::string const& helpRequestId = "");
    void WriteActionApplied(Unit const* actor, std::string const& kind,
                            std::string const& decisionId);
    void WriteCastContinuation(Unit const* actor, Unit const* target, uint32_t spell,
                               std::string const& decisionId);
    void WriteHelpRequest(Unit const* requester, Unit const* recipient, Unit const* target,
                          uint32_t requesterEpoch, uint32_t recipientEpoch,
                          std::string const& requestId, std::string const& decisionId, uint32_t ttlMs,
                          bool recipientInCombat);
    void WriteHelpResponse(Unit const* recipient, Unit const* requester, Unit const* target,
                           uint32_t requesterEpoch, uint32_t recipientEpoch,
                           std::string const& requestId, std::string const& requestDecisionId,
                           std::string const& decisionId, uint32_t latencyMs,
                           bool recipientWasInCombat);
    void WriteHelpExpired(Unit const* requester, Unit const* recipient, Unit const* target,
                          uint32_t requesterEpoch, uint32_t recipientEpoch,
                          std::string const& requestId, std::string const& requestDecisionId,
                          std::string const& reason);


private:
    WojCombatLog() = default;
    std::string _serverBootId;
    std::string _observationId;
    uint64_t _observationSequence = 0;
    std::chrono::steady_clock::time_point _observationUntil{};
    std::chrono::steady_clock::time_point _nextObservation{};
    mutable std::mutex _recordOriginalLock;
    std::unordered_map<WojActorKey, std::chrono::steady_clock::time_point,
                       WojActorKeyHash> _lastOriginalObservation;
    mutable std::mutex _lifecycleLock;
    std::unordered_set<uint64_t> _lifecycleSeen;
    std::unordered_set<uint64_t> _aiInitRolled;
};

void AddWojCombatLogScripts();

#endif
