#ifndef WOJ_AI_H
#define WOJ_AI_H
#include "CreatureAI.h"
#include "WojConfig.h"
#include "WojTypes.h"
#include "ObjectGuid.h"
#include <chrono>
#include <deque>
#include <random>
#include <unordered_map>
#include <unordered_set>

class Player;

// Marker used by hot-reload verification. A rejected runtime summon is not
// equivalent to the original/native AI even though neither is WojAI.
class WojRejectedRuntimeAI
{
public:
    virtual ~WojRejectedRuntimeAI() = default;
};

class WojAI : public CreatureAI
{
public:
    explicit WojAI(Creature* creature, uint32_t policyActorId, bool shadowOnly = false);
    ~WojAI() override;
    uint32_t PolicyActorId() const { return _policyActorId; }
    // Builds the ordinary Jev wire snapshot while native CreatureAI remains
    // installed. Never executes an action or queues a provider decision.
    void CaptureShadowSnapshot(WojSnapshot& out);
    void ObserveShadowCast(uint32_t spellId);

    // Capture a stock-eligible proximity event, but leave ATTACK to Jev.
    void MoveInLineOfSight(Unit* who) override;
    void UpdateAI(uint32 diff) override;
    void JustEngagedWith(Unit* who) override;
    void JustDied(Unit* killer) override;
    void OnDespawn() override;
    void EnterEvadeMode(EvadeReason why) override;
    void JustReachedHome() override;
    void MovementInform(uint32 type, uint32 id) override;
    void OnSpellCast(SpellInfo const* spell) override;
    // ModifyHealReceived несёт SpellInfo; порядок двух Unit* различается
    // между direct и periodic путями core, поэтому оба проверяются ниже.
    void ArmHealingWaveProvenance(Unit* first, Unit* second, SpellInfo const* spell);
    bool ConsumeHealingWaveProvenance(Unit* healer, Unit* receiver, uint32_t gain);
    void PrepareForReplacement(char const* reason);
    void ReloadBehaviorPolicy();
    void NotifySpellCastCancelled(uint32_t spellId);
    // I2 correction: this comment used to claim the same WojAI object
    // survives death and resurrection, on the theory that Creature::Respawn
    // only builds a fresh AI when the spawn has an id2 and our murloc has a
    // single entry. That theory only holds in compatibility-mode respawn.
    // Guid 89965 is confirmed dynamic-mode (see the comment on the
    // WojKeepAlive::Apply() call site in WojScriptLoader.cpp for the
    // evidence), so Creature::Respawn's in-place path is never taken at
    // all: Creature::RemoveCorpse() destroys this Creature on death, and
    // Map::ProcessCreatureRespawn() later builds a brand new one, for which
    // WojCreatureScript::GetAI() constructs a brand new WojAI - not the
    // same object.
    //
    // JustRespawned() still fires once on that new object (the core's
    // Creature constructor unconditionally sets TriggerJustRespawned, so
    // Creature::Update() calls it on the next tick regardless of respawn
    // mode - see Creature.cpp), but by then _lastActions/_sinceAction/
    // _sinceDecision are already at the fresh object's own zero-initialised
    // defaults: there is no previous life left on this instance to clear.
    // The override earns its keep only if a future config ever forces this
    // spawn into compatibility mode, where the object really would persist
    // across death; left in so that day does not regress silently.
    // Reset() is left alone: it also runs on evade, where the history is
    // exactly what we want to keep.
    void JustRespawned() override;

private:
    Player* FindNearbyDecisionPlayer(float radiusYards) const;
    bool RefreshDecisionActivation(uint32 diff, WojActorKey const& actor, uint32_t epoch);
    void BuildSnapshot(WojSnapshot& out);
    uint32_t RollRange(uint32_t minimum, uint32_t maximum);
    void ApplyAction(WojAction const& action, bool continuation = false);
    bool ContinueJevCast();
    bool IsJevCastContinuationEnabled(uint32_t spell) const;
    void ClearJevCastContinuation();
    void AttackStartOwned(Unit* target);
    bool CanStartObservedAggro(Unit* target) const;
    bool CanAssistFormation(Unit* target) const;
    void ContinueAttack();
    float PreferredCombatRangeYards() const;
    bool CancelOwnFlee();
    bool RememberOneShot(std::string const& decisionId);
    void PushAction(std::string marker);
    void RejectAction(WojAction const& action, char const* kind, std::string const& reason,
                      std::string const& helpRequestId = "", bool recordTrigger = true);
    // M4 (round 3): shared by both places a NONE streak can legitimately
    // end - see the call sites in ApplyAction and UpdateAI.
    void ResetSuppressedNone();
    bool ResumeRoutine(WojAction const& action);
    void StopRoutine(char const* reason);
    void RestoreRoutineMotion();
    void SuspendRoutineForCombat();
    void ReleaseRoutineMoveBlock();
    void ReleaseRoutineSuspension();
    WojPoint const& MovementAnchor() const;
    struct TriggerRuntime
    {
        WojBehaviorTrigger view;
        uint32_t spell = 0;
        uint32_t firstMinMs = 0;
        uint32_t firstMaxMs = 0;
        uint32_t repeatMinMs = 0;
        uint32_t repeatMaxMs = 0;
        bool ticksOutOfCombat = false;
        bool timerArmed = false;
        bool timerPaused = false;
        bool eventDriven = false;
        bool oncePerEpoch = false;
        // SmartAI FRIENDLY_HEALTH_PCT row 1065/913 has NO_REPEAT: after a
        // successful event cast it stays consumed until the next AI epoch.
        bool eventConsumedThisEpoch = false;
        bool chancePassed = true;
        uint32_t chancePct = 100;
        bool ambientTalk = false;
        std::chrono::steady_clock::time_point deadline{};
    };
    struct PendingCast
    {
        bool active = false;
        uint32_t epoch = 0;
        uint32_t spell = 0;
        uint32_t generation = 0;
        uint32_t expectedSummonActorId = 0;
        uint32_t expectedSummonCount = 0;
        std::string decisionId;
        std::string target;
        std::string marker;
        bool permitsContinuation = false;
        std::chrono::steady_clock::time_point attemptedAt{};
        std::chrono::steady_clock::time_point expiresAt{};
    };
    struct JevCastContinuation
    {
        bool active = false;
        uint32_t epoch = 0;
        uint32_t spell = 0;
        uint32_t generation = 0;
        std::string target;
        std::string decisionId;
        std::string marker;
    };
    struct PendingHeal
    {
        bool active = false;
        uint32_t epoch = 0;
        uint32_t spell = 0;
        uint32_t generation = 0;
        std::string decisionId;
        std::string target;
    };
    void InitializeBehavior(char const* reason);
    void ApplyAiInitDespawn();
    void RefreshBehavior(WojSnapshot& snapshot);
    void AppendBehaviorEvent(std::string const& triggerId, uint32_t generation,
                             char const* kind, std::string const& decisionId = "",
                             std::string const& reason = "");
    TriggerRuntime* FindTrigger(std::string const& id);
    TriggerRuntime const* FindTrigger(std::string const& id) const;
    void SetTriggerState(TriggerRuntime& trigger, char const* status,
                         char const* blockedBy, char const* eventKind = nullptr,
                         std::string const& decisionId = "", std::string const& reason = "");
    void ArmTimedTrigger(TriggerRuntime& trigger, bool initial, char const* reason);
    bool ValidateTriggeredAction(WojAction const& action, Unit const* target,
                                 TriggerRuntime*& trigger, std::string& reason);
    void FailPendingCast(char const* kind, char const* reason);
    void SucceedPendingCast(uint32_t spellId);
    bool IsAggroTalker() const;
    bool IsAmbientTalker() const;
    void AttemptAggroTalk();
    bool UpdateHealthPhases();
    void UpdateHealthEvents();
    void UpdateAggroEvents();
    void AdvanceAggroEventContinuation();
    void CancelAggroEventContinuation(char const* reason);
    void StartHealthPhase(WojHealthPhaseBehavior const& phase,
                          std::string const& decisionId, uint32_t generation,
                          uint32_t decisionEpoch);
    void RestoreHealthPhaseBaseline(char const* reason);

    uint32_t _policyActorId = 0;
    bool _shadowOnly = false;
    std::mt19937 _shadowRandom;
    uint32_t _shadowPolicyRevision = 0;
    uint32 _sinceDecision = 0;   // since we last asked
    uint32 _sinceAction = 0;     // since we last actually did something
    uint64 _seq = 0;
    WojPoint _spawnOrigin;
    WojPoint _combatAnchor;
    bool _hasCombatAnchor = false;
    WojRoutineProfile _routineProfile = WojRoutineProfile::Idle;
    float _routineWanderRadius = 0.f;
    uint32_t _routinePathId = 0;
    uint32_t _routinePointCount = 0;
    bool _routineInstalled = false;
    uint32_t _routineAuthorizedEpoch = 0;
    bool _wasInCombat = false;
    bool _routineSuspendedForCombat = false;
    bool _ownsRoutineMoveBlock = false;
    bool _fleeEmoteSentThisCombat = false;
    bool _fleeConsumedThisEpoch = false;
    std::deque<std::string> _lastActions;
    ObjectGuid _attackContinuation;
    ObjectGuid _observedAggroTarget;
    std::chrono::steady_clock::time_point _observedAggroUntil{};
    float _appliedCombatRangeYards = 0.0f;
    bool _ownsTimedFlee = false;
    bool _ownsAssistFlight = false;
    std::chrono::steady_clock::time_point _ownTimedFleeUntil{};
    std::chrono::steady_clock::time_point _assistFlightUntil{};
    std::deque<std::string> _oneShotOrder;
    std::unordered_set<std::string> _oneShotDecisions;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> _lastSuccessfulCast;
    std::vector<TriggerRuntime> _triggers;
    std::deque<WojBehaviorEvent> _behaviorEvents;
    uint64_t _behaviorSeq = 0;
    uint32_t _behaviorEpoch = 0;
    PendingCast _pendingCast;
    JevCastContinuation _jevCastContinuation;
    std::chrono::steady_clock::time_point _nextSecondaryYieldAt{};
    std::chrono::steady_clock::time_point _secondaryDecisionWaitUntil{};
    PendingHeal _pendingHeal;
    struct HealthPhaseRuntime
    {
        bool active = false;
        bool arrived = false;
        bool equipmentApplied = false;
        WojHealthPhaseBehavior behavior;
        std::string decisionId;
        uint32_t generation = 0;
        uint32_t decisionEpoch = 0;
        ObjectGuid victim;
        std::chrono::steady_clock::time_point equipmentAt{};
        std::chrono::steady_clock::time_point restoreAt{};
    };
    HealthPhaseRuntime _healthPhase;
    std::unordered_set<std::string> _consumedHealthPhases;
    std::unordered_set<std::string> _consumedHealthEvents;
    std::unordered_set<std::string> _consumedAggroEvents;
    ObjectGuid _aggroInvoker;
    bool _aggroEventArmed = false;
    struct AggroEventContinuation
    {
        bool active = false;
        std::string eventId;
        std::string decisionId;
        uint32_t epoch = 0;
        uint32_t generation = 0;
        uint32_t attempt = 0;
        ObjectGuid invoker;
        std::chrono::steady_clock::time_point retryAt{};
    };
    AggroEventContinuation _aggroEventContinuation;
    std::chrono::steady_clock::time_point _nextHealthPhaseAt{};
    int8_t _baselineEquipmentId = 0;
    bool _baselineDualWield = false;
    bool _aggroTalkAttemptedThisCombat = false;
    std::chrono::steady_clock::time_point _lastBudgetWarning{};
    uint32_t _activationProbeElapsedMs = 0;
    bool _activationInitialized = false;
    bool _decisionActive = false;
    bool _nearbyDecisionPlayer = false;
    bool _lifecycleNeedsRespawnRoll = false;

    // I3: rate limiting for the NONE log line only (WojAI.cpp:ApplyAction).
    // _inNoneStreak remembers whether the previous action was also a miss,
    // so a fresh outage can be told apart from a stale cooldown left over
    // from an earlier one. _suppressedNoneCount counts misses swallowed
    // since the last line that actually printed, so that count can be named
    // instead of silently dropped.
    bool _inNoneStreak = false;
    uint32 _suppressedNoneCount = 0;
    std::chrono::steady_clock::time_point _lastNoneLog{};
};
#endif
