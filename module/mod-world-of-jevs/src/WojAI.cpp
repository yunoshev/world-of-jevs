#include "WojAI.h"
#include "WojCombatLog.h"
#include "WojConfig.h"
#include "WojBridge.h"
#include "WojHelp.h"
#include "WojRegistry.h"
#include "WojRuntimeBinding.h"
#include "WojRuntimeIdentity.h"
#include "CellImpl.h"
#include "ChatTextBuilder.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "CreatureTextMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Language.h"
#include "Map.h"
#include "MovementGenerator.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "WaypointMgr.h"
#include "World.h"
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <list>
#include <algorithm>
#include <unordered_set>
#include <utility>

namespace
{
    // Budget: the module's own bookkeeping (building the snapshot, reading
    // and writing the registry) may not spend more than this per NPC per
    // tick. ApplyAction is deliberately excluded from this measurement:
    // MovePoint paths across the navmesh (hundreds of microseconds) and Say
    // broadcasts a packet to everyone in earshot, so this is no longer "no
    // I/O" the moment an action exists to apply.
    constexpr int64 TICK_BUDGET_US = 250;
    constexpr float NEIGHBOR_SEARCH_YARDS = 40.0f;
    constexpr float ALLY_SEARCH_YARDS = 30.0f;
    // Bound the exact sight event across one asynchronous Jev decision.
    constexpr auto OBSERVED_AGGRO_TTL = std::chrono::seconds(5);
    constexpr size_t MAX_ATTACKERS = 5;
    constexpr size_t MAX_ENEMIES = 5;
    constexpr size_t MAX_ALLIES = 8;
    constexpr size_t MAX_HEAL_ALLIES = 13;
    constexpr size_t MAX_ONE_SHOT_DECISIONS = 128;
    constexpr uint32_t FLEE_DURATION_MS = 10000;
    constexpr uint32_t ASSIST_MOVE_POINT_ID = 0x574F4A;
    // Do not spam the log more than once a minute per NPC for the same
    // recurring overrun.
    constexpr int64 BUDGET_WARNING_COOLDOWN_SECONDS = 60;
    // I3: same idea, for WojActionKind::None. Measured on the live stand at
    // ~31 lines/minute/NPC (~45k lines, ~6MB/day, no rotation) once Part B
    // made the decision stream run forever with nothing bounding its length.
    // Reuses this exact pattern rather than inventing a second one, per the
    // fix brief.
    constexpr int64 NONE_LOG_COOLDOWN_SECONDS = 60;
    // A rough backstop against nonsense from the gateway, not a behaviour
    // rule: the decider's own leash is 20 yards by spec. This exists so a
    // hallucinated {"x":0,"y":0,"z":0} (a very plausible LLM output) cannot
    // send the murloc walking across the continent through the terrain.
    constexpr float MAX_MOVE_FROM_HOME_YARDS = 60.0f;
    constexpr size_t MAX_SAY_CHARS = 200;
    constexpr size_t MAX_LAST_ACTIONS = 3;
    constexpr size_t MAX_BEHAVIOR_EVENTS = 8;
    constexpr uint32_t CAST_ACK_GRACE_MS = 2000;
    constexpr uint32_t ACTIVATION_PROBE_MS = 500;

    std::string TriggerIdForSpell(uint32_t spell)
    {
        return "spell_" + std::to_string(spell);
    }

    // Обе stock SmartAI цели типа CREATURE_RANGE (Renew 6074 и Healing
    // Wave 913) пропускают самого caster. Это curated target rule, не
    // эвристика по SpellInfo: другой positive spell не должен унаследовать
    // запрет молча.
    bool RequiresOtherOwnedAlly(uint32_t policyActorId, uint32_t spell)
    {
        if (WojSpellBehavior const* policy = WojConfig::Instance().FindBehaviorSpellForActor(policyActorId, spell))
            return policy->target == "ally" && !policy->allowSelf;
        return spell == 6074 || spell == 913; // v1/v2 migration compatibility
    }

    bool ParseRawGuid(std::string const& value, ObjectGuid& out)
    {
        if (value.empty() || value.front() == '0')
            return false;
        if (!std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return false;
        try
        {
            size_t consumed = 0;
            unsigned long long const raw = std::stoull(value, &consumed);
            if (consumed != value.size() || raw == 0)
                return false;
            out = ObjectGuid(static_cast<uint64_t>(raw));
            return true;
        }
        catch (std::exception const&)
        {
            return false;
        }
    }

    float HealthPct(Unit const* unit)
    {
        uint32_t const maximum = unit->GetMaxHealth();
        return maximum ? 100.0f * static_cast<float>(unit->GetHealth()) / maximum : 0.0f;
    }

    bool IsOwned(Creature const* source, Unit const* unit)
    {
        Creature const* creature = unit ? unit->ToCreature() : nullptr;
        if (!source || !creature)
            return false;
        WojConfig const& config = WojConfig::Instance();
        uint32_t const sourceActor = WojRuntimeBinding::Instance().Resolve(source);
        uint32_t const targetActor = WojRuntimeBinding::Instance().Resolve(creature);
        if (!sourceActor || !targetActor)
            return false;
        bool const bound = config.Behavior.schemaVersion == 3
            ? config.IsBoundActor(targetActor, creature->GetEntry(), creature->GetMapId())
            : config.IsJevActor(targetActor, creature->GetEntry(), creature->GetMapId());
        return bound && config.SameGroup(sourceActor, targetActor);
    }

    bool IsThreat(Creature const* source, Unit const* unit)
    {
        if (!source || !unit)
            return false;
        if (source->GetVictim() == unit)
            return true;
        for (ThreatReference const* reference : source->GetThreatMgr().GetSortedThreatList())
            if (reference && reference->IsAvailable() && reference->GetVictim() == unit)
                return true;
        return false;
    }

    // A core combat edge may precede selection of Creature::GetVictim() by a
    // tick.  Do not turn that transient into an arbitrary target choice for a
    // stock "victim" spell: accept it only when the threat manager exposes
    // exactly one live available target, which is the requested unit.  The
    // same resolver is used by snapshot eligibility and final CAST validation.
    bool HasUnambiguousThreatVictim(Creature const* source, Unit const* unit)
    {
        if (!source || !unit)
            return false;
        if (source->GetVictim())
            return source->GetVictim() == unit;

        Unit const* sole = nullptr;
        for (ThreatReference const* reference : source->GetThreatMgr().GetSortedThreatList())
        {
            Unit* candidate = reference && reference->IsAvailable() ? reference->GetVictim() : nullptr;
            if (!candidate || !candidate->IsAlive())
                continue;
            if (sole && sole != candidate)
                return false;
            sole = candidate;
        }
        return sole == unit && IsThreat(source, unit);
    }

    bool AttackingOwned(Creature const* source, Unit const* unit)
    {
        return unit && IsOwned(source, unit->GetVictim());
    }

    bool HasExternalMovementControl(Creature const* creature)
    {
        return creature->HasFearAura() ||
            creature->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_STUNNED | UNIT_STATE_ROOT);
    }

    bool HasExternalAttackControl(Creature const* creature)
    {
        return creature->HasFearAura() ||
            creature->HasUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_STUNNED);
    }

    WojUnit SnapshotUnit(Unit const* source, Unit const* unit, bool engaged = false,
                         bool attackingAlly = false)
    {
        return {std::to_string(unit->GetGUID().GetRawValue()), unit->GetEntry(), unit->GetName(),
                unit->GetLevel(), unit->IsPlayer(), HealthPct(unit), source->GetDistance(unit),
                engaged, attackingAlly};
    }

    WojAlly SnapshotAlly(Unit const* source, Creature const* unit)
    {
        return {std::to_string(unit->GetGUID().GetRawValue()),
                WojRuntimeBinding::Instance().Resolve(unit), unit->GetEntry(),
                unit->GetName(), HealthPct(unit), source->GetDistance(unit), unit->IsInCombat()};
    }

    bool WithinHomeLeash(Unit const* unit, WojPoint const& home)
    {
        float const dx = unit->GetPositionX() - home.x;
        float const dy = unit->GetPositionY() - home.y;
        return dx * dx + dy * dy <= MAX_MOVE_FROM_HOME_YARDS * MAX_MOVE_FROM_HOME_YARDS;
    }

    bool IsCasterCenteredEnemyArea(SpellInfo const* spell, float& radius)
    {
        radius = 0.0f;
        for (SpellEffectInfo const& effect : spell->Effects)
        {
            if (!effect.IsEffect())
                continue;
            SpellImplicitTargetInfo const* targets[] = {&effect.TargetA, &effect.TargetB};
            bool const sourceIsCaster = effect.TargetA.GetTarget() == TARGET_SRC_CASTER ||
                                        effect.TargetB.GetTarget() == TARGET_SRC_CASTER;
            for (SpellImplicitTargetInfo const* target : targets)
            {
                bool const centeredOnCaster = target->GetReferenceType() == TARGET_REFERENCE_TYPE_CASTER ||
                    (sourceIsCaster && target->GetReferenceType() == TARGET_REFERENCE_TYPE_SRC);
                if (centeredOnCaster && target->IsArea() && target->GetCheckType() == TARGET_CHECK_ENEMY)
                {
                    radius = std::max(radius, effect.CalcRadius());
                }
            }
        }
        return radius > 0.0f;
    }

    bool HasEnoughPower(Creature const* caster, SpellInfo const* spell)
    {
        if (spell->PowerType >= MAX_POWERS)
            return false;
        int32 const cost = spell->CalcPowerCost(caster, spell->GetSchoolMask());
        return cost >= 0 && caster->GetPower(Powers(spell->PowerType)) >= static_cast<uint32_t>(cost);
    }

    bool IsSpellInRange(Creature const* caster, SpellInfo const* spell, Unit const* target)
    {
        float areaRadius = 0.0f;
        if (IsCasterCenteredEnemyArea(spell, areaRadius))
            return caster->IsWithinDistInMap(target, areaRadius) && caster->IsWithinLOSInMap(target);

        float const minRange = caster->GetSpellMinRangeForTarget(target, spell);
        float const maxRange = caster->GetSpellMaxRangeForTarget(target, spell);
        float const distance = caster->GetDistance(target);
        // Mirror Spell::CheckRange's minimum, including AzerothCore's ranged
        // NPC rule. Comparing only center distance to the DBC minimum made
        // Shoot appear eligible in melee range, then CastSpell rejected it
        // with SPELL_FAILED_TOO_CLOSE after the Jev response arrived.
        bool rangedMinimum = spell->RangeEntry &&
            spell->RangeEntry->Flags == SPELL_RANGE_RANGED;
        if (minRange > 0.0f && minRange <= 6.0f &&
            !caster->GetOwnerGUID().IsPlayer())
            rangedMinimum = true;
        bool const tooClose = rangedMinimum
            ? caster->IsWithinRange(target, minRange + caster->GetMeleeRange(target))
            : minRange > 0.0f && caster->IsWithinCombatRange(target, minRange);
        return !tooClose && distance <= maxRange && caster->IsWithinLOSInMap(target);
    }

    bool MatchesTargetSelector(Creature const* caster, WojSpellBehavior const* policy,
                               Unit const* target)
    {
        if (!caster || !policy || !target || policy->targetSelector == "any")
            return caster && policy && target;
        if (policy->targetSelector == "victim")
            return HasUnambiguousThreatVictim(caster, target);
        return policy->targetSelector == "hostile_random" && policy->hasTargetRadius &&
            caster->GetDistance(target) <= policy->targetRadiusYards;
    }

    char const* ActionName(WojActionKind kind)
    {
        switch (kind)
        {
            case WojActionKind::MoveTo: return "MOVE_TO";
            case WojActionKind::Say: return "SAY";
            case WojActionKind::StockTalk: return "STOCK_TALK";
            case WojActionKind::Idle: return "IDLE";
            case WojActionKind::Attack: return "ATTACK";
            case WojActionKind::Cast: return "CAST";
            case WojActionKind::StartPhase: return "START_PHASE";
            case WojActionKind::HealthEvent: return "HEALTH_EVENT";
            case WojActionKind::AggroEvent: return "AGGRO_EVENT";
            case WojActionKind::CallHelp: return "CALL_HELP";
            case WojActionKind::FleeForAssist: return "FLEE_FOR_ASSIST";
            case WojActionKind::Flee: return "FLEE";
            case WojActionKind::AnswerHelp: return "ANSWER_HELP";
            case WojActionKind::StopAttack: return "STOP_ATTACK";
            case WojActionKind::Evade: return "EVADE";
            case WojActionKind::ResumeRoutine: return "RESUME_ROUTINE";
            default: return "NONE";
        }
    }

    char const* RoutineProfileName(WojRoutineProfile profile)
    {
        switch (profile)
        {
            case WojRoutineProfile::Random: return "random";
            case WojRoutineProfile::Waypoint: return "waypoint";
            default: return "idle";
        }
    }

    void BroadcastFleeEmote(Creature* creature)
    {
        Acore::BroadcastTextBuilder builder(creature, CHAT_MSG_MONSTER_EMOTE,
            BROADCAST_TEXT_FLEE_FOR_ASSIST, creature->getGender());
        sCreatureTextMgr->SendChatPacket(creature, builder, CHAT_MSG_MONSTER_EMOTE);
    }
}

WojAI::WojAI(Creature* creature, uint32_t policyActorId, bool shadowOnly)
    : CreatureAI(creature), _policyActorId(policyActorId), _shadowOnly(shadowOnly),
      _shadowRandom(static_cast<uint32_t>(creature->GetGUID().GetRawValue()))
{
    _baselineEquipmentId = creature->GetOriginalEquipmentId();
    _baselineDualWield = creature->CanDualWield();
    // Базовое поведение не начинает бой само: ATTACK запускает его явно,
    // а сохранённое продолжение затем владеет преследованием и melee.
    // DEFENSIVE сохраняет штатный CreatureAI::TriggerAlert на скрытого
    // игрока. MoveInLineOfSight только запоминает допустимый aggro-триггер;
    // атаковать без решения Jev он не может.
    if (!_shadowOnly)
        me->SetReactState(REACT_DEFENSIVE);
    WojActorBinding const* actorBinding = WojConfig::Instance().FindActor(_policyActorId);
    bool const preserveStockRoutine = actorBinding && actorBinding->preserveStockRoutine;
    if (!_shadowOnly && !preserveStockRoutine)
        me->GetMotionMaster()->MoveIdle();

    creature->GetRespawnPosition(_spawnOrigin.x, _spawnOrigin.y, _spawnOrigin.z, &_spawnOrigin.o);
    _combatAnchor = _spawnOrigin;
    _routineWanderRadius = creature->GetWanderDistance();
    _routinePathId = creature->GetWaypointPath();
    switch (creature->GetDefaultMovementType())
    {
        case RANDOM_MOTION_TYPE:
            _routineProfile = WojRoutineProfile::Random;
            break;
        case WAYPOINT_MOTION_TYPE:
            _routineProfile = WojRoutineProfile::Waypoint;
            if (WaypointPath const* path = sWaypointMgr->GetPath(_routinePathId))
                _routinePointCount = static_cast<uint32_t>(path->Nodes.size());
            break;
        default:
            _routineProfile = WojRoutineProfile::Idle;
            _routineWanderRadius = 0.f;
            _routinePathId = 0;
            break;
    }
    if ((_shadowOnly || preserveStockRoutine) && _routineProfile != WojRoutineProfile::Idle)
    {
        _routineInstalled = true;
        _routineAuthorizedEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    }
    if (!_shadowOnly)
    {
        LOG_INFO("module", "mod-world-of-jevs: took over policy_actor {} spawn {} entry {}",
                 _policyActorId, creature->GetSpawnId(), creature->GetEntry());
        ApplyAiInitDespawn();
    }
    InitializeBehavior("spawn");
    _shadowPolicyRevision = WojConfig::Instance().Behavior.revision;
}

uint32_t WojAI::RollRange(uint32_t minimum, uint32_t maximum)
{
    if (maximum <= minimum)
        return minimum;
    if (!_shadowOnly)
        return urand(minimum, maximum);
    return std::uniform_int_distribution<uint32_t>(minimum, maximum)(_shadowRandom);
}

void WojAI::CaptureShadowSnapshot(WojSnapshot& out)
{
    if (!_shadowOnly)
        return;
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    if (_behaviorEpoch != epoch ||
        _shadowPolicyRevision != WojConfig::Instance().Behavior.revision)
    {
        InitializeBehavior("shadow_refresh");
        _shadowPolicyRevision = WojConfig::Instance().Behavior.revision;
    }
    bool const inCombat = me->IsInCombat();
    if (inCombat != _wasInCombat)
    {
        AppendBehaviorEvent("", 0, inCombat ? "combat_enter" : "combat_exit", "", "core");
        auto const now = std::chrono::steady_clock::now();
        for (TriggerRuntime& trigger : _triggers)
            if (trigger.spell && !trigger.eventDriven && !trigger.ticksOutOfCombat)
            {
                if (inCombat && !trigger.timerArmed)
                    ArmTimedTrigger(trigger, true, "combat_enter");
                else if (inCombat && trigger.timerPaused)
                {
                    trigger.deadline = now + std::chrono::milliseconds(trigger.view.waitMs);
                    trigger.timerPaused = false;
                }
                else if (!inCombat && trigger.timerArmed && !trigger.timerPaused)
                {
                    trigger.view.waitMs = static_cast<uint32_t>(std::max<int64_t>(0,
                        std::chrono::duration_cast<std::chrono::milliseconds>(trigger.deadline - now).count()));
                    trigger.timerPaused = true;
                }
            }
        if (inCombat)
        {
            _combatAnchor = {me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation()};
            _hasCombatAnchor = true;
            _aggroEventArmed = true;
            if (Unit* victim = me->GetVictim())
                _aggroInvoker = victim->GetGUID();
        }
        else
        {
            _aggroEventArmed = false;
            _aggroInvoker.Clear();
        }
        _wasInCombat = inCombat;
    }
    UpdateHealthPhases();
    UpdateHealthEvents();
    UpdateAggroEvents();
    out.capturedAt = std::chrono::steady_clock::now();
    out.epoch = epoch;
    BuildSnapshot(out);
    out.seq = ++_seq;
    WojConfig const& config = WojConfig::Instance();
    out.freshnessLimitMs = inCombat ? config.CombatFreshnessMs : config.IdleFreshnessMs;
    out.secondsSinceLastDecision = static_cast<float>(inCombat ? config.CombatTickMs : config.TickMs) / 1000.0f;
    out.secondsSinceLastAction = 0.0f;
    out.enqueuedAt = std::chrono::steady_clock::now();
}

void WojAI::ObserveShadowCast(uint32_t spellId)
{
    if (!_shadowOnly)
        return;
    TriggerRuntime* trigger = FindTrigger(TriggerIdForSpell(spellId));
    if (!trigger)
        return;
    _lastSuccessfulCast[spellId] = std::chrono::steady_clock::now();
    AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
        "succeeded", "", "observed_native_cast");
    if (trigger->oncePerEpoch)
    {
        trigger->eventConsumedThisEpoch = true;
        SetTriggerState(*trigger, "blocked", "once");
    }
    else if (!trigger->eventDriven)
        ArmTimedTrigger(*trigger, false, "observed_native_cast");
}

void WojAI::MoveInLineOfSight(Unit* who)
{
    if (!who || !who->IsAlive() || me->IsInCombat() || !me->CanSeeOrDetect(who) ||
        !me->CanStartAttack(who))
        return;
    auto const now = std::chrono::steady_clock::now();
    if (_observedAggroTarget.IsEmpty() || now >= _observedAggroUntil ||
        _observedAggroTarget == who->GetGUID())
    {
        _observedAggroTarget = who->GetGUID();
        _observedAggroUntil = now + OBSERVED_AGGRO_TTL;
    }
}

bool WojAI::CanStartObservedAggro(Unit* target) const
{
    return target && _observedAggroTarget == target->GetGUID() &&
        std::chrono::steady_clock::now() < _observedAggroUntil &&
        me->CanCreatureAttack(target);
}

bool WojAI::CanAssistFormation(Unit* target) const
{
    CreatureGroup* formation = me->GetFormation();
    if (!formation || !target || !target->IsAlive() || !me->IsValidAttackTarget(target) ||
        !me->CanCreatureAttack(target))
        return false;
    for (auto const& [member, info] : formation->GetMembers())
    {
        if (!member || member == me || !member->IsAlive() || member->GetVictim() != target)
            continue;
        // Mirror only the source formation's *eligibility* flags. Do not
        // call MemberEngagingTarget: that would attack before Jev decides.
        bool const eligible = member == formation->GetLeader()
            ? info.HasGroupFlag(uint16(GroupAIFlags::GROUP_AI_FLAG_MEMBER_ASSIST_LEADER))
            : info.HasGroupFlag(uint16(GroupAIFlags::GROUP_AI_FLAG_LEADER_ASSIST_MEMBER));
        if (eligible)
            return true;
    }
    return false;
}

void WojAI::ApplyAiInitDespawn()
{
    WojConfig const& config = WojConfig::Instance();
    WojActorBinding const* binding = config.FindActor(_policyActorId);
    if (!binding || binding->runtimeSummon || !binding->hasAiInitDespawn ||
        binding->spawnId != me->GetSpawnId() || binding->entry != me->GetEntry() ||
        binding->mapId != me->GetMapId() ||
        !WojCombatLog::Instance().ClaimAiInitDespawnRoll(me))
        return;
    WojAiInitDespawn const& rule = binding->aiInitDespawn;
    bool const selected = urand(1, 100) <= rule.chancePct;
    WojCombatLog::Instance().WriteAiInitDespawnRoll(me, rule.chancePct,
        rule.delayMs, selected, config.Behavior.revision);
    if (selected)
        me->DespawnOrUnsummon(Milliseconds(rule.delayMs));
}

WojAI::~WojAI()
{
    if (_shadowOnly)
        return;
    if (_pendingCast.active)
        FailPendingCast("cancelled", "unload");
    StopRoutine("unload");
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(WojRuntimeKey(me), epoch), "unload");
}

Player* WojAI::FindNearbyDecisionPlayer(float radiusYards) const
{
    Player* player = nullptr;
    Acore::AnyPlayerInObjectRangeCheck check(me, radiusYards, true, true);
    Acore::PlayerSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(me, player, check);
    Cell::VisitObjects(me, searcher, radiusYards);
    return player;
}

bool WojAI::RefreshDecisionActivation(uint32 diff, WojActorKey const& actor, uint32_t epoch)
{
    WojActivationBehavior const& activation = WojConfig::Instance().ActivationForActor(_policyActorId);
    bool active = activation.mode == WojActivationMode::Always;
    char const* reason = active ? "always" : "no_trigger";
    Player* nearbyPlayer = nullptr;

    if (!active && me->IsInCombat())
    {
        active = true;
        reason = "combat";
        _activationProbeElapsedMs = ACTIVATION_PROBE_MS;
    }
    else if (!active && !WojHelp::Instance().Inbox(actor, epoch).empty())
    {
        active = true;
        reason = "help";
        _activationProbeElapsedMs = ACTIVATION_PROBE_MS;
    }
    else if (!active)
    {
        _activationProbeElapsedMs = std::min<uint32_t>(ACTIVATION_PROBE_MS,
            _activationProbeElapsedMs + std::min(diff, ACTIVATION_PROBE_MS));
        if (!_activationInitialized || _activationProbeElapsedMs >= ACTIVATION_PROBE_MS)
        {
            nearbyPlayer = FindNearbyDecisionPlayer(activation.radiusYards);
            _nearbyDecisionPlayer = nearbyPlayer != nullptr;
            _activationProbeElapsedMs = 0;
        }
        active = _nearbyDecisionPlayer;
        if (active)
            reason = "proximity";
    }

    if (!_activationInitialized || active != _decisionActive)
    {
        if (!active)
        {
            WojRegistry::Instance().CancelPending(actor);
            ClearJevCastContinuation();
        }
        _activationInitialized = true;
        _decisionActive = active;
        WojCombatLog::Instance().WriteDecisionActivation(me, active, reason,
            activation.radiusYards, nearbyPlayer);
        if (active)
            _sinceDecision = me->IsInCombat() ? WojConfig::Instance().CombatTickMs
                                              : WojConfig::Instance().TickMs;
        else
            _sinceDecision = 0;
    }
    return active;
}

void WojAI::PrepareForReplacement(char const* reason)
{
    ClearJevCastContinuation();
    CancelAggroEventContinuation(reason ? reason : "replacement");
    if (_pendingCast.active)
        FailPendingCast("cancelled", reason);
    AppendBehaviorEvent("", 0, "reset", "", reason);
    StopRoutine(reason);
}

void WojAI::ReloadBehaviorPolicy()
{
    ClearJevCastContinuation();
    CancelAggroEventContinuation("config_reload");
    auto const consumedAggroEvents = _consumedAggroEvents;
    ObjectGuid const aggroInvoker = _aggroInvoker;
    bool const aggroEventArmed = _aggroEventArmed;
    bool const hadObservedCombat = _wasInCombat;
    bool const hadAggroTalkAttempt = _aggroTalkAttemptedThisCombat;
    TriggerRuntime const* oldAggroTalk = FindTrigger("aggro_talk");
    uint32_t const oldAggroTalkGeneration = oldAggroTalk ? oldAggroTalk->view.generation : 0;
    std::string const oldAggroTalkStatus = oldAggroTalk ? oldAggroTalk->view.status : "";
    std::string const oldAggroTalkBlockedBy = oldAggroTalk ? oldAggroTalk->view.blockedBy : "";
    uint32_t const oldEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    if (_pendingCast.active)
    {
        uint32_t const spell = _pendingCast.spell;
        FailPendingCast("cancelled", "config_reload");
        me->InterruptNonMeleeSpells(false, spell, true, true);
    }
    WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(WojRuntimeKey(me), oldEpoch),
                        "config_reload");
    WojRegistry::Instance().BumpEpoch(WojRuntimeKey(me), "config_reload");
    uint32_t const newEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    if (_routineInstalled)
        _routineAuthorizedEpoch = newEpoch;
    InitializeBehavior("config_reload");
    _consumedAggroEvents = consumedAggroEvents;
    _aggroInvoker = aggroInvoker;
    _aggroEventArmed = aggroEventArmed && me->IsInCombat();
    _wasInCombat = me->IsInCombat();
    // Reload is not a new combat. Preserve an unspoken eligible token
    // without rerolling it. A closed token keeps its real blocker (chance or
    // once); the blocked transition is only an epoch-boundary witness, never
    // a fabricated cancellation or second stock Talk.
    if (_wasInCombat && hadObservedCombat && hadAggroTalkAttempt && oldAggroTalkGeneration)
        if (TriggerRuntime* talk = FindTrigger("aggro_talk"))
        {
            talk->view.generation = oldAggroTalkGeneration;
            _aggroTalkAttemptedThisCombat = true;
            if (oldAggroTalkStatus == "eligible")
            {
                talk->view.status = oldAggroTalkStatus;
                talk->view.blockedBy = oldAggroTalkBlockedBy;
                AppendBehaviorEvent(talk->view.id, talk->view.generation,
                    "eligible", "", "config_reload_preserved");
            }
            else
            {
                talk->view.status = oldAggroTalkStatus;
                talk->view.blockedBy = oldAggroTalkBlockedBy;
                AppendBehaviorEvent(talk->view.id, talk->view.generation,
                    "blocked", "", "config_reload_preserved");
            }
        }
    if (_wasInCombat && !hadObservedCombat)
    {
        // Combat may have started between the previous UpdateAI observation
        // and reload. Reconstruct that edge once for every scene; otherwise
        // setting _wasInCombat above would suppress both common enter work
        // and Riverpaw's only stock-talk roll.
        AppendBehaviorEvent("", 0, "combat_enter", "", "config_reload");
        _aggroTalkAttemptedThisCombat = false;
        AttemptAggroTalk();
        if (!_hasCombatAnchor)
        {
            _combatAnchor = {me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation()};
            _hasCombatAnchor = true;
        }
        uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
        if (_routineInstalled && _routineAuthorizedEpoch == epoch)
        {
            SuspendRoutineForCombat();
            WojCombatLog::Instance().WriteRoutineEvent("routine_suspend", me, epoch,
                RoutineProfileName(_routineProfile), _routinePathId, "", "combat");
        }
    }
    if (_wasInCombat)
        for (TriggerRuntime& trigger : _triggers)
            if (trigger.spell && !trigger.eventDriven && !trigger.ticksOutOfCombat && !trigger.timerArmed)
                ArmTimedTrigger(trigger, true, "config_reload");
    _sinceDecision = me->IsInCombat() ? WojConfig::Instance().CombatTickMs :
        WojConfig::Instance().TickMs;
    LOG_INFO("module", "mod-world-of-jevs: behavior policy revision {} applied to guid {} epoch {}",
             WojConfig::Instance().Behavior.revision, _policyActorId, newEpoch);
}

WojAI::TriggerRuntime* WojAI::FindTrigger(std::string const& id)
{
    auto it = std::find_if(_triggers.begin(), _triggers.end(), [&](TriggerRuntime const& trigger)
    {
        return trigger.view.id == id;
    });
    return it == _triggers.end() ? nullptr : &*it;
}

WojAI::TriggerRuntime const* WojAI::FindTrigger(std::string const& id) const
{
    auto it = std::find_if(_triggers.begin(), _triggers.end(), [&](TriggerRuntime const& trigger)
    {
        return trigger.view.id == id;
    });
    return it == _triggers.end() ? nullptr : &*it;
}

void WojAI::AppendBehaviorEvent(std::string const& triggerId, uint32_t generation,
                                char const* kind, std::string const& decisionId,
                                std::string const& reason)
{
    WojBehaviorEvent event;
    event.seq = ++_behaviorSeq;
    event.triggerId = triggerId;
    event.generation = generation;
    event.kind = kind;
    event.decisionId = decisionId;
    event.reason = reason.substr(0, 80);
    _behaviorEvents.push_back(event);
    while (_behaviorEvents.size() > MAX_BEHAVIOR_EVENTS)
        _behaviorEvents.pop_front();
    if (!_shadowOnly)
        WojCombatLog::Instance().WriteTriggerTransition(me, _behaviorEpoch, event);
}

void WojAI::SetTriggerState(TriggerRuntime& trigger, char const* status,
                            char const* blockedBy, char const* eventKind,
                            std::string const& decisionId, std::string const& reason)
{
    std::string const blocked = blockedBy ? blockedBy : "";
    bool const changed = trigger.view.status != status || trigger.view.blockedBy != blocked;
    trigger.view.status = status;
    trigger.view.blockedBy = blocked;
    if (eventKind)
        AppendBehaviorEvent(trigger.view.id, trigger.view.generation, eventKind, decisionId, reason);
    else if (changed && trigger.view.status == "eligible")
        AppendBehaviorEvent(trigger.view.id, trigger.view.generation, "eligible", "", "predicate");
    else if (changed && trigger.view.status == "blocked")
        AppendBehaviorEvent(trigger.view.id, trigger.view.generation, "blocked", "", blocked);
}

void WojAI::ArmTimedTrigger(TriggerRuntime& trigger, bool initial, char const* reason)
{
    if (trigger.timerArmed)
        ++trigger.view.generation;
    uint32_t const minimum = initial ? trigger.firstMinMs : trigger.repeatMinMs;
    uint32_t const maximum = initial ? trigger.firstMaxMs : trigger.repeatMaxMs;
    uint32_t const delay = RollRange(minimum, maximum);
    trigger.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    trigger.timerArmed = true;
    trigger.timerPaused = trigger.ambientTalk ? me->IsInCombat() :
        (!trigger.ticksOutOfCombat && !me->IsInCombat());
    trigger.chancePassed = trigger.chancePct >= 100;
    trigger.view.hasWaitMs = true;
    trigger.view.waitMs = delay;
    trigger.view.status = "waiting";
    trigger.view.blockedBy = "timer";
    AppendBehaviorEvent(trigger.view.id, trigger.view.generation, "rearmed", "", reason);
}

void WojAI::StartHealthPhase(WojHealthPhaseBehavior const& phase,
                             std::string const& decisionId, uint32_t generation,
                             uint32_t decisionEpoch)
{
    _healthPhase = HealthPhaseRuntime{};
    _healthPhase.active = true;
    _healthPhase.behavior = phase;
    _healthPhase.decisionId = decisionId;
    _healthPhase.generation = generation;
    _healthPhase.decisionEpoch = decisionEpoch;
    _healthPhase.victim = me->GetVictim() ? me->GetVictim()->GetGUID() : _attackContinuation;
    _consumedHealthPhases.insert(phase.id);

    auto const now = std::chrono::steady_clock::now();
    _nextHealthPhaseAt = now + std::chrono::milliseconds(phase.tacticalDelayMs);
    for (TriggerRuntime& trigger : _triggers)
        if (trigger.timerArmed && !trigger.eventDriven)
        {
            if (trigger.timerPaused)
                trigger.view.waitMs = std::min<uint32_t>(
                    std::numeric_limits<uint32_t>::max() - phase.tacticalDelayMs,
                    trigger.view.waitMs) + phase.tacticalDelayMs;
            else
                trigger.deadline += std::chrono::milliseconds(phase.tacticalDelayMs);
        }

    if (_pendingCast.active)
    {
        uint32_t const spell = _pendingCast.spell;
        FailPendingCast("cancelled", "health_phase");
        me->InterruptNonMeleeSpells(false, spell, true, true);
    }

    WojActorKey const actor = WojRuntimeKey(me);
    WojRegistry::Instance().CancelPending(actor, "health_phase");
    WojRegistry::Instance().BumpEpoch(actor, "health_phase");
    _behaviorEpoch = WojRegistry::Instance().CurrentEpoch(actor);
    me->CastSpell(me, phase.transitionSpell, false);
    sCreatureTextMgr->SendChat(me, static_cast<uint8>(phase.talkGroup));
    me->SetUnitFlag(UNIT_FLAG_PACIFIED);
    me->SetReactState(REACT_PASSIVE);
    me->GetMotionMaster()->Clear();
    me->GetMotionMaster()->MovePoint(0x574F50, phase.destination.x, phase.destination.y,
        phase.destination.z);
    WojCombatLog::Instance().WriteHealthPhase(me, phase, "triggered", _behaviorEpoch,
        HealthPct(me), decisionId, generation, decisionEpoch);
}

bool WojAI::UpdateHealthPhases()
{
    auto const now = std::chrono::steady_clock::now();
    if (_healthPhase.active)
    {
        if (_healthPhase.arrived && !_healthPhase.equipmentApplied && now >= _healthPhase.equipmentAt)
        {
            me->LoadEquipment(static_cast<int8>(_healthPhase.behavior.equipmentId), true);
            me->SetCanDualWield(_healthPhase.behavior.dualWield);
            _healthPhase.equipmentApplied = true;
            WojCombatLog::Instance().WriteHealthPhase(me, _healthPhase.behavior, "equipment",
                _behaviorEpoch, HealthPct(me), _healthPhase.decisionId, _healthPhase.generation,
                _healthPhase.decisionEpoch);
        }
        if (_healthPhase.arrived && now >= _healthPhase.restoreAt)
        {
            WojHealthPhaseBehavior const completed = _healthPhase.behavior;
            std::string const decisionId = _healthPhase.decisionId;
            uint32_t const generation = _healthPhase.generation;
            uint32_t const decisionEpoch = _healthPhase.decisionEpoch;
            ObjectGuid const victimGuid = _healthPhase.victim;
            _healthPhase = HealthPhaseRuntime{};
            me->SetReactState(REACT_DEFENSIVE);
            me->RemoveUnitFlag(UNIT_FLAG_PACIFIED);
            me->SetStandState(UNIT_STAND_STATE_STAND);
            if (Unit* victim = ObjectAccessor::GetUnit(*me, victimGuid); victim && victim->IsAlive())
            {
                _attackContinuation = victim->GetGUID();
                AttackStartOwned(victim);
                me->SetTarget(victim->GetGUID());
            }
            WojCombatLog::Instance().WriteHealthPhase(me, completed, "restored",
                _behaviorEpoch, HealthPct(me), decisionId, generation, decisionEpoch);
            if (TriggerRuntime* trigger = FindTrigger("health_phase_" + completed.id))
                SetTriggerState(*trigger, "blocked", "once", "succeeded",
                    decisionId, "phase_restored");
            return false;
        }
        return true;
    }

    bool earlierPending = false;
    for (WojHealthPhaseBehavior const& phase : WojConfig::Instance().HealthPhasesForActor(_policyActorId))
    {
        TriggerRuntime* trigger = FindTrigger("health_phase_" + phase.id);
        if (!trigger)
            continue;
        trigger->view.hasWaitMs = false;
        trigger->view.waitMs = 0;
        if (_consumedHealthPhases.count(phase.id))
        {
            SetTriggerState(*trigger, "blocked", "once");
            continue;
        }
        if (earlierPending)
        {
            SetTriggerState(*trigger, "blocked", "policy");
            continue;
        }
        earlierPending = true;
        if (!me->IsInCombat())
        {
            SetTriggerState(*trigger, "blocked", "combat");
            continue;
        }
        if (now < _nextHealthPhaseAt)
        {
            trigger->view.hasWaitMs = true;
            trigger->view.waitMs = static_cast<uint32_t>(std::max<int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    _nextHealthPhaseAt - now).count()));
            SetTriggerState(*trigger, "waiting", "timer");
            continue;
        }
        if (_pendingCast.active || _ownsTimedFlee || _ownsAssistFlight ||
            HasExternalMovementControl(me))
        {
            SetTriggerState(*trigger, "blocked", "control");
            continue;
        }
        bool const below = me->GetMaxHealth() &&
            static_cast<uint64_t>(me->GetHealth()) * 100u <
                static_cast<uint64_t>(me->GetMaxHealth()) * phase.triggerBelowHealthPct;
        SetTriggerState(*trigger, below ? "eligible" : "blocked", below ? nullptr : "health");
    }
    return false;
}

void WojAI::UpdateHealthEvents()
{
    auto const& events = WojConfig::Instance().HealthEventsForActor(_policyActorId);
    uint32_t const pct = me->GetMaxHealth() ? static_cast<uint32_t>(me->GetHealthPct()) : 0;
    for (WojHealthEventBehavior const& event : events)
    {
        TriggerRuntime* trigger = FindTrigger("health_event_" + event.id);
        if (!trigger)
            continue;
        trigger->view.hasWaitMs = false;
        trigger->view.waitMs = 0;
        if (_consumedHealthEvents.count(event.id))
            SetTriggerState(*trigger, "blocked", "once");
        else if (!me->IsInCombat())
            SetTriggerState(*trigger, "blocked", "combat");
        else if (pct < event.healthPctMin || pct > event.healthPctMax)
            SetTriggerState(*trigger, "blocked", "health");
        else if (_pendingCast.active || me->IsNonMeleeSpellCast(false) ||
                 _ownsTimedFlee || _ownsAssistFlight || HasExternalMovementControl(me))
            SetTriggerState(*trigger, "blocked", "control");
        else if (!sCreatureTextMgr->TextExist(me->GetEntry(), event.talkGroup))
            SetTriggerState(*trigger, "blocked", "physical");
        else
            SetTriggerState(*trigger, "eligible", nullptr);
    }
}

void WojAI::UpdateAggroEvents()
{
    auto const& events = WojConfig::Instance().AggroEventsForActor(_policyActorId);
    Unit* invoker = _aggroInvoker.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*me, _aggroInvoker);
    for (WojAggroEventBehavior const& event : events)
    {
        TriggerRuntime* trigger = FindTrigger("aggro_event_" + event.id);
        if (!trigger)
            continue;
        if (_consumedAggroEvents.count(event.id))
            SetTriggerState(*trigger, "blocked", "once");
        else if (_aggroEventContinuation.active && _aggroEventContinuation.eventId == event.id)
            SetTriggerState(*trigger, "active", nullptr);
        else if (_aggroEventContinuation.active)
            SetTriggerState(*trigger, "blocked", "policy");
        else if (!_aggroEventArmed || !me->IsInCombat())
            SetTriggerState(*trigger, "blocked", "combat");
        else if (!invoker || !invoker->IsAlive() || !me->IsValidAttackTarget(invoker))
            SetTriggerState(*trigger, "blocked", "target");
        else if (_pendingCast.active || me->IsNonMeleeSpellCast(false) ||
                 _ownsTimedFlee || _ownsAssistFlight || HasExternalMovementControl(me))
            SetTriggerState(*trigger, "blocked", "control");
        else if (!sCreatureTextMgr->TextExist(me->GetEntry(), event.actions.back().textGroup))
            SetTriggerState(*trigger, "blocked", "physical");
        else
            SetTriggerState(*trigger, "eligible", nullptr);
    }
}

void WojAI::CancelAggroEventContinuation(char const* reason)
{
    if (!_aggroEventContinuation.active)
        return;
    AggroEventContinuation const pending = _aggroEventContinuation;
    _aggroEventContinuation = AggroEventContinuation{};
    Unit* invoker = pending.invoker.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*me, pending.invoker);
    WojCombatLog::Instance().WriteAggroEventEffect(me, invoker, pending.eventId,
        pending.decisionId, pending.epoch, pending.generation, 0, "sequence", 0, 0,
        reason ? reason : "cancelled", 0, pending.attempt);
    if (TriggerRuntime* trigger = FindTrigger("aggro_event_" + pending.eventId);
        trigger && trigger->view.generation == pending.generation)
        SetTriggerState(*trigger, "blocked", "combat", "cancelled",
            pending.decisionId, reason ? reason : "cancelled");
}

void WojAI::AdvanceAggroEventContinuation()
{
    if (!_aggroEventContinuation.active ||
        std::chrono::steady_clock::now() < _aggroEventContinuation.retryAt)
        return;
    AggroEventContinuation pending = _aggroEventContinuation;
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    auto const& events = WojConfig::Instance().AggroEventsForActor(_policyActorId);
    auto const found = std::find_if(events.begin(), events.end(),
        [&](WojAggroEventBehavior const& event)
        {
            return event.id == pending.eventId && event.actorId == _policyActorId &&
                event.entry == me->GetEntry();
        });
    TriggerRuntime* trigger = FindTrigger("aggro_event_" + pending.eventId);
    Unit* invoker = pending.invoker.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*me, pending.invoker);
    if (pending.epoch != epoch || found == events.end() || !trigger ||
        trigger->view.generation != pending.generation || !_aggroEventArmed ||
        !me->IsInCombat() || !invoker || !invoker->IsAlive() ||
        !me->IsValidAttackTarget(invoker))
    {
        CancelAggroEventContinuation("stale_or_target_lost");
        _aggroEventArmed = false;
        return;
    }
    if (me->IsNonMeleeSpellCast(false))
    {
        _aggroEventContinuation.retryAt = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(1200);
        return;
    }
    if (pending.attempt < std::numeric_limits<uint32_t>::max())
        ++pending.attempt;
    _aggroEventContinuation.attempt = pending.attempt;
    bool allEffectsApplied = true;
    for (size_t index = 0; index < found->actions.size(); ++index)
    {
        WojAggroEventEffect const& effect = found->actions[index];
        if (effect.kind == WojAggroEventEffectKind::Cast)
        {
            SpellCastResult const result = me->CastSpell(invoker, effect.spell, false);
            if (!_aggroEventContinuation.active || _aggroEventContinuation.epoch != pending.epoch ||
                _aggroEventContinuation.generation != pending.generation ||
                _aggroEventContinuation.eventId != pending.eventId)
                return;
            bool const linked = result == SPELL_CAST_OK || result == SPELL_FAILED_SPELL_IN_PROGRESS;
            allEffectsApplied &= result == SPELL_CAST_OK;
            WojCombatLog::Instance().WriteAggroEventEffect(me, invoker, found->id,
                pending.decisionId, epoch, pending.generation, index, "cast",
                effect.spell, 0, result == SPELL_CAST_OK ? "core_accepted" :
                    (linked ? "core_in_progress" : "core_rejected"),
                static_cast<uint32_t>(result), pending.attempt);
            if (!linked)
            {
                // SmartAI RetryLater raises a 1200 ms timer and does not run
                // linked effects. This is continuation of the same Jev order.
                _aggroEventContinuation.retryAt = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(1200);
                return;
            }
        }
        else if (effect.kind == WojAggroEventEffectKind::RemoveAura)
        {
            bool const before = me->HasAura(effect.spell);
            me->RemoveAurasDueToSpell(effect.spell);
            bool const after = me->HasAura(effect.spell);
            allEffectsApplied &= !after;
            WojCombatLog::Instance().WriteAggroEventEffect(me, me, found->id,
                pending.decisionId, epoch, pending.generation, index, "remove_aura",
                effect.spell, 0, after ? "still_present" : (before ? "removed" : "absent"),
                0, pending.attempt);
        }
        else
        {
            Talk(static_cast<uint8>(effect.textGroup), invoker);
            WojCombatLog::Instance().WriteStockTalk(me, pending.decisionId, epoch,
                trigger->view.id, pending.generation, effect.textGroup);
            WojCombatLog::Instance().WriteAggroEventEffect(me, invoker, found->id,
                pending.decisionId, epoch, pending.generation, index, "talk",
                0, effect.textGroup, "dispatched", 0, pending.attempt);
        }
    }
    std::string const decisionId = pending.decisionId;
    std::string const eventId = pending.eventId;
    _aggroEventContinuation = AggroEventContinuation{};
    _consumedAggroEvents.insert(eventId);
    SetTriggerState(*trigger, "blocked", "once",
        allEffectsApplied ? "succeeded" : "failed", decisionId,
        allEffectsApplied ? "stock_aggro_event" : "physical_effect_failed");
}

void WojAI::RestoreHealthPhaseBaseline(char const* reason)
{
    auto const& phases = WojConfig::Instance().HealthPhasesForActor(_policyActorId);
    if (phases.empty() && !_healthPhase.active && _consumedHealthPhases.empty())
        return;
    WojHealthPhaseBehavior evidence = _healthPhase.active ? _healthPhase.behavior :
        (!phases.empty() ? phases.front() : WojHealthPhaseBehavior{});
    std::string const decisionId = _healthPhase.decisionId;
    uint32_t const generation = _healthPhase.generation;
    uint32_t const decisionEpoch = _healthPhase.decisionEpoch;
    if (_healthPhase.active)
        if (TriggerRuntime* trigger = FindTrigger("health_phase_" + evidence.id))
            AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
                "cancelled", decisionId, reason ? reason : "reset");
    _healthPhase = HealthPhaseRuntime{};
    _consumedHealthPhases.clear();
    _nextHealthPhaseAt = {};
    me->LoadEquipment(_baselineEquipmentId, true);
    me->SetCanDualWield(_baselineDualWield);
    me->SetStandState(UNIT_STAND_STATE_STAND);
    me->RemoveUnitFlag(UNIT_FLAG_PACIFIED);
    me->SetReactState(REACT_DEFENSIVE);
    WojCombatLog::Instance().WriteHealthPhase(me, evidence, reason ? reason : "reset",
        WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)), HealthPct(me),
        decisionId, generation, decisionEpoch);
}

void WojAI::InitializeBehavior(char const* reason)
{
    _behaviorEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    _behaviorSeq = 0;
    _behaviorEvents.clear();
    _triggers.clear();
    _pendingCast = PendingCast{};
    _nextSecondaryYieldAt = {};
    _secondaryDecisionWaitUntil = {};
    _pendingHeal = PendingHeal{};
    _aggroTalkAttemptedThisCombat = false;
    _aggroEventArmed = false;
    _aggroInvoker.Clear();
    _aggroEventContinuation = AggroEventContinuation{};

    auto add = [&](std::string id, WojSpellBehavior const* policy = nullptr)
    {
        TriggerRuntime trigger;
        trigger.view.id = std::move(id);
        trigger.view.generation = 1;
        trigger.view.status = "blocked";
        trigger.view.blockedBy = "physical";
        if (policy)
        {
            trigger.spell = policy->spell;
            trigger.firstMinMs = policy->initial.minimum;
            trigger.firstMaxMs = policy->initial.maximum;
            trigger.repeatMinMs = policy->repeat.minimum;
            trigger.repeatMaxMs = policy->repeat.maximum;
            trigger.ticksOutOfCombat = policy->ticksOutOfCombat;
            trigger.chancePct = policy->chancePct;
            trigger.eventDriven = policy->eventDriven;
            trigger.oncePerEpoch = policy->oncePerEpoch;
        }
        _triggers.push_back(std::move(trigger));
    };

    add("aggro");
    add("help_call");
    add("help_answer");
    add("flee");
    add("routine");
    add("return");
    if (IsAggroTalker())
        add("aggro_talk");
    if (IsAmbientTalker())
    {
        add("ambient_talk");
        TriggerRuntime* trigger = FindTrigger("ambient_talk");
        WojAmbientTalkBehavior const& policy = WojConfig::Instance().AmbientTalkForActor(_policyActorId);
        trigger->ambientTalk = true;
        trigger->firstMinMs = policy.initial.minimum;
        trigger->firstMaxMs = policy.initial.maximum;
        trigger->repeatMinMs = policy.repeat.minimum;
        trigger->repeatMaxMs = policy.repeat.maximum;
        trigger->chancePct = policy.chancePct;
        ArmTimedTrigger(*trigger, true, "initial");
    }
    for (WojSpellBehavior const* policy : WojConfig::Instance().BehaviorSpellsForActor(
        _policyActorId, me->GetEntry()))
        add(TriggerIdForSpell(policy->spell), policy);
    for (WojHealthPhaseBehavior const& phase : WojConfig::Instance().HealthPhasesForActor(
        _policyActorId))
        add("health_phase_" + phase.id);
    for (WojHealthEventBehavior const& event : WojConfig::Instance().HealthEventsForActor(
        _policyActorId))
        add("health_event_" + event.id);
    for (WojAggroEventBehavior const& event : WojConfig::Instance().AggroEventsForActor(
        _policyActorId))
        add("aggro_event_" + event.id);

    AppendBehaviorEvent("", 0, "reset", "", reason);
    for (TriggerRuntime& trigger : _triggers)
        if (trigger.spell && trigger.eventDriven)
            AppendBehaviorEvent(trigger.view.id, trigger.view.generation, "rearmed", "", "initial");
        else if (trigger.spell && trigger.ticksOutOfCombat)
            ArmTimedTrigger(trigger, true, "initial");
}

void WojAI::BuildSnapshot(WojSnapshot& out)
{
    // seq is stamped by UpdateAI, not here: only UpdateAI knows the snapshot
    // is actually being handed over, so it owns the numbering.
    out.actor = WojRuntimeKey(me);
    out.guid = _policyActorId;
    out.entry = me->GetEntry();
    out.name = me->GetName();
    out.id = std::to_string(me->GetGUID().GetRawValue());
    out.position = {me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation()};
    out.home = _spawnOrigin;
    out.health = me->GetHealth();
    out.healthMax = me->GetMaxHealth();
    out.mana = me->GetPower(POWER_MANA);
    out.manaMax = me->GetMaxPower(POWER_MANA);
    out.inCombat = me->IsInCombat();
    out.fleeing = me->HasUnitState(UNIT_STATE_FLEEING);
    out.behavior.policyRevision = WojConfig::Instance().Behavior.revision;
    if (WojActorBinding const* binding = WojConfig::Instance().FindActor(_policyActorId))
        out.behavior.bindingGeneration = binding->generation;
    else
        out.behavior.bindingGeneration = WojConfig::Instance().BindingGeneration;
    out.behavior.groupId = WojConfig::Instance().Behavior.schemaVersion == 3
        ? WojConfig::Instance().GroupId(_policyActorId) : "";
    out.behavior.sceneId = WojConfig::Instance().SceneId;
    out.lastActions = _lastActions;
    out.routine.profile = _routineProfile;
    out.routine.wanderRadius = _routineProfile == WojRoutineProfile::Random ? _routineWanderRadius : 0.f;
    out.routine.pathId = _routineProfile == WojRoutineProfile::Waypoint ? _routinePathId : 0;
    out.routine.pointCount = _routineProfile == WojRoutineProfile::Waypoint ? _routinePointCount : 0;
    out.routine.active = _routineInstalled && _routineAuthorizedEpoch == out.epoch;
    bool const routineMovementAvailable = _routineProfile == WojRoutineProfile::Idle ||
        (!me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) && me->GetSpeed(MOVE_WALK) >= 0.1f);
    out.routine.canResume = !out.routine.active && me->IsAlive() && !me->IsInCombat() &&
        !HasExternalMovementControl(me) && routineMovementAvailable;

    auto const actorSpellPolicies = WojConfig::Instance().BehaviorSpellsForActor(
        _policyActorId, me->GetEntry());
    bool hasPositiveSpell = false;
    for (WojSpellBehavior const* policy : actorSpellPolicies)
        if (SpellInfo const* spell = sSpellMgr->GetSpellInfo(policy->spell); spell && spell->IsPositive())
        {
            hasPositiveSpell = true;
            break;
        }

    // Лечебный домен строится по явным14 owned spawn, а не по ambient
    // обзору30yd. Для NPC без положительных заклинаний этот обход не нужен.
    std::vector<Creature*> ownedHealCandidates;
    Map* map = me->GetMap();
    if (map && hasPositiveSpell)
    {
        WojConfig const& config = WojConfig::Instance();
        std::unordered_set<uint32_t> const candidates = config.Behavior.schemaVersion == 3
            ? config.GroupActors(config.GroupId(_policyActorId)) : config.OwnedGuids;
        for (uint32_t spawnId : candidates)
        {
            if (spawnId == _policyActorId)
                continue;
            auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            Creature* candidate = nullptr;
            for (auto it = bounds.first; it != bounds.second; ++it)
                if (it->second && it->second->IsAlive())
                {
                    candidate = it->second;
                    break;
                }
            if (candidate && IsOwned(me, candidate) && me->IsFriendlyTo(candidate))
                ownedHealCandidates.push_back(candidate);
        }
    }
    std::sort(ownedHealCandidates.begin(), ownedHealCandidates.end(), [](Creature const* left, Creature const* right)
    {
        return left->GetGUID().GetRawValue() < right->GetGUID().GetRawValue();
    });
    if (ownedHealCandidates.size() > MAX_HEAL_ALLIES)
        ownedHealCandidates.resize(MAX_HEAL_ALLIES);

    if (Unit* victim = me->GetVictim(); victim && victim->IsAlive())
    {
        out.hasVictim = true;
        out.victim = SnapshotUnit(me, victim, true, AttackingOwned(me, victim));
    }

    for (ThreatReference const* reference : me->GetThreatMgr().GetSortedThreatList())
    {
        Unit* attacker = reference ? reference->GetVictim() : nullptr;
        if (!reference || !reference->IsAvailable() || !attacker || !attacker->IsAlive())
            continue;
        out.attackers.push_back(SnapshotUnit(me, attacker, true, AttackingOwned(me, attacker)));
        if (out.attackers.size() == MAX_ATTACKERS)
            break;
    }

    std::list<Unit*> nearby;
    Acore::AnyUnitInObjectRangeCheck check(me, NEIGHBOR_SEARCH_YARDS);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(me, nearby, check);
    Cell::VisitObjects(me, searcher, NEIGHBOR_SEARCH_YARDS);

    std::vector<Unit*> sensedEnemies;
    for (Unit* unit : nearby)
    {
        if (!unit || unit == me || !unit->IsAlive())
            continue;
        float const distance = me->GetDistance(unit);
        if (distance <= NEIGHBOR_SEARCH_YARDS && me->IsValidAttackTarget(unit) &&
            me->CanSeeOrDetect(unit) && me->IsWithinLOSInMap(unit))
            sensedEnemies.push_back(unit);
        Creature* ally = unit->ToCreature();
        if (ally && distance <= ALLY_SEARCH_YARDS && IsOwned(me, ally) && me->IsFriendlyTo(ally))
            out.allies.push_back(SnapshotAlly(me, ally));
    }

    std::sort(sensedEnemies.begin(), sensedEnemies.end(), [&](Unit const* left, Unit const* right)
    {
        float const leftDistance = me->GetDistance(left);
        float const rightDistance = me->GetDistance(right);
        return leftDistance == rightDistance ? left->GetGUID().GetRawValue() < right->GetGUID().GetRawValue()
                                             : leftDistance < rightDistance;
    });
    if (!sensedEnemies.empty())
    {
        out.hasNearestEnemy = true;
        Unit* nearest = sensedEnemies.front();
        out.nearestEnemy = SnapshotUnit(me, nearest, IsThreat(me, nearest), AttackingOwned(me, nearest));
    }
    std::sort(out.allies.begin(), out.allies.end(), [](WojAlly const& left, WojAlly const& right)
    {
        return left.distance == right.distance ? left.id < right.id : left.distance < right.distance;
    });
    if (out.allies.size() > MAX_ALLIES)
        out.allies.resize(MAX_ALLIES);
    WojPoint const& movementAnchor = MovementAnchor();
    float const callHelpRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    float const fleeAssistRadius = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
    WojFleeBehavior const& fleePolicy = WojConfig::Instance().FleeForActor(_policyActorId);
    bool const canFlee = fleePolicy.enabled && !me->HasPreventsFleeingAura() && !HasExternalMovementControl(me) &&
        me->GetSpeed(MOVE_RUN) >= 0.1f && WithinHomeLeash(me, movementAnchor);
    bool const helpOutstanding = WojHelp::Instance().HasOutstandingRequest(WojRuntimeKey(me), out.epoch);
    for (WojAlly const& ally : out.allies)
    {
        ObjectGuid guid;
        Unit* candidate = ParseRawGuid(ally.id, guid) ? ObjectAccessor::GetUnit(*me, guid) : nullptr;
        if (!candidate || !me->IsWithinLOSInMap(candidate))
            continue;
        // A call is useful only when it can recruit a free ally. Re-addressing
        // an actor already in combat merely creates a request/answer roundtrip
        // with no physical change and can race a still in-flight answer.
        if (!helpOutstanding && !candidate->IsInCombat() && ally.distance <= callHelpRadius)
            out.callHelpTargets.push_back(ally.id);
        float const dx = candidate->GetPositionX() - movementAnchor.x;
        float const dy = candidate->GetPositionY() - movementAnchor.y;
        if (!helpOutstanding && canFlee && ally.distance <= fleeAssistRadius &&
            dx * dx + dy * dy <= MAX_MOVE_FROM_HOME_YARDS * MAX_MOVE_FROM_HOME_YARDS)
            out.fleeAssistTargets.push_back(ally.id);
    }

    // Порядок усечения — часть восприятия, а не скрытый выбор цели:
    // текущая цель, доступные угрозы, затем остальные видимые противники.
    std::unordered_set<uint64_t> enemyIds;
    auto addEnemy = [&](Unit* unit)
    {
        bool const formationAssist = CanAssistFormation(unit);
        if (!unit || !unit->IsAlive() || me->GetDistance(unit) > NEIGHBOR_SEARCH_YARDS ||
            !me->IsValidAttackTarget(unit) ||
            ((!me->CanSeeOrDetect(unit) || !me->IsWithinLOSInMap(unit)) && !formationAssist) ||
            !enemyIds.insert(unit->GetGUID().GetRawValue()).second || out.enemies.size() >= MAX_ENEMIES)
            return;
        bool const engaged = IsThreat(me, unit);
        bool const attackingAlly = AttackingOwned(me, unit) || formationAssist;
        out.enemies.push_back(SnapshotUnit(me, unit, engaged, attackingAlly));
        // A fresh observed stock-aggro event remains actionable while its
        // async Jev answer is in flight, even if a high-level player steps
        // just outside the short level-scaled aggro radius. Never invent a
        // different/nearest target; all other physical guards still apply.
        if (engaged ? me->CanCreatureAttack(unit) :
            (me->CanStartAttack(unit) || CanStartObservedAggro(unit) || formationAssist))
            out.attackTargets.push_back(std::to_string(unit->GetGUID().GetRawValue()));
    };
    addEnemy(me->GetVictim());
    for (ThreatReference const* reference : me->GetThreatMgr().GetSortedThreatList())
        if (reference && reference->IsAvailable())
            addEnemy(reference->GetVictim());
    if (CreatureGroup* formation = me->GetFormation())
        for (auto const& [member, info] : formation->GetMembers())
            if (member && member != me && member->IsAlive())
                addEnemy(member->GetVictim());
    for (Unit* unit : sensedEnemies)
        addEnemy(unit);
    // A visible stock-aggro target takes precedence over peaceful motion.
    // Without this fence an async snapshot could offer both ATTACK and
    // RESUME_ROUTINE; combat could begin before the latter arrived and turn
    // an otherwise fresh Jev answer into a predictable executor rejection.
    if (!out.attackTargets.empty())
        out.routine.canResume = false;
    if (canFlee)
        for (WojUnit const& enemy : out.enemies)
            if (enemy.engaged)
                out.fleeTargets.push_back(enemy.id);

    std::unordered_set<uint64_t> observedHealAllies;
    if (!actorSpellPolicies.empty())
    {
        for (WojSpellBehavior const* behavior : actorSpellPolicies)
        {
            uint32_t const spellId = behavior->spell;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
            if (!spell)
                continue;
            WojSpell observed;
            observed.id = spellId;
            observed.cooldownMs = me->GetSpellCooldown(spellId);
            // A Jev-owned flee is already a committed tactical action.  Do
            // not advertise a spell that the executor would have to satisfy
            // by silently cancelling that earlier decision.  The same fence
            // is repeated in RefreshBehavior/ValidateTriggeredAction because
            // an asynchronous answer can cross a movement-state transition.
            bool const castControlBlocked = _pendingCast.active || _ownsTimedFlee ||
                _ownsAssistFlight || me->HasUnitState(UNIT_STATE_FLEEING);
            observed.ready = observed.cooldownMs == 0 && !castControlBlocked &&
                !me->IsNonMeleeSpellCast(false) && HasEnoughPower(me, spell);

            std::vector<Unit*> candidates;
            if (behavior->target == "self")
                candidates.push_back(me);
            else if (behavior->target == "ally")
            {
                if (!RequiresOtherOwnedAlly(_policyActorId, spellId))
                    candidates.push_back(me);
                candidates.insert(candidates.end(), ownedHealCandidates.begin(), ownedHealCandidates.end());
            }
            else
            {
                for (WojUnit const& enemy : out.enemies)
                {
                    ObjectGuid guid;
                    if (ParseRawGuid(enemy.id, guid))
                        if (Unit* target = ObjectAccessor::GetUnit(*me, guid))
                            candidates.push_back(target);
                }
            }

            for (Unit* target : candidates)
            {
                if (!target || !target->IsAlive())
                    continue;
                std::string const id = std::to_string(target->GetGUID().GetRawValue());
                bool const selfAllowed = behavior->allowSelf;
                bool const validRelation = behavior->target == "self"
                    ? target == me
                    : behavior->target == "ally"
                        ? ((target == me && selfAllowed) ||
                            (IsOwned(me, target) && me->IsFriendlyTo(target) && me->_IsValidAssistTarget(target, spell)))
                        : (IsThreat(me, target) ? me->CanCreatureAttack(target) : me->CanStartAttack(target));
                bool const beyondSnapshotFloor = !behavior->hasMinSnapshotDistance ||
                    me->GetDistance(target) >= behavior->minSnapshotDistanceYards;
                bool const inRange = validRelation && MatchesTargetSelector(me, behavior, target) &&
                    beyondSnapshotFloor && IsSpellInRange(me, spell, target);
                if (behavior->target == "ally" && target != me && inRange)
                    observedHealAllies.insert(target->GetGUID().GetRawValue());
                // SmartCast AURA_NOT_PRESENT checks any caster. Keeping the
                // old own-caster filter here let two Oracles overwrite the
                // same Renew and made Poison/Infection look available while
                // their aura was already present.
                bool const hasAura = behavior && behavior->requireAuraAbsent
                    ? target->HasAura(spellId) : target->HasAura(spellId, me->GetGUID());
                if (inRange && hasAura)
                    observed.auraTargets.push_back(id);
                if (observed.ready && inRange)
                    observed.validTargets.push_back(id);
            }
            if (behavior->targetSelector == "hostile_random" &&
                observed.validTargets.size() > 1)
            {
                // SmartAI target type 5 samples one random hostile when the
                // action fires. Preserve that server-owned randomness while
                // still giving Jev the tactical CAST/hold choice: the wire
                // exposes exactly the sampled legal target, never a menu that
                // lets the model turn a random selector into victim focus.
                size_t const selected = RollRange(0,
                    static_cast<uint32_t>(observed.validTargets.size() - 1));
                std::string const targetId = observed.validTargets[selected];
                observed.validTargets.assign(1, targetId);
                if (!observed.auraTargets.empty())
                {
                    bool const selectedHasAura = std::find(observed.auraTargets.begin(),
                        observed.auraTargets.end(), targetId) != observed.auraTargets.end();
                    observed.auraTargets.clear();
                    if (selectedHasAura)
                        observed.auraTargets.push_back(targetId);
                }
            }
            auto const castAt = _lastSuccessfulCast.find(spellId);
            if (castAt != _lastSuccessfulCast.end())
            {
                observed.hasLastCastAge = true;
                int64 const age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - castAt->second).count();
                observed.lastCastAgeMs = static_cast<uint32_t>(std::min<int64>(
                    std::max<int64>(age, 0), std::numeric_limits<uint32_t>::max()));
            }
            out.spells.push_back(std::move(observed));
        }
    }
    for (Creature* candidate : ownedHealCandidates)
        if (observedHealAllies.count(candidate->GetGUID().GetRawValue()))
            out.healAllies.push_back(SnapshotAlly(me, candidate));

    auto const now = std::chrono::steady_clock::now();
    for (WojHelpRecord const& request : WojHelp::Instance().Inbox(WojRuntimeKey(me), out.epoch))
    {
        Creature* requester = ObjectAccessor::GetCreature(*me, ObjectGuid(request.requester));
        Unit* target = ObjectAccessor::GetUnit(*me, ObjectGuid(request.target));
        if (!requester || !(WojRuntimeKey(requester) == request.requesterKey) ||
            !target || !(WojRuntimeKey(target) == request.targetKey) ||
            !requester->IsAlive() || !IsOwned(me, requester) ||
            !me->IsFriendlyTo(requester) || me->GetDistance(requester) > ALLY_SEARCH_YARDS ||
            WojRegistry::Instance().CurrentEpoch(request.requesterKey) != request.requesterEpoch ||
            !target->IsAlive() || !IsThreat(requester, target) ||
            !me->IsValidAttackTarget(target) || me->GetDistance(target) > NEIGHBOR_SEARCH_YARDS ||
            !me->CanSeeOrDetect(target) || !me->IsWithinLOSInMap(target))
            continue;

        WojHelpRequest observed;
        observed.requestId = request.id;
        observed.requester = SnapshotAlly(me, requester);
        observed.requesterEpoch = request.requesterEpoch;
        observed.target = SnapshotUnit(me, target, true, AttackingOwned(me, target));
        observed.expiresInMs = static_cast<uint32_t>(std::max<int64>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(request.expiresAt - now).count()));
        out.helpRequests.push_back(std::move(observed));
    }
    RefreshBehavior(out);
}

void WojAI::RefreshBehavior(WojSnapshot& snapshot)
{
    auto const now = std::chrono::steady_clock::now();
    auto setCommon = [&](char const* id, char const* status, char const* blocked)
    {
        if (TriggerRuntime* trigger = FindTrigger(id))
            SetTriggerState(*trigger, status, blocked);
    };

    setCommon("aggro", snapshot.inCombat ? "active" :
        (snapshot.attackTargets.empty() ? "blocked" : "eligible"),
        snapshot.inCombat || !snapshot.attackTargets.empty() ? nullptr : "target");

    bool const hasEngagedEnemy = std::any_of(snapshot.enemies.begin(), snapshot.enemies.end(),
        [](WojUnit const& enemy) { return enemy.engaged; });
    bool const helpCallEligible = snapshot.inCombat && hasEngagedEnemy && !snapshot.callHelpTargets.empty();
    setCommon("help_call", helpCallEligible ? "eligible" : "blocked",
        helpCallEligible ? nullptr : (!snapshot.inCombat ? "combat" : "target"));
    setCommon("help_answer", snapshot.helpRequests.empty() ? "blocked" : "eligible",
        snapshot.helpRequests.empty() ? "target" : nullptr);
    setCommon("routine", snapshot.routine.active ? "active" :
        (snapshot.routine.canResume ? "eligible" : "blocked"),
        snapshot.routine.active || snapshot.routine.canResume ? nullptr :
            (snapshot.inCombat ? "combat" :
                (!snapshot.attackTargets.empty() ? "target" : "physical")));
    bool const returning = me->IsInEvadeMode() ||
        me->GetMotionMaster()->GetCurrentMovementGeneratorType() == HOME_MOTION_TYPE;
    setCommon("return", returning ? "active" :
        (snapshot.inCombat && _hasCombatAnchor ? "eligible" : "blocked"),
        returning || (snapshot.inCombat && _hasCombatAnchor) ? nullptr : "home");

    if (TriggerRuntime* flee = FindTrigger("flee"))
    {
        WojFleeBehavior const& policy = WojConfig::Instance().FleeForActor(_policyActorId);
        flee->view.hasWaitMs = false;
        if (!policy.enabled)
            SetTriggerState(*flee, "blocked", "policy");
        else if (_ownsTimedFlee || _ownsAssistFlight)
            SetTriggerState(*flee, "active", nullptr);
        else if (policy.oncePerEpoch && _fleeConsumedThisEpoch)
            SetTriggerState(*flee, "blocked", "once");
        else if (policy.requireInCombat && !snapshot.inCombat)
            SetTriggerState(*flee, "blocked", "combat");
        else if (snapshot.healthMax && static_cast<uint64_t>(snapshot.health) * 100u >
            static_cast<uint64_t>(snapshot.healthMax) * policy.maxHealthPct)
            SetTriggerState(*flee, "blocked", "health");
        else if (policy.requireNotCasting && me->IsNonMeleeSpellCast(false))
            SetTriggerState(*flee, "blocked", "physical");
        else
        {
            bool eligibleTarget = false;
            for (std::string const& id : snapshot.fleeTargets)
                if (std::any_of(snapshot.enemies.begin(), snapshot.enemies.end(),
                    [&](WojUnit const& enemy)
                    {
                        return enemy.id == id && (!policy.requireEngaged || enemy.engaged) &&
                            enemy.distance <= policy.maxDistanceYards;
                    }))
                {
                    eligibleTarget = true;
                    break;
                }
            SetTriggerState(*flee, eligibleTarget ? "eligible" : "blocked",
                eligibleTarget ? nullptr : "target");
        }
    }

    if (TriggerRuntime* talk = FindTrigger("ambient_talk"))
    {
        int64_t const remaining = talk->timerPaused ? talk->view.waitMs :
            std::chrono::duration_cast<std::chrono::milliseconds>(talk->deadline - now).count();
        talk->view.hasWaitMs = true;
        talk->view.waitMs = static_cast<uint32_t>(std::max<int64_t>(remaining, 0));
        if (!talk->timerArmed || !IsAmbientTalker())
            SetTriggerState(*talk, "blocked", "policy");
        else if (snapshot.inCombat)
            SetTriggerState(*talk, "blocked", "combat");
        else if (remaining > 0)
            SetTriggerState(*talk, "waiting", "timer");
        else if (_pendingCast.active || _ownsTimedFlee || _ownsAssistFlight ||
            me->HasUnitState(UNIT_STATE_FLEEING))
            SetTriggerState(*talk, "blocked", _pendingCast.active ? "physical" : "control");
        else if (!talk->chancePassed)
        {
            if (RollRange(1, 100) > talk->chancePct)
            {
                AppendBehaviorEvent(talk->view.id, talk->view.generation,
                    "chance_skipped", "", std::to_string(talk->chancePct) + "_percent");
                ArmTimedTrigger(*talk, false, "chance_skipped");
            }
            else
            {
                talk->chancePassed = true;
                SetTriggerState(*talk, "eligible", nullptr);
            }
        }
        else
            SetTriggerState(*talk, "eligible", nullptr);
    }

    for (TriggerRuntime& trigger : _triggers)
    {
        if (!trigger.spell || trigger.ambientTalk)
            continue;
        if (trigger.oncePerEpoch && trigger.eventConsumedThisEpoch)
        {
            trigger.view.hasWaitMs = false;
            trigger.view.waitMs = 0;
            SetTriggerState(trigger, "blocked", "once");
            continue;
        }
        if (!trigger.eventDriven && !trigger.timerArmed)
        {
            trigger.view.hasWaitMs = true;
            trigger.view.waitMs = 0;
            SetTriggerState(trigger, "blocked", "combat");
            continue;
        }
        int64_t const remaining = trigger.eventDriven ? 0 : (trigger.timerPaused ? trigger.view.waitMs :
            std::chrono::duration_cast<std::chrono::milliseconds>(trigger.deadline - now).count());
        trigger.view.hasWaitMs = !trigger.eventDriven;
        trigger.view.waitMs = static_cast<uint32_t>(std::max<int64_t>(remaining, 0));
        if (_pendingCast.active && _pendingCast.spell == trigger.spell &&
            _pendingCast.generation == trigger.view.generation)
        {
            SetTriggerState(trigger, "active", nullptr);
            continue;
        }
        if (!trigger.eventDriven && remaining > 0)
        {
            SetTriggerState(trigger, "waiting", "timer");
            continue;
        }
        WojSpellBehavior const* policy = WojConfig::Instance().FindBehaviorSpellForActor(_policyActorId, trigger.spell);
        if (!policy)
        {
            SetTriggerState(trigger, "blocked", "physical");
            continue;
        }
        if (policy->requireCasterCombat && !snapshot.inCombat)
        {
            SetTriggerState(trigger, "blocked", "combat");
            continue;
        }
        if (policy->requireCasterOutOfCombat && snapshot.inCombat)
        {
            SetTriggerState(trigger, "blocked", "combat");
            continue;
        }
        if (policy->hasMaxCasterHealthPct && (!snapshot.healthMax ||
            static_cast<uint64_t>(snapshot.health) * 100u >=
                static_cast<uint64_t>(snapshot.healthMax) * policy->maxCasterHealthPct))
        {
            // Stock HealthBelowPct is strict: exactly 33% is not eligible.
            SetTriggerState(trigger, "blocked", "health");
            continue;
        }
        if (_pendingCast.active || _ownsTimedFlee || _ownsAssistFlight ||
            me->HasUnitState(UNIT_STATE_FLEEING))
        {
            // Keep the elapsed stock timer armed.  Once the current cast or
            // flee/control motion ends this exact generation becomes eligible
            // again.  Never advertise overlapping casts to an async model.
            SetTriggerState(trigger, "blocked", _pendingCast.active ? "physical" : "control");
            continue;
        }

        WojSpell const* observed = nullptr;
        for (WojSpell const& spell : snapshot.spells)
            if (spell.id == trigger.spell)
            {
                observed = &spell;
                break;
            }
        if (!observed || !observed->ready)
        {
            SetTriggerState(trigger, "blocked", "physical");
            continue;
        }
        auto auraAbsent = [&](std::string const& id)
        {
            return std::find(observed->auraTargets.begin(), observed->auraTargets.end(), id) ==
                observed->auraTargets.end();
        };
        bool eligibleTarget = false;
        for (std::string const& id : observed->validTargets)
        {
            float distance = 0.0f;
            bool targetCombat = false;
            bool engaged = false;
            bool found = false;
            float healthPct = 100.0f;
            if (policy->target == "enemy")
            {
                auto const enemy = std::find_if(snapshot.enemies.begin(), snapshot.enemies.end(),
                    [&](WojUnit const& value) { return value.id == id; });
                if (enemy != snapshot.enemies.end())
                {
                    found = true;
                    distance = enemy->distance;
                    engaged = enemy->engaged;
                    healthPct = enemy->healthPct;
                }
            }
            else
            {
                if (id == snapshot.id)
                {
                    found = true;
                    targetCombat = snapshot.inCombat;
                    healthPct = snapshot.healthMax ? 100.0f * snapshot.health / snapshot.healthMax : 0.0f;
                }
                else
                {
                    auto const ally = std::find_if(snapshot.healAllies.begin(), snapshot.healAllies.end(),
                        [&](WojAlly const& value) { return value.id == id; });
                    if (ally != snapshot.healAllies.end())
                    {
                        found = true;
                        distance = ally->distance;
                        targetCombat = ally->inCombat;
                        healthPct = ally->healthPct;
                    }
                }
            }
            eligibleTarget = found && (!policy->requireTargetInCombat || targetCombat) &&
                (!policy->requireEngaged || engaged) &&
                (!policy->hasMaxDistance || distance <= policy->maxDistanceYards) &&
                (!policy->requireAuraAbsent || auraAbsent(id)) &&
                (!policy->hasMaxTargetHealthPct || healthPct <= policy->maxTargetHealthPct);
            if (eligibleTarget)
                break;
        }
        if (!eligibleTarget)
        {
            SetTriggerState(trigger, "blocked", policy->hasMaxTargetHealthPct ? "health" : "target");
            continue;
        }
        if (!trigger.chancePassed)
        {
            if (RollRange(1, 100) > trigger.chancePct)
            {
                AppendBehaviorEvent(trigger.view.id, trigger.view.generation,
                    "chance_skipped", "", std::to_string(trigger.chancePct) + "_percent");
                ArmTimedTrigger(trigger, false, "chance_skipped");
                continue;
            }
            trigger.chancePassed = true;
        }
        SetTriggerState(trigger, "eligible", nullptr);
    }

    snapshot.behavior.seq = _behaviorSeq;
    snapshot.behavior.floorSeq = _behaviorEvents.empty() ? _behaviorSeq + 1 : _behaviorEvents.front().seq;
    snapshot.behavior.events.assign(_behaviorEvents.begin(), _behaviorEvents.end());
    snapshot.behavior.triggers.reserve(_triggers.size());
    for (TriggerRuntime const& trigger : _triggers)
        snapshot.behavior.triggers.push_back(trigger.view);
}

bool WojAI::ValidateTriggeredAction(WojAction const& action, Unit const* target,
                                    TriggerRuntime*& trigger, std::string& reason)
{
    std::string const id = action.kind == WojActionKind::Cast
        ? TriggerIdForSpell(action.spell) : "flee";
    trigger = FindTrigger(id);
    if (!trigger || action.triggerGeneration == 0 ||
        trigger->view.generation != action.triggerGeneration)
    {
        reason = "trigger generation is stale or unknown";
        return false;
    }

    if (action.kind == WojActionKind::Flee || action.kind == WojActionKind::FleeForAssist)
    {
        WojFleeBehavior const& policy = WojConfig::Instance().FleeForActor(_policyActorId);
        bool const lowHealth = me->GetMaxHealth() &&
            static_cast<uint64_t>(me->GetHealth()) * 100u <=
                static_cast<uint64_t>(me->GetMaxHealth()) * policy.maxHealthPct;
        bool const engaged = target && IsThreat(me, target);
        if (!policy.enabled || (policy.requireInCombat && !me->IsInCombat()) || !lowHealth ||
            (policy.oncePerEpoch && _fleeConsumedThisEpoch) ||
            (policy.requireNotCasting && me->IsNonMeleeSpellCast(false)) ||
            !target || (policy.requireEngaged && !engaged) ||
            me->GetDistance(target) > policy.maxDistanceYards)
        {
            reason = "flee trigger predicate is no longer eligible";
            return false;
        }
        return true;
    }

    auto const now = std::chrono::steady_clock::now();
    WojSpellBehavior const* policy = WojConfig::Instance().FindBehaviorSpellForActor(_policyActorId, action.spell);
    bool const timerWaiting = !trigger->eventDriven &&
        (trigger->timerPaused ? trigger->view.waitMs > 0 : trigger->deadline > now);
    if (!policy || policy->entry != me->GetEntry() ||
        (trigger->oncePerEpoch && trigger->eventConsumedThisEpoch) ||
        (!trigger->eventDriven && !trigger->timerArmed) ||
        timerWaiting || _pendingCast.active || _ownsTimedFlee || _ownsAssistFlight ||
        me->HasUnitState(UNIT_STATE_FLEEING) || !target ||
        (policy->requireCasterCombat && !me->IsInCombat()) ||
        (policy->requireCasterOutOfCombat && me->IsInCombat()))
    {
        reason = "spell trigger is waiting, active, or no longer in combat";
        return false;
    }

    bool const friendly = target == me || (IsOwned(me, target) && me->IsFriendlyTo(target));
    bool const relationMatches = policy->target == "self"
        ? target == me
        : policy->target == "ally" ? friendly : !friendly;
    if (!relationMatches)
    {
        reason = "selected target relation no longer matches trigger policy";
        return false;
    }
    if (!MatchesTargetSelector(me, policy, target))
    {
        reason = "selected target no longer matches configured stock selector";
        return false;
    }
    if (target == me && !policy->allowSelf)
    {
        reason = "self target is forbidden by trigger policy";
        return false;
    }
    if (policy->requireTargetInCombat && !target->IsInCombat())
    {
        reason = "selected target is no longer in combat";
        return false;
    }
    if (policy->requireEngaged && !IsThreat(me, target))
    {
        reason = "selected target is no longer engaged";
        return false;
    }
    if (policy->hasMaxDistance && me->GetDistance(target) > policy->maxDistanceYards)
    {
        reason = "selected target exceeds trigger policy distance";
        return false;
    }
    if (policy->requireAuraAbsent && target->HasAura(action.spell))
    {
        reason = "selected target already has the spell aura";
        return false;
    }
    if (policy->hasMaxTargetHealthPct && (!target->GetMaxHealth() ||
        static_cast<uint64_t>(target->GetHealth()) * 100u >
            static_cast<uint64_t>(target->GetMaxHealth()) * policy->maxTargetHealthPct))
    {
        reason = "selected target exceeds trigger policy health";
        return false;
    }
    if (policy->hasMaxCasterHealthPct && (!me->GetMaxHealth() ||
        static_cast<uint64_t>(me->GetHealth()) * 100u >=
            static_cast<uint64_t>(me->GetMaxHealth()) * policy->maxCasterHealthPct))
    {
        reason = "caster no longer satisfies trigger policy health";
        return false;
    }
    if (!trigger->chancePassed)
    {
        reason = "poison chance has not passed";
        return false;
    }
    return true;
}

bool WojAI::IsAggroTalker() const
{
    WojConfig const& config = WojConfig::Instance();
    WojAggroTalkBehavior const& policy = config.AggroTalkForActor(_policyActorId);
    return config.ActorAggroTalk(_policyActorId) && policy.enabled &&
        sCreatureTextMgr->TextExist(me->GetEntry(), policy.textGroup);
}

bool WojAI::IsAmbientTalker() const
{
    WojConfig const& config = WojConfig::Instance();
    WojAmbientTalkBehavior const& policy = config.AmbientTalkForActor(_policyActorId);
    return config.ActorAmbientTalk(_policyActorId) && policy.enabled &&
        sCreatureTextMgr->TextExist(me->GetEntry(), policy.textGroup);
}

void WojAI::AttemptAggroTalk()
{
    if (!IsAggroTalker() || _aggroTalkAttemptedThisCombat)
        return;
    _aggroTalkAttemptedThisCombat = true;
    TriggerRuntime* trigger = FindTrigger("aggro_talk");
    if (!trigger)
        return;
    uint32_t const chancePct = WojConfig::Instance().AggroTalkForActor(_policyActorId).chancePct;
    if (urand(1, 100) > chancePct)
    {
        trigger->view.status = "blocked";
        trigger->view.blockedBy = "chance";
        AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
            "chance_skipped", "", std::to_string(chancePct) + "_percent");
        return;
    }
    SetTriggerState(*trigger, "eligible", nullptr);
}

void WojAI::FailPendingCast(char const* kind, char const* reason)
{
    if (!_pendingCast.active)
        return;
    PendingCast const pending = _pendingCast;
    if (pending.permitsContinuation)
        ClearJevCastContinuation();
    if (pending.expectedSummonActorId)
        WojRuntimeBinding::Instance().FinishSummonPermit(
            me, pending.spell, pending.expectedSummonActorId, pending.expectedSummonCount);
    _pendingCast = PendingCast{};
    _pendingHeal = PendingHeal{};
    if (TriggerRuntime* trigger = FindTrigger(TriggerIdForSpell(pending.spell));
        trigger && trigger->view.generation == pending.generation)
    {
        trigger->view.status = "eligible";
        trigger->view.blockedBy.clear();
        AppendBehaviorEvent(trigger->view.id, pending.generation, kind,
            pending.decisionId, reason);
    }
}

void WojAI::SucceedPendingCast(uint32_t spellId)
{
    if (!_pendingCast.active || _pendingCast.spell != spellId ||
        _pendingCast.epoch != WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)))
        return;
    PendingCast const pending = _pendingCast;
    if (pending.expectedSummonActorId &&
        !WojRuntimeBinding::Instance().FinishSummonPermit(
            me, pending.spell, pending.expectedSummonActorId, pending.expectedSummonCount))
    {
        if (pending.permitsContinuation)
            ClearJevCastContinuation();
        _pendingCast = PendingCast{};
        _pendingHeal = PendingHeal{};
        if (TriggerRuntime* trigger = FindTrigger(TriggerIdForSpell(spellId));
            trigger && trigger->view.generation == pending.generation)
        {
            trigger->view.status = "eligible";
            trigger->view.blockedBy.clear();
            AppendBehaviorEvent(trigger->view.id, pending.generation, "failed",
                pending.decisionId, "summon_mismatch");
        }
        LOG_ERROR("module", "mod-world-of-jevs: tactical cast {} by actor {} did not create its exact configured runtime summon {}",
            pending.spell, _policyActorId, pending.expectedSummonActorId);
        return;
    }
    _pendingCast = PendingCast{};
    // OnSpellCast идёт после effect hooks. Для 913 provenance уже записан
    // или честно отсутствует; токен нельзя переносить на следующий cast.
    _pendingHeal = PendingHeal{};
    _lastSuccessfulCast[spellId] = std::chrono::steady_clock::now();
    if (TriggerRuntime* trigger = FindTrigger(TriggerIdForSpell(spellId));
        trigger && trigger->view.generation == pending.generation)
    {
        AppendBehaviorEvent(trigger->view.id, pending.generation, "succeeded",
            pending.decisionId, "spell_cast");
        if (trigger->oncePerEpoch)
        {
            trigger->eventConsumedThisEpoch = true;
            trigger->view.hasWaitMs = false;
            trigger->view.waitMs = 0;
            trigger->view.status = "blocked";
            trigger->view.blockedBy = "once";
        }
        else
            ArmTimedTrigger(*trigger, false, "success");
        if (pending.permitsContinuation && pending.attemptedAt != std::chrono::steady_clock::time_point{})
        {
            // SmartScript's UPDATE_IC path calls ProcessAction and then
            // RecalcTimer in the same timed-event turn.  For this one
            // opt-in continuation, preserve that cast-start anchor instead
            // of charging the source 2s repeat again after OnSpellCast's
            // acknowledgement delay.
            trigger->deadline = pending.attemptedAt + std::chrono::milliseconds(trigger->view.waitMs);
            int64_t const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                trigger->deadline - std::chrono::steady_clock::now()).count();
            trigger->view.waitMs = static_cast<uint32_t>(std::max<int64_t>(remaining, 0));
        }
        if (pending.permitsContinuation && !trigger->oncePerEpoch && trigger->timerArmed &&
            !trigger->timerPaused)
        {
            _jevCastContinuation.active = true;
            _jevCastContinuation.epoch = pending.epoch;
            _jevCastContinuation.spell = pending.spell;
            _jevCastContinuation.generation = trigger->view.generation;
            _jevCastContinuation.target = pending.target;
            _jevCastContinuation.decisionId = pending.decisionId;
            _jevCastContinuation.marker = pending.marker;
        }
        else
            ClearJevCastContinuation();
    }
    else
        ClearJevCastContinuation();
}

void WojAI::OnSpellCast(SpellInfo const* spell)
{
    if (spell)
        SucceedPendingCast(spell->Id);
}

void WojAI::JustEngagedWith(Unit* who)
{
    _observedAggroTarget.Clear();
    CancelAggroEventContinuation("new_combat_entry");
    // Core supplies the exact unit which caused this physical combat entry.
    // Never substitute a later victim/nearest enemy for this identity.
    if (who)
        _aggroInvoker = who->GetGUID();
    else
        _aggroInvoker.Clear();
    _aggroEventArmed = who && who->IsAlive() && me->IsValidAttackTarget(who);
    _consumedAggroEvents.clear();
    for (WojAggroEventBehavior const& event : WojConfig::Instance().AggroEventsForActor(_policyActorId))
        if (TriggerRuntime* trigger = FindTrigger("aggro_event_" + event.id))
        {
            ++trigger->view.generation;
            SetTriggerState(*trigger, "blocked", "combat", "rearmed", "", "combat_enter");
        }
}

void WojAI::ArmHealingWaveProvenance(Unit* first, Unit* second, SpellInfo const* spell)
{
    if (!spell || !_pendingCast.active || _pendingCast.spell != spell->Id ||
        _pendingCast.epoch != WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)) ||
        _pendingHeal.active)
        return;
    WojSpellBehavior const* policy = WojConfig::Instance().FindBehaviorSpellForActor(_policyActorId, spell->Id);
    if (!policy || policy->completion != "positive_heal")
        return;
    // Unit::HealBySpell и periodic aura передают callback аргументы в разном
    // порядке. Не доверяем именам формальных параметров core.
    Unit* other = first == me ? second : (second == me ? first : nullptr);
    if (!other || std::to_string(other->GetGUID().GetRawValue()) != _pendingCast.target)
        return;
    _pendingHeal.active = true;
    _pendingHeal.epoch = _pendingCast.epoch;
    _pendingHeal.spell = _pendingCast.spell;
    _pendingHeal.generation = _pendingCast.generation;
    _pendingHeal.decisionId = _pendingCast.decisionId;
    _pendingHeal.target = _pendingCast.target;
}

bool WojAI::ConsumeHealingWaveProvenance(Unit* healer, Unit* receiver, uint32_t gain)
{
    if (!_pendingHeal.active)
        return false;
    // Stock NO_REPEAT consumes a successful cast even when another effect
    // raced health to full. Keep the token until OnSpellCast clears it, but
    // never turn zero effective gain into acceptance evidence.
    if (!gain)
        return false;
    PendingHeal const pending = _pendingHeal;
    _pendingHeal = PendingHeal{}; // exact-once for a positive actual gain
    if (pending.epoch != WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)) ||
        healer != me || !receiver || std::to_string(receiver->GetGUID().GetRawValue()) != pending.target)
        return false;
    WojCombatLog::Instance().WriteHealingWaveProvenance(me, receiver, gain, pending.spell,
        pending.decisionId, pending.epoch, pending.generation);
    return true;
}

void WojAI::NotifySpellCastCancelled(uint32_t spellId)
{
    if (_pendingCast.active && _pendingCast.spell == spellId)
        FailPendingCast("cancelled", "core_cancel");
}

void WojAI::PushAction(std::string marker)
{
    _lastActions.push_back(std::move(marker));
    while (_lastActions.size() > MAX_LAST_ACTIONS)
        _lastActions.pop_front();
}

bool WojAI::CancelOwnFlee()
{
    MotionMaster* motion = me->GetMotionMaster();
    // Аура страха принадлежит ядру, не Jev. Её нельзя снять новым решением.
    if (me->HasFearAura())
    {
        if (_ownsAssistFlight && motion->GetMotionSlotType(MOTION_SLOT_ACTIVE) == POINT_MOTION_TYPE)
            motion->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
        _ownsTimedFlee = false;
        _ownsAssistFlight = false;
        return false;
    }
    if (_ownsTimedFlee && motion->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == TIMED_FLEEING_MOTION_TYPE)
        motion->MovementExpiredOnSlot(MOTION_SLOT_CONTROLLED, false);
    _ownsTimedFlee = false;
    if (_ownsAssistFlight)
    {
        if (motion->GetMotionSlotType(MOTION_SLOT_ACTIVE) == POINT_MOTION_TYPE)
            motion->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
        me->RemoveUnitFlag(UNIT_FLAG_FLEEING);
        me->ClearUnitState(UNIT_STATE_FLEEING | UNIT_STATE_FLEEING_MOVE);
        _ownsAssistFlight = false;
    }
    return true;
}

bool WojAI::RememberOneShot(std::string const& decisionId)
{
    if (decisionId.empty() || !_oneShotDecisions.insert(decisionId).second)
        return false;
    _oneShotOrder.push_back(decisionId);
    while (_oneShotOrder.size() > MAX_ONE_SHOT_DECISIONS)
    {
        _oneShotDecisions.erase(_oneShotOrder.front());
        _oneShotOrder.pop_front();
    }
    return true;
}

void WojAI::ResetSuppressedNone()
{
    // M4 (round 3): the NEW-2 reset used to live only in ApplyAction, but
    // ApplyAction is not called at all while the kill switch is open
    // (UpdateAI returns before TakeAction) - so a streak frozen mid-outage
    // by WorldOfJevs.Enable=0 kept its stale count and its stale
    // _lastNoneLog across the whole disabled period, and handed both to
    // whatever streak started (or was still running) once the module came
    // back, hours later. That is NEW-2's exact wrong output, reached
    // through a door NEW-2 did not check. _lastNoneLog resets to its
    // default-constructed value, not now(): there is no real log time to
    // speak of until a streak actually prints something.
    _suppressedNoneCount = 0;
    _lastNoneLog = std::chrono::steady_clock::time_point{};
}

bool WojAI::IsJevCastContinuationEnabled(uint32_t spell) const
{
    WojActorBinding const* binding = WojConfig::Instance().FindActor(_policyActorId);
    return binding && binding->entry == me->GetEntry() && binding->mapId == me->GetMapId() &&
        binding->hasExecutorRepeatContinuationSpell &&
        binding->executorRepeatContinuationSpell == spell;
}

void WojAI::ClearJevCastContinuation()
{
    _jevCastContinuation = JevCastContinuation{};
    _secondaryDecisionWaitUntil = {};
}

bool WojAI::ContinueJevCast()
{
    if (!_jevCastContinuation.active)
        return false;
    JevCastContinuation const continuation = _jevCastContinuation;
    if (continuation.epoch != WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)) ||
        !IsJevCastContinuationEnabled(continuation.spell))
    {
        ClearJevCastContinuation();
        return false;
    }

    TriggerRuntime* armed = FindTrigger(TriggerIdForSpell(continuation.spell));
    if (!armed || armed->view.generation != continuation.generation || !armed->timerArmed ||
        armed->eventDriven || armed->timerPaused)
    {
        // A combat exit pauses an in-combat source timer. This continuation
        // must not survive it and later resume as if Jev had chosen again.
        ClearJevCastContinuation();
        return false;
    }
    auto const now = std::chrono::steady_clock::now();
    if (armed->deadline > now)
        return false; // still the exact same Jev intent; do not validate/clear it yet
    if (_secondaryDecisionWaitUntil != std::chrono::steady_clock::time_point{})
    {
        if (now < _secondaryDecisionWaitUntil)
            return false; // keep the old Jev intent while the fresh answer is in flight
        _secondaryDecisionWaitUntil = {}; // no usable answer; resume the old intent
    }

    // A repeat chosen by Jev must not permanently hide another timed spell.
    // Yield at the source timer boundary and ask Jev again with both spells
    // visible. This does not choose the other spell or repeat Shoot for Jev;
    // either physical cast still needs a usable Jev decision (or the already
    // authorized Shoot continuation when no other spell is due).
    // If Jev prefers Shoot or its request fails, keep the old authorization
    // for subsequent repeats. The same overdue secondary timer must not
    // force a remote request on every 2-second Shoot boundary.
    if (now >= _nextSecondaryYieldAt)
    {
        for (TriggerRuntime const& other : _triggers)
        {
            if (!other.spell || other.spell == continuation.spell || other.ambientTalk ||
                other.eventDriven || !other.timerArmed || other.timerPaused ||
                other.deadline > now || (other.oncePerEpoch && other.eventConsumedThisEpoch))
                continue;
            WojSpellBehavior const* policy =
                WojConfig::Instance().FindBehaviorSpellForActor(_policyActorId, other.spell);
            SpellInfo const* spell = policy ? sSpellMgr->GetSpellInfo(other.spell) : nullptr;
            if (!spell || me->HasSpellCooldown(other.spell) || !HasEnoughPower(me, spell))
                continue;
            _nextSecondaryYieldAt = now + std::chrono::milliseconds(
                static_cast<int64_t>(WojConfig::Instance().CombatTickMs) * 3);
            _secondaryDecisionWaitUntil = now +
                std::chrono::milliseconds(WojConfig::Instance().TimeoutMs);
            _sinceDecision = std::max(_sinceDecision, WojConfig::Instance().CombatTickMs);
            LOG_INFO("module", "mod-world-of-jevs: actor {} yielded cast continuation {} to due spell {} for a fresh Jev decision",
                _policyActorId, continuation.spell, other.spell);
            return false;
        }
    }

    ObjectGuid guid;
    Unit* target = ParseRawGuid(continuation.target, guid) ? ObjectAccessor::GetUnit(*me, guid) : nullptr;
    WojAction action;
    action.kind = WojActionKind::Cast;
    action.spell = continuation.spell;
    action.target = continuation.target;
    action.triggerGeneration = continuation.generation;
    action.decisionId = continuation.decisionId;
    action.marker = continuation.marker;
    action.tier = "jev";
    TriggerRuntime* trigger = nullptr;
    std::string reason;
    // This is the same final predicate used for a new gateway CAST. The
    // normal CAST path below additionally checks cooldown, GCD/power, range
    // and LOS immediately before invoking core.
    if (!target || !target->IsAlive() ||
        !ValidateTriggeredAction(action, target, trigger, reason))
    {
        ClearJevCastContinuation();
        return false;
    }

    // A physical attempt consumes this token. A successful OnSpellCast arms
    // the next one; a rejection/cancel never becomes an executor retry.
    ClearJevCastContinuation();
    ApplyAction(action, true);
    return true;
}

void WojAI::RejectAction(WojAction const& action, char const* kind, std::string const& reason,
                         std::string const& helpRequestId, bool recordTrigger)
{
    if (recordTrigger && !action.decisionId.empty())
    {
        std::string const id = action.kind == WojActionKind::Cast ? TriggerIdForSpell(action.spell) :
            (action.kind == WojActionKind::Flee || action.kind == WojActionKind::FleeForAssist) ? "flee" :
            action.kind == WojActionKind::Attack ? "aggro" :
            action.kind == WojActionKind::CallHelp ? "help_call" :
            action.kind == WojActionKind::AnswerHelp ? "help_answer" :
            action.kind == WojActionKind::StockTalk ? action.triggerId :
            action.kind == WojActionKind::StartPhase ? "health_phase_" + action.phaseId :
            action.kind == WojActionKind::HealthEvent ? "health_event_" + action.healthEventId :
            action.kind == WojActionKind::AggroEvent ? "aggro_event_" + action.aggroEventId :
            action.kind == WojActionKind::ResumeRoutine ? "routine" :
            action.kind == WojActionKind::Evade ? "return" : "";
        if (TriggerRuntime* trigger = FindTrigger(id); trigger)
            AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
                "rejected", action.decisionId, reason);
    }
    LOG_WARN("module", "mod-world-of-jevs: guid {} rejected {}: {}",
             _policyActorId, kind, reason);
    WojCombatLog::Instance().WriteActionRejected(me, kind, reason, action.decisionId, helpRequestId);
}

WojPoint const& WojAI::MovementAnchor() const
{
    return _hasCombatAnchor ? _combatAnchor : _spawnOrigin;
}

bool WojAI::ResumeRoutine(WojAction const& action)
{
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    bool const routineMovementUnavailable = _routineProfile != WojRoutineProfile::Idle &&
        (me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) || me->GetSpeed(MOVE_WALK) < 0.1f);
    if (me->IsInCombat() || !me->IsAlive() || HasExternalMovementControl(me) ||
        routineMovementUnavailable)
    {
        RejectAction(action, "RESUME_ROUTINE", "routine cannot resume during combat or movement control");
        return false;
    }

    bool const wasInstalled = _routineInstalled;
    MotionMaster* motion = me->GetMotionMaster();
    MovementGeneratorType expectedMotion = IDLE_MOTION_TYPE;
    if (_routineProfile == WojRoutineProfile::Random)
        expectedMotion = RANDOM_MOTION_TYPE;
    else if (_routineProfile == WojRoutineProfile::Waypoint)
        expectedMotion = WAYPOINT_MOTION_TYPE;
    if (_routineInstalled && _routineAuthorizedEpoch == epoch &&
        motion->GetMotionSlotType(MOTION_SLOT_IDLE) == expectedMotion)
        return true;
    switch (_routineProfile)
    {
        case WojRoutineProfile::Random:
            if (motion->GetMotionSlotType(MOTION_SLOT_IDLE) != RANDOM_MOTION_TYPE)
                motion->MoveRandom(_routineWanderRadius);
            break;
        case WojRoutineProfile::Waypoint:
            if (!_routinePathId || !_routinePointCount)
            {
                RejectAction(action, "RESUME_ROUTINE", "configured waypoint path is unavailable");
                return false;
            }
            if (motion->GetMotionSlotType(MOTION_SLOT_IDLE) != WAYPOINT_MOTION_TYPE)
                motion->MoveWaypoint(_routinePathId, true);
            else if (MovementGenerator* generator = motion->GetMotionSlot(MOTION_SLOT_IDLE))
                generator->Resume(0);
            break;
        case WojRoutineProfile::Idle:
            if (motion->GetMotionSlotType(MOTION_SLOT_IDLE) != IDLE_MOTION_TYPE)
                motion->MoveIdle();
            break;
    }

    _routineInstalled = true;
    _routineAuthorizedEpoch = epoch;
    WojCombatLog::Instance().WriteRoutineEvent(wasInstalled ? "routine_resume" : "routine_start",
        me, epoch, RoutineProfileName(_routineProfile), _routinePathId, action.decisionId, "decision");
    if (TriggerRuntime* trigger = FindTrigger("routine"))
        AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
            "succeeded", action.decisionId, wasInstalled ? "resumed" : "started");
    return true;
}

void WojAI::StopRoutine(char const* reason)
{
    ReleaseRoutineSuspension();
    if (!_routineInstalled)
        return;
    WojCombatLog::Instance().WriteRoutineEvent("routine_stop", me,
        WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)), RoutineProfileName(_routineProfile),
        _routinePathId, "", reason);
    _routineInstalled = false;
    _routineAuthorizedEpoch = 0;
}

void WojAI::RestoreRoutineMotion()
{
    ReleaseRoutineSuspension();
    MotionMaster* motion = me->GetMotionMaster();
    motion->Clear(true);
    if (!_routineInstalled)
        motion->MoveIdle();
    else if (_routineAuthorizedEpoch == WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)) &&
        _routineProfile == WojRoutineProfile::Waypoint &&
        motion->GetMotionSlotType(MOTION_SLOT_IDLE) == WAYPOINT_MOTION_TYPE)
        if (MovementGenerator* generator = motion->GetMotionSlot(MOTION_SLOT_IDLE))
            generator->Resume(0);
}

void WojAI::ReleaseRoutineSuspension()
{
    ReleaseRoutineMoveBlock();
    if (!_routineSuspendedForCombat)
        return;
    if (_routineProfile == WojRoutineProfile::Waypoint &&
        me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_IDLE) == WAYPOINT_MOTION_TYPE)
        if (MovementGenerator* generator = me->GetMotionMaster()->GetMotionSlot(MOTION_SLOT_IDLE))
            generator->Resume(0);
    _routineSuspendedForCombat = false;
}

void WojAI::ReleaseRoutineMoveBlock()
{
    if (!_ownsRoutineMoveBlock)
        return;
    me->RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE);
    _ownsRoutineMoveBlock = false;
}

void WojAI::SuspendRoutineForCombat()
{
    if (!_routineInstalled)
        return;
    MotionMaster* motion = me->GetMotionMaster();
    if (!_routineSuspendedForCombat && _routineProfile == WojRoutineProfile::Waypoint &&
        motion->GetMotionSlotType(MOTION_SLOT_IDLE) == WAYPOINT_MOTION_TYPE)
    {
        if (MovementGenerator* generator = motion->GetMotionSlot(MOTION_SLOT_IDLE))
            generator->Pause(0);
    }
    MovementGeneratorType const current = motion->GetCurrentMovementGeneratorType();
    if (!_routineSuspendedForCombat && current == WAYPOINT_MOTION_TYPE)
        me->StopMoving();
    if (_routineProfile == WojRoutineProfile::Random && current == RANDOM_MOTION_TYPE &&
        !me->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
    {
        me->SetUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        _ownsRoutineMoveBlock = true;
        me->StopMoving();
    }
    _routineSuspendedForCombat = true;
}

void WojAI::ApplyAction(WojAction const& action, bool continuation)
{
    // I3: whether the PREVIOUS action was also a miss, captured before the
    // switch below updates _inNoneStreak for next time. This is what lets
    // the None case tell "gateway just failed" (log now, unconditionally)
    // apart from "gateway is still down from a minute ago" (cooldown may
    // apply) - a plain cooldown timer alone cannot tell those two apart,
    // because it does not know whether anything succeeded in between.
    bool const wasInNoneStreak = _inNoneStreak;
    _inNoneStreak = (action.kind == WojActionKind::None);

    // NEW-2 (round 2)/M4 (round 3): a suppressed run that ends before its
    // cooldown expires must not survive into the next, unrelated outage -
    // see ResetSuppressedNone() for the reasoning. This is the "gateway
    // recovered" half of when a streak ends; UpdateAI's kill switch is the
    // other half.
    if (!_inNoneStreak)
        ResetSuppressedNone();

    if (!continuation && action.kind != WojActionKind::None)
    {
        _secondaryDecisionWaitUntil = {};
        bool const refreshesSameIntent = action.kind == WojActionKind::Cast && action.tier == "jev" &&
            _jevCastContinuation.active && action.spell == _jevCastContinuation.spell &&
            action.target == _jevCastContinuation.target && IsJevCastContinuationEnabled(action.spell);
        bool const maintainsShootEngagement = action.kind == WojActionKind::Attack && action.tier == "jev" &&
            _jevCastContinuation.active && action.target == _jevCastContinuation.target &&
            IsJevCastContinuationEnabled(_jevCastContinuation.spell);
        if (refreshesSameIntent)
        {
            // A newer Jev answer chose the exact same tactical action before
            // the stock timer elapsed. Keep its current intent, but retain
            // the timer already armed by the physical cast.
            if (action.triggerGeneration == _jevCastContinuation.generation)
            {
                _jevCastContinuation.decisionId = action.decisionId;
                _jevCastContinuation.marker = action.marker;
                TriggerRuntime* trigger = FindTrigger(TriggerIdForSpell(action.spell));
                if (trigger && trigger->timerArmed && !trigger->timerPaused &&
                    trigger->deadline > std::chrono::steady_clock::now())
                {
                    // This is a Jev refresh, not a physical cast attempt.
                    // In particular, do not manufacture an action_rejected
                    // record merely because the source repeat timer is not
                    // due yet.
                    return;
                }
            }
        }
        else if (!maintainsShootEngagement)
            ClearJevCastContinuation();
    }

    bool const oneShot = action.kind == WojActionKind::Say || action.kind == WojActionKind::StockTalk ||
        action.kind == WojActionKind::Cast || action.kind == WojActionKind::StartPhase ||
        action.kind == WojActionKind::AggroEvent ||
        action.kind == WojActionKind::CallHelp || action.kind == WojActionKind::FleeForAssist ||
        action.kind == WojActionKind::Flee || action.kind == WojActionKind::AnswerHelp ||
        action.kind == WojActionKind::Evade || action.kind == WojActionKind::ResumeRoutine;
    if (oneShot && action.decisionId.empty())
    {
        RejectAction(action, "ONE_SHOT", "empty decision_id");
        return;
    }
    if (!continuation && oneShot && _oneShotDecisions.count(action.decisionId))
        return;

    switch (action.kind)
    {
        case WojActionKind::MoveTo:
        {
            // This is the one place external data touches the world, and
            // until now the only one without a check. (0,0,0) is a valid
            // map coordinate, so a hallucinated {"x":0,"y":0,"z":0} from the
            // gateway would otherwise send the murloc pathing to the origin
            // and, when that path fails, sliding through the terrain across
            // the continent.
            bool const finite = std::isfinite(action.point.x) && std::isfinite(action.point.y) && std::isfinite(action.point.z);
            if (!finite)
            {
                RejectAction(action, "MOVE_TO", "non-finite coordinate");
                return; // rejected action is not an action: no timer reset, no history entry
            }

            float const dx = action.point.x - _spawnOrigin.x;
            float const dy = action.point.y - _spawnOrigin.y;
            float const distFromHomeSq = dx * dx + dy * dy;
            if (distFromHomeSq > MAX_MOVE_FROM_HOME_YARDS * MAX_MOVE_FROM_HOME_YARDS)
            {
                RejectAction(action, "MOVE_TO", "destination is farther than 60 yards from home");
                return;
            }

            if (me->IsInCombat())
            {
                RejectAction(action, "MOVE_TO", "generic movement is unavailable during combat");
                return;
            }
            if (HasExternalMovementControl(me) || !CancelOwnFlee())
            {
                RejectAction(action, "MOVE_TO", "external movement control is active");
                return;
            }
            _attackContinuation.Clear();
            me->GetMotionMaster()->MovePoint(0, action.point.x, action.point.y, action.point.z);
            break;
        }
        case WojActionKind::Say:
        {
            std::string text = action.text;
            if (text.size() > MAX_SAY_CHARS)
            {
                LOG_WARN("module", "mod-world-of-jevs: guid {} SAY truncated from {} to {} chars",
                         _policyActorId, text.size(), MAX_SAY_CHARS);
                text.resize(MAX_SAY_CHARS);
            }
            me->Say(text, LANG_UNIVERSAL);
            WojCombatLog::Instance().WriteSay(me, action.decisionId, text);
            break;
        }
        case WojActionKind::StockTalk:
        {
            TriggerRuntime* trigger = FindTrigger(action.triggerId);
            uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
            bool const ambient = action.triggerId == "ambient_talk";
            bool const aggro = action.triggerId == "aggro_talk";
            uint32_t const textGroup = ambient
                ? WojConfig::Instance().AmbientTalkForActor(_policyActorId).textGroup
                : WojConfig::Instance().AggroTalkForActor(_policyActorId).textGroup;
            bool const policyEligible = ambient
                ? IsAmbientTalker() && !me->IsInCombat()
                : aggro && IsAggroTalker() && me->IsInCombat() && _aggroTalkAttemptedThisCombat;
            if (!policyEligible || !sCreatureTextMgr->TextExist(me->GetEntry(), textGroup) || !trigger ||
                action.triggerGeneration == 0 || trigger->view.generation != action.triggerGeneration ||
                trigger->view.status != "eligible")
            {
                RejectAction(action, "STOCK_TALK", "stock talk is no longer eligible");
                return;
            }
            SetTriggerState(*trigger, "active", nullptr, "reserved", action.decisionId, "stock_talk");
            // CreatureAI::Talk routes through CreatureTextMgr and preserves
            // the configured stock text/sound path; it does not claim a
            // client actually heard it.
            Talk(static_cast<uint8>(textGroup), nullptr);
            WojCombatLog::Instance().WriteStockTalk(me, action.decisionId, epoch,
                action.triggerId, action.triggerGeneration, textGroup);
            AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
                "succeeded", action.decisionId, "stock_group");
            if (ambient)
                ArmTimedTrigger(*trigger, false, "success");
            else
            {
                trigger->view.status = "blocked";
                trigger->view.blockedBy = "once";
            }
            break;
        }
        case WojActionKind::Attack:
        {
            ObjectGuid guid;
            if (!ParseRawGuid(action.target, guid))
            {
                RejectAction(action, "ATTACK", "bad target id");
                return;
            }
            Unit* target = ObjectAccessor::GetUnit(*me, guid);
            if (!target || !target->IsAlive())
            {
                RejectAction(action, "ATTACK", "target not found or dead");
                return;
            }
            bool const engaged = IsThreat(me, target);
            bool const formationAssist = CanAssistFormation(target);
            bool const canStart = engaged ? me->CanCreatureAttack(target) :
                (me->CanStartAttack(target) || CanStartObservedAggro(target) || formationAssist);
            if (!canStart || me->GetDistance(target) > NEIGHBOR_SEARCH_YARDS ||
                ((!me->CanSeeOrDetect(target) || !me->IsWithinLOSInMap(target)) && !formationAssist))
            {
                RejectAction(action, "ATTACK", "target is outside stock aggro or not an available attack target");
                return;
            }
            if (!CancelOwnFlee())
            {
                RejectAction(action, "ATTACK", "external fear is active");
                return;
            }
            if (!_hasCombatAnchor)
            {
                _combatAnchor = {me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation()};
                _hasCombatAnchor = true;
            }
            bool const startsNewAttack = me->GetVictim() != target || _attackContinuation != guid;
            _attackContinuation = guid;
            SuspendRoutineForCombat();
            ReleaseRoutineMoveBlock();
            AttackStartOwned(target);
            if (me->GetVictim() != target)
            {
                _attackContinuation.Clear();
                if (!me->IsInCombat())
                    ReleaseRoutineSuspension();
                RejectAction(action, "ATTACK", "core did not start the requested attack");
                return;
            }
            if (startsNewAttack)
            {
                if (TriggerRuntime* trigger = FindTrigger("aggro"))
                    AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
                        "succeeded", action.decisionId, "attack_started");
            }
            break;
        }
        case WojActionKind::Cast:
        {
            WojSpellBehavior const* policy =
                WojConfig::Instance().FindBehaviorSpellForActor(_policyActorId, action.spell);
            if (!policy)
            {
                RejectAction(action, "CAST", "spell is not allowed for actor");
                return;
            }
            ObjectGuid guid;
            if (!ParseRawGuid(action.target, guid))
            {
                RejectAction(action, "CAST", "bad target id");
                return;
            }
            Unit* target = ObjectAccessor::GetUnit(*me, guid);
            if (!target || !target->IsAlive())
            {
                RejectAction(action, "CAST", "target not found or dead");
                return;
            }
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(action.spell);
            if (!spell || me->HasSpellCooldown(action.spell) || me->IsNonMeleeSpellCast(false) || !HasEnoughPower(me, spell))
            {
                RejectAction(action, "CAST", "spell is not ready");
                return;
            }
            float areaRadius = 0.0f;
            bool const selfCenteredArea = IsCasterCenteredEnemyArea(spell, areaRadius);
            bool const hostileEngaged = policy->target == "enemy" && IsThreat(me, target);
            bool const allowedRelation = policy->target == "self"
                ? target == me
                : policy->target == "ally"
                    ? (target == me || (IsOwned(me, target) && me->IsFriendlyTo(target) &&
                        me->_IsValidAssistTarget(target, spell)))
                    : (hostileEngaged ? me->CanCreatureAttack(target) : me->CanStartAttack(target));
            if (!allowedRelation || !MatchesTargetSelector(me, policy, target))
            {
                RejectAction(action, "CAST", !allowedRelation
                    ? (spell->IsPositive() ? "target is not friendly" : "target is not hostile")
                    : "target does not match the configured stock selector");
                return;
            }
            if (RequiresOtherOwnedAlly(_policyActorId, action.spell) && target == me)
            {
                RejectAction(action, "CAST", "stock ally spell requires another eligible owned ally");
                return;
            }
            if (!IsSpellInRange(me, spell, target))
            {
                RejectAction(action, "CAST", "target is out of range or sight");
                return;
            }
            TriggerRuntime* trigger = nullptr;
            std::string triggerReason;
            if (!ValidateTriggeredAction(action, target, trigger, triggerReason))
            {
                RejectAction(action, "CAST", triggerReason);
                return;
            }
            if (_ownsTimedFlee || _ownsAssistFlight || me->HasUnitState(UNIT_STATE_FLEEING))
            {
                RejectAction(action, "CAST", "flee or movement control is active");
                return;
            }
            if (!spell->IsPositive())
                SuspendRoutineForCombat();

            WojActorBinding const* expectedSummon =
                WojConfig::Instance().FindRuntimeSummonForCast(_policyActorId, action.spell);
            uint32_t const expectedSummonCount = expectedSummon ?
                WojConfig::Instance().RuntimeSummonCountForCast(_policyActorId, action.spell) : 0;
            if (expectedSummon && !WojRuntimeBinding::Instance().BeginSummonPermit(
                    me, action.spell, expectedSummon->spawnId, expectedSummonCount))
            {
                if (!me->IsInCombat())
                    ReleaseRoutineSuspension();
                RejectAction(action, "CAST", "exact runtime summon permit could not be armed");
                return;
            }

            uint32_t const castMs = spell->CalcCastTime(me);
            int32_t const rawDuration = spell->IsChanneled() ? spell->GetDuration() : 0;
            uint32_t const channelMs = rawDuration > 0 ? static_cast<uint32_t>(rawDuration) : 0;
            // A next-swing spell stays queued until a main-hand melee hit.
            // Its zero cast time is not the time until OnSpellCast arrives.
            bool const nextSwing = spell->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) ||
                spell->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING);
            uint32_t const swingMs = nextSwing ? std::max(me->GetAttackTime(BASE_ATTACK),
                static_cast<uint32_t>(std::max(me->getAttackTimer(BASE_ATTACK), 0))) : 0;
            uint32_t const ackMs = std::min<uint32_t>(
                std::max({castMs, channelMs, swingMs}), 30000u - CAST_ACK_GRACE_MS) +
                CAST_ACK_GRACE_MS;
            _pendingCast.active = true;
            _pendingCast.epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
            _pendingCast.spell = action.spell;
            _pendingCast.generation = action.triggerGeneration;
            _pendingCast.expectedSummonActorId = expectedSummon ? expectedSummon->spawnId : 0;
            _pendingCast.expectedSummonCount = expectedSummonCount;
            _pendingCast.decisionId = action.decisionId;
            _pendingCast.target = action.target;
            _pendingCast.marker = action.marker;
            _pendingCast.permitsContinuation = action.tier == "jev" &&
                IsJevCastContinuationEnabled(action.spell);
            _pendingCast.expiresAt = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(ackMs);
            SetTriggerState(*trigger, "active", nullptr, "reserved", action.decisionId, "cast");
            _pendingCast.attemptedAt = std::chrono::steady_clock::now();
            SpellCastResult const result = me->CastSpell(selfCenteredArea ? me : target, action.spell, false);
            if (result != SPELL_CAST_OK)
            {
                if (_pendingCast.active && _pendingCast.spell == action.spell &&
                    _pendingCast.generation == action.triggerGeneration)
                    FailPendingCast("failed", "core_rejected");
                if (!me->IsInCombat())
                    ReleaseRoutineSuspension();
                RejectAction(action, "CAST", "core rejected cast " +
                    std::to_string(static_cast<uint32_t>(result)), "", false);
                return;
            }
            if (continuation)
                WojCombatLog::Instance().WriteCastContinuation(me, target, action.spell, action.decisionId);
            break;
        }
        case WojActionKind::StartPhase:
        {
            std::string const triggerId = "health_phase_" + action.phaseId;
            TriggerRuntime* trigger = FindTrigger(triggerId);
            auto const& phases = WojConfig::Instance().HealthPhasesForActor(_policyActorId);
            auto const found = std::find_if(phases.begin(), phases.end(),
                [&](WojHealthPhaseBehavior const& phase)
                {
                    return phase.id == action.phaseId && phase.entry == me->GetEntry();
                });
            auto const now = std::chrono::steady_clock::now();
            bool earlierPending = false;
            if (found != phases.end())
                for (auto it = phases.begin(); it != found; ++it)
                    if (!_consumedHealthPhases.count(it->id))
                    {
                        earlierPending = true;
                        break;
                    }
            bool const below = found != phases.end() && me->GetMaxHealth() &&
                static_cast<uint64_t>(me->GetHealth()) * 100u <
                    static_cast<uint64_t>(me->GetMaxHealth()) * found->triggerBelowHealthPct;
            if (!trigger || found == phases.end() || action.triggerGeneration == 0 ||
                trigger->view.generation != action.triggerGeneration ||
                trigger->view.status != "eligible" || _healthPhase.active ||
                _consumedHealthPhases.count(action.phaseId) || earlierPending || !below ||
                !me->IsInCombat() || now < _nextHealthPhaseAt || _pendingCast.active ||
                _ownsTimedFlee || _ownsAssistFlight || HasExternalMovementControl(me))
            {
                RejectAction(action, "START_PHASE", "health phase is no longer eligible");
                return;
            }
            SetTriggerState(*trigger, "active", nullptr, "reserved",
                action.decisionId, "phase_start");
            StartHealthPhase(*found, action.decisionId, action.triggerGeneration,
                action.snapshotEpoch);
            break;
        }
        case WojActionKind::HealthEvent:
        {
            auto const& events = WojConfig::Instance().HealthEventsForActor(_policyActorId);
            auto const found = std::find_if(events.begin(), events.end(),
                [&](WojHealthEventBehavior const& event)
                {
                    return event.id == action.healthEventId && event.entry == me->GetEntry();
                });
            TriggerRuntime* trigger = FindTrigger("health_event_" + action.healthEventId);
            uint32_t const pct = me->GetMaxHealth() ? static_cast<uint32_t>(me->GetHealthPct()) : 0;
            if (found == events.end() || !trigger || action.triggerGeneration == 0 ||
                trigger->view.generation != action.triggerGeneration ||
                trigger->view.status != "eligible" ||
                _consumedHealthEvents.count(action.healthEventId) || !me->IsInCombat() ||
                pct < found->healthPctMin || pct > found->healthPctMax ||
                _pendingCast.active || me->IsNonMeleeSpellCast(false) ||
                _ownsTimedFlee || _ownsAssistFlight || HasExternalMovementControl(me) ||
                !sCreatureTextMgr->TextExist(me->GetEntry(), found->talkGroup))
            {
                RejectAction(action, "HEALTH_EVENT", "health event is no longer eligible");
                return;
            }
            SetTriggerState(*trigger, "active", nullptr, "reserved",
                action.decisionId, "health_event");
            if (found->castSpell)
            {
                WojActorBinding const* expectedSummon =
                    WojConfig::Instance().FindRuntimeSummonForCast(_policyActorId, found->castSpell);
                uint32_t const expectedSummonCount = expectedSummon ?
                    WojConfig::Instance().RuntimeSummonCountForCast(_policyActorId, found->castSpell) : 0;
                if (expectedSummon)
                {
                    // This event completes talk and consumption in this call.
                    // A delayed cast needs a separate pending-event lifecycle;
                    // never close its provenance permit before the effect runs.
                    SpellInfo const* spell = sSpellMgr->GetSpellInfo(found->castSpell);
                    if (!spell || spell->CalcCastTime(me) != 0 || spell->IsChanneled() ||
                        spell->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE) ||
                        spell->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING) ||
                        !WojRuntimeBinding::Instance().BeginSummonPermit(me, found->castSpell,
                            expectedSummon->spawnId, expectedSummonCount))
                    {
                        SetTriggerState(*trigger, "blocked", "physical", "failed",
                            action.decisionId, "summon_permit_unavailable");
                        RejectAction(action, "HEALTH_EVENT",
                            "exact runtime summon permit could not be armed", "", false);
                        return;
                    }
                }
                SpellCastResult const result = me->CastSpell(me, found->castSpell, false);
                bool const exactSummons = !expectedSummon ||
                    WojRuntimeBinding::Instance().FinishSummonPermit(me, found->castSpell,
                        expectedSummon->spawnId, expectedSummonCount);
                if (result != SPELL_CAST_OK)
                {
                    // The core may have applied some effects before reporting
                    // failure. Never repeat a one-shot summon with unknown
                    // partial physical effects.
                    if (expectedSummon)
                        _consumedHealthEvents.insert(found->id);
                    SetTriggerState(*trigger, "blocked", "physical", "failed",
                        action.decisionId, "core_rejected");
                    RejectAction(action, "HEALTH_EVENT", "core rejected health event cast " +
                        std::to_string(static_cast<uint32_t>(result)), "", false);
                    return;
                }
                if (!exactSummons)
                {
                    // A partially executed cast may already have spawned one
                    // child. Retrying the one-shot event could duplicate it.
                    _consumedHealthEvents.insert(found->id);
                    SetTriggerState(*trigger, "blocked", "physical", "failed",
                        action.decisionId, "summon_mismatch");
                    RejectAction(action, "HEALTH_EVENT",
                        "health event cast did not create its exact configured runtime summons", "", false);
                    return;
                }
            }
            Talk(static_cast<uint8>(found->talkGroup), nullptr);
            WojCombatLog::Instance().WriteStockTalk(me, action.decisionId,
                WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)),
                trigger->view.id, action.triggerGeneration, found->talkGroup);
            _consumedHealthEvents.insert(found->id);
            SetTriggerState(*trigger, "blocked", "once", "succeeded",
                action.decisionId, "stock_health_event");
            break;
        }
        case WojActionKind::AggroEvent:
        {
            auto const& events = WojConfig::Instance().AggroEventsForActor(_policyActorId);
            auto const found = std::find_if(events.begin(), events.end(),
                [&](WojAggroEventBehavior const& event)
                {
                    return event.id == action.aggroEventId && event.actorId == _policyActorId &&
                        event.entry == me->GetEntry();
                });
            TriggerRuntime* trigger = FindTrigger("aggro_event_" + action.aggroEventId);
            Unit* invoker = _aggroInvoker.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*me, _aggroInvoker);
            if (found == events.end() || !trigger || !action.triggerGeneration ||
                trigger->view.generation != action.triggerGeneration ||
                trigger->view.status != "eligible" ||
                _aggroEventContinuation.active ||
                _consumedAggroEvents.count(action.aggroEventId) || !_aggroEventArmed ||
                !me->IsInCombat() || !invoker || !invoker->IsAlive() ||
                !me->IsValidAttackTarget(invoker) ||
                _pendingCast.active || me->IsNonMeleeSpellCast(false) ||
                _ownsTimedFlee || _ownsAssistFlight || HasExternalMovementControl(me) ||
                !sCreatureTextMgr->TextExist(me->GetEntry(), found->actions.back().textGroup))
            {
                RejectAction(action, "AGGRO_EVENT", "aggro event is no longer eligible");
                return;
            }
            SetTriggerState(*trigger, "active", nullptr, "reserved", action.decisionId, "aggro_event");
            _aggroEventContinuation.active = true;
            _aggroEventContinuation.eventId = found->id;
            _aggroEventContinuation.decisionId = action.decisionId;
            _aggroEventContinuation.epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
            _aggroEventContinuation.generation = action.triggerGeneration;
            _aggroEventContinuation.attempt = 0;
            _aggroEventContinuation.invoker = _aggroInvoker;
            _aggroEventContinuation.retryAt = std::chrono::steady_clock::now();
            AdvanceAggroEventContinuation();
            break;
        }
        case WojActionKind::CallHelp:
        case WojActionKind::FleeForAssist:
        {
            ObjectGuid recipientGuid;
            ObjectGuid targetGuid;
            if (!ParseRawGuid(action.recipient, recipientGuid) || !ParseRawGuid(action.target, targetGuid))
            {
                RejectAction(action, ActionName(action.kind), "bad recipient or target id");
                return;
            }
            Creature* recipient = ObjectAccessor::GetCreature(*me, recipientGuid);
            Unit* target = ObjectAccessor::GetUnit(*me, targetGuid);
            float const recipientRadius = action.kind == WojActionKind::CallHelp
                ? sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS)
                : sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
            if (!recipient || recipient == me || !recipient->IsAlive() || !IsOwned(me, recipient) ||
                !me->IsFriendlyTo(recipient) || me->GetDistance(recipient) > recipientRadius ||
                !me->IsWithinLOSInMap(recipient))
            {
                RejectAction(action, ActionName(action.kind), "recipient is not an available owned ally");
                return;
            }
            if (!target || !target->IsAlive() || !IsThreat(me, target) ||
                !me->IsValidAttackTarget(target) || me->GetDistance(target) > NEIGHBOR_SEARCH_YARDS ||
                !me->CanSeeOrDetect(target) || !me->IsWithinLOSInMap(target))
            {
                RejectAction(action, ActionName(action.kind), "target is not an engaged enemy");
                return;
            }
            if (action.kind == WojActionKind::CallHelp && recipient->IsInCombat())
            {
                RejectAction(action, "CALL_HELP", "recipient entered combat before request execution");
                return;
            }
            if (action.kind == WojActionKind::FleeForAssist)
            {
                TriggerRuntime* trigger = nullptr;
                std::string triggerReason;
                if (!ValidateTriggeredAction(action, target, trigger, triggerReason))
                {
                    RejectAction(action, "FLEE_FOR_ASSIST", triggerReason);
                    return;
                }
                WojPoint const& movementAnchor = MovementAnchor();
                float const dx = recipient->GetPositionX() - movementAnchor.x;
                float const dy = recipient->GetPositionY() - movementAnchor.y;
                if (me->HasPreventsFleeingAura() || HasExternalMovementControl(me) ||
                    me->GetSpeed(MOVE_RUN) < 0.1f || !WithinHomeLeash(me, movementAnchor) ||
                    dx * dx + dy * dy > MAX_MOVE_FROM_HOME_YARDS * MAX_MOVE_FROM_HOME_YARDS)
                {
                    RejectAction(action, "FLEE_FOR_ASSIST", "movement is unavailable or destination exceeds home leash");
                    return;
                }
            }

            uint32_t const requesterEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
            uint32_t const recipientEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(recipient));
            WojHelpRecord record;
            record.decisionId = action.decisionId;
            record.requester = me->GetGUID().GetRawValue();
            record.requesterKey = WojRuntimeKey(me);
            record.requesterSpawn = _policyActorId;
            record.requesterEpoch = requesterEpoch;
            record.recipient = recipient->GetGUID().GetRawValue();
            record.recipientKey = WojRuntimeKey(recipient);
            record.recipientSpawn = WojRuntimeBinding::Instance().Resolve(recipient);
            record.recipientEpoch = recipientEpoch;
            record.target = target->GetGUID().GetRawValue();
            record.targetKey = WojRuntimeKey(target);
            if (!WojHelp::Instance().Create(record))
            {
                RejectAction(action, ActionName(action.kind), "help request is already outstanding or coordinator full");
                return;
            }
            bool const recipientInCombat = recipient->IsInCombat();
            WojCombatLog::Instance().WriteHelpRequest(me, recipient, target,
                requesterEpoch, recipientEpoch, record.id, action.decisionId,
                WojHelp::TTL_MS, recipientInCombat);
            if (TriggerRuntime* help = FindTrigger("help_call"))
                AppendBehaviorEvent(help->view.id, help->view.generation,
                    "succeeded", action.decisionId, "request_created");
            if (action.kind == WojActionKind::FleeForAssist)
            {
                TriggerRuntime* trigger = FindTrigger("flee");
                SetTriggerState(*trigger, "active", nullptr, "reserved", action.decisionId, "assist");
                bool const alreadyFleeing = _ownsTimedFlee || _ownsAssistFlight;
                CancelOwnFlee();
                SuspendRoutineForCombat();
                ReleaseRoutineMoveBlock();
                _attackContinuation.Clear();
                me->AttackStop();
                me->SetUnitFlag(UNIT_FLAG_FLEEING);
                me->AddUnitState(UNIT_STATE_FLEEING);
                _ownsAssistFlight = true;
                _fleeConsumedThisEpoch = true;
                _assistFlightUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(FLEE_DURATION_MS);
                me->GetMotionMaster()->MovePoint(ASSIST_MOVE_POINT_ID, recipient->GetPosition());
                bool const motionInstalled = me->HasUnitState(UNIT_STATE_FLEEING) &&
                    me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == POINT_MOTION_TYPE;
                WojCombatLog::Instance().WriteFleeStart(me, target, action.decisionId,
                    requesterEpoch, "assist", motionInstalled);
                if (!alreadyFleeing && !_fleeEmoteSentThisCombat)
                {
                    BroadcastFleeEmote(me);
                    WojCombatLog::Instance().WriteFleeEmote(me, action.decisionId, requesterEpoch);
                    _fleeEmoteSentThisCombat = true;
                }
                AppendBehaviorEvent(trigger->view.id, action.triggerGeneration,
                    "succeeded", action.decisionId, "assist_flight");
            }
            break;
        }
        case WojActionKind::Flee:
        {
            ObjectGuid targetGuid;
            if (!ParseRawGuid(action.target, targetGuid))
            {
                RejectAction(action, "FLEE", "bad target id");
                return;
            }
            Unit* target = ObjectAccessor::GetUnit(*me, targetGuid);
            TriggerRuntime* trigger = nullptr;
            std::string triggerReason;
            if (!ValidateTriggeredAction(action, target, trigger, triggerReason))
            {
                RejectAction(action, "FLEE", triggerReason);
                return;
            }
            if (!target || !target->IsAlive() || !IsThreat(me, target) || !me->IsValidAttackTarget(target) ||
                me->GetDistance(target) > NEIGHBOR_SEARCH_YARDS || !me->CanSeeOrDetect(target) ||
                !me->IsWithinLOSInMap(target) ||
                me->HasPreventsFleeingAura() || HasExternalMovementControl(me) ||
                me->GetSpeed(MOVE_RUN) < 0.1f || !WithinHomeLeash(me, MovementAnchor()))
            {
                RejectAction(action, "FLEE", "target is not engaged or movement is unavailable");
                return;
            }
            _attackContinuation.Clear();
            me->AttackStop();
            bool const alreadyFleeing = _ownsTimedFlee || _ownsAssistFlight;
            CancelOwnFlee();
            SuspendRoutineForCombat();
            ReleaseRoutineMoveBlock();
            SetTriggerState(*trigger, "active", nullptr, "reserved", action.decisionId, "flee");
            me->GetMotionMaster()->MoveFleeing(target, FLEE_DURATION_MS);
            _ownsTimedFlee = true;
            _fleeConsumedThisEpoch = true;
            _ownTimedFleeUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(FLEE_DURATION_MS);
            bool const motionInstalled = me->HasUnitState(UNIT_STATE_FLEEING) &&
                me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == TIMED_FLEEING_MOTION_TYPE;
            WojCombatLog::Instance().WriteFleeStart(me, target, action.decisionId,
                WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)), "timed", motionInstalled);
            if (!alreadyFleeing && !_fleeEmoteSentThisCombat)
            {
                BroadcastFleeEmote(me);
                WojCombatLog::Instance().WriteFleeEmote(me, action.decisionId,
                    WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me)));
                _fleeEmoteSentThisCombat = true;
            }
            AppendBehaviorEvent(trigger->view.id, action.triggerGeneration,
                "succeeded", action.decisionId, "flee_started");
            break;
        }
        case WojActionKind::AnswerHelp:
        {
            WojHelpRecord request;
            uint32_t const recipientEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
            if (!WojHelp::Instance().Peek(action.helpRequestId, WojRuntimeKey(me),
                                          recipientEpoch, request))
            {
                RejectAction(action, "ANSWER_HELP", "request not found, expired, or belongs to another life",
                             action.helpRequestId);
                return;
            }
            Creature* requester = ObjectAccessor::GetCreature(*me, ObjectGuid(request.requester));
            Unit* target = ObjectAccessor::GetUnit(*me, ObjectGuid(request.target));
            if (!requester || !(WojRuntimeKey(requester) == request.requesterKey) ||
                !target || !(WojRuntimeKey(target) == request.targetKey) ||
                !requester->IsAlive() || !IsOwned(me, requester) ||
                !me->IsFriendlyTo(requester) || me->GetDistance(requester) > ALLY_SEARCH_YARDS ||
                WojRegistry::Instance().CurrentEpoch(request.requesterKey) != request.requesterEpoch ||
                !target->IsAlive() || !IsThreat(requester, target) ||
                !me->IsValidAttackTarget(target) || me->GetDistance(target) > NEIGHBOR_SEARCH_YARDS ||
                !me->CanSeeOrDetect(target) || !me->IsWithinLOSInMap(target))
            {
                RejectAction(action, "ANSWER_HELP", "request participants or target are no longer valid",
                             action.helpRequestId);
                return;
            }
            bool const recipientWasInCombat = me->IsInCombat();
            if (!CancelOwnFlee())
            {
                RejectAction(action, "ANSWER_HELP", "external fear is active", action.helpRequestId);
                return;
            }
            _attackContinuation = target->GetGUID();
            SuspendRoutineForCombat();
            ReleaseRoutineMoveBlock();
            AttackStartOwned(target);
            if (me->GetVictim() != target)
            {
                _attackContinuation.Clear();
                if (!me->IsInCombat())
                    ReleaseRoutineSuspension();
                RejectAction(action, "ANSWER_HELP", "core did not start the requested attack",
                             action.helpRequestId);
                return;
            }
            if (!WojHelp::Instance().Consume(request.id))
            {
                _attackContinuation.Clear();
                me->AttackStop();
                if (!me->IsInCombat())
                    ReleaseRoutineSuspension();
                RejectAction(action, "ANSWER_HELP", "request disappeared before execution",
                             action.helpRequestId);
                return;
            }
            uint32_t const latencyMs = static_cast<uint32_t>(std::min<int64>(
                std::max<int64>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - request.createdAt).count()),
                std::numeric_limits<uint32_t>::max()));
            WojCombatLog::Instance().WriteHelpResponse(me, requester, target,
                request.requesterEpoch, request.recipientEpoch, request.id, request.decisionId,
                action.decisionId, latencyMs, recipientWasInCombat);
            if (TriggerRuntime* help = FindTrigger("help_answer"))
                AppendBehaviorEvent(help->view.id, help->view.generation,
                    "succeeded", action.decisionId, "request_answered");
            break;
        }
        case WojActionKind::StopAttack:
            _attackContinuation.Clear();
            me->CombatStop(true);
            if (CancelOwnFlee())
            {
                me->StopMoving();
                RestoreRoutineMotion();
            }
            break;
        case WojActionKind::Evade:
            if (!CancelOwnFlee())
            {
                RejectAction(action, "EVADE", "external fear is active");
                return;
            }
            _attackContinuation.Clear();
            if (TriggerRuntime* trigger = FindTrigger("return"))
                AppendBehaviorEvent(trigger->view.id, trigger->view.generation,
                    "succeeded", action.decisionId, "evade_started");
            EnterEvadeMode(EVADE_REASON_OTHER);
            break;
        case WojActionKind::ResumeRoutine:
            if (!ResumeRoutine(action))
                return;
            break;
        case WojActionKind::Idle:
            // The decider looked and chose to do nothing. Record the marker so
            // the next snapshot carries it, but do not reset _sinceAction:
            // standing still is exactly what "no action" has to keep meaning,
            // or the wander timer would never mature.
            PushAction(action.marker.empty() ? "idle" : action.marker);
            // M3: decision_id was parsed and never read anywhere - printing
            // it here is what makes this line findable in decisions.jsonl,
            // the only record of why the gateway chose it (thought repeats
            // verbatim across hundreds of IDLE lines and cannot disambiguate
            // on its own).
            // Один и тот же decision_id в нескольких строках подряд — это
            // норма, а не сбой: гейтвей отдал одно решение из кэша снимка
            // (обстановка не изменилась), и запись в журнале на все эти
            // строки одна — та, что это решение и описывает.
            // Ярус вставлен между thought и decision, а не в конец строки:
            // decision {} — это decision_id гейтвея, uuid4().hex, всегда
            // 32 знака (НЕ восьмизначный requestId из WojBridge, его в этой
            // строке нет вовсе), и ярус остаётся на
            // предсказуемом расстоянии от правого края независимо от длины
            // thought (у Jev это несколько полей через " | ", у эвристики —
            // короткая причина деградации). ".empty() ? "?"" переживает
            // будущий ярус, для которого этот бинарник ещё не пересобирали:
            // пустая строка от неизвестного значения не должна выглядеть как
            // отсутствие яруса вовсе.
            LOG_INFO("module", "mod-world-of-jevs: guid {} -> IDLE | {} | {} | decision {}",
                     _policyActorId, action.thought,
                     action.tier.empty() ? "?" : action.tier.c_str(), action.decisionId);
            return;
        case WojActionKind::None:
        {
            // Nothing usable came back from the gateway (timeout, bad
            // response, ...). This must stay visibly distinct from IDLE,
            // which is the decider actively choosing to do nothing: this is
            // a miss, not a choice. The worker thread cannot log (it races
            // sLog's teardown/reconfiguration on the game thread - see
            // WojBridge.cpp), so it hands the reason back here in
            // action.thought instead: gateway unreachable or a bad status,
            // a request_id mismatch, or a response that failed to parse.
            //
            // I3: two requirements sit above the cooldown itself. First, the
            // opening line of any outage must never be held back - an
            // operator has to see "it just broke" within the same second,
            // not after a stale timer left over from an unrelated miss
            // expires. streakStart (this miss follows something that was
            // NOT a miss - IDLE, SAY, MOVE_TO, or nothing yet) forces that.
            // Second, a suppressed repeat must not vanish silently: it is
            // counted and named in the next line that does print, so
            // "broken once" and "broken continuously for an hour" cannot
            // look identical in the log. IDLE and real actions are
            // deliberately left alone (fix brief, I3): they are the NPC
            // actually living, which is the whole point of watching it.
            bool const streakStart = !wasInNoneStreak;
            auto const now = std::chrono::steady_clock::now();
            bool const cooldownElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - _lastNoneLog).count() >= NONE_LOG_COOLDOWN_SECONDS;
            if (streakStart || cooldownElapsed)
            {
                if (_suppressedNoneCount > 0)
                    LOG_WARN("module", "mod-world-of-jevs: guid {} -> NONE (no usable action from gateway): {} ({} more suppressed in the last {}s)",
                             _policyActorId, action.thought, _suppressedNoneCount, NONE_LOG_COOLDOWN_SECONDS);
                else
                    LOG_WARN("module", "mod-world-of-jevs: guid {} -> NONE (no usable action from gateway): {}",
                             _policyActorId, action.thought);
                _lastNoneLog = now;
                _suppressedNoneCount = 0;
            }
            else
            {
                ++_suppressedNoneCount;
            }
            return;
        }
        default:
            return;
    }
    if (!continuation && !action.decisionId.empty())
        WojCombatLog::Instance().WriteActionApplied(me, ActionName(action.kind), action.decisionId);
    _sinceAction = 0;
    if (!continuation && oneShot)
        RememberOneShot(action.decisionId);
    // The decider names the marker. Recognising the action by matching its
    // spoken text here would mean the same string literal living in two
    // languages, where editing one silently breaks the other.
    PushAction(action.marker.empty() ? "unknown" : action.marker);

    // M3: same reasoning as the IDLE line above - decision_id ties this line
    // back to its record in decisions.jsonl.
    // Тот же порядок полей и тот же смысл ".empty() ? "?"", что и в ветке
    // IDLE выше — см. комментарий там.
    LOG_INFO("module", "mod-world-of-jevs: guid {} -> {}{} | {} | {} | decision {}",
             _policyActorId,
             ActionName(action.kind),
             continuation ? "_CONTINUATION" : "",
             action.thought,
             action.tier.empty() ? "?" : action.tier.c_str(), action.decisionId);
}

float WojAI::PreferredCombatRangeYards() const
{
    WojActorBinding const* binding = WojConfig::Instance().FindActor(_policyActorId);
    if (!binding || binding->entry != me->GetEntry() || binding->mapId != me->GetMapId() ||
        !binding->hasPreferredCombatRange)
        return 0.0f;
    return binding->preferredCombatRangeYards;
}

void WojAI::AttackStartOwned(Unit* target)
{
    // Core Creature::AtEngage synchronously asks the stock formation to
    // engage its other members. That would put a Jev-owned member in combat
    // before its own decision. Keep the formation for movement and evade,
    // but suppress only this initiating JevAI attack's stock assist callback.
    CreatureGroup* formation = me->GetFormation();
    struct RestoreFormation
    {
        Creature* creature;
        CreatureGroup* formation;
        ~RestoreFormation() { if (formation) creature->SetFormation(formation); }
    } restore{me, formation};
    if (formation)
        me->SetFormation(nullptr);
    float const range = PreferredCombatRangeYards();
    _appliedCombatRangeYards = range;
    if (range > 0.0f)
        AttackStartCaster(target, range);
    else
        AttackStart(target);
}

void WojAI::ContinueAttack()
{
    if (!_attackContinuation || _healthPhase.active || _ownsTimedFlee || _ownsAssistFlight ||
        HasExternalAttackControl(me))
        return;
    Unit* target = ObjectAccessor::GetUnit(*me, _attackContinuation);
    if (!target || !target->IsAlive() || !me->IsHostileTo(target))
    {
        _attackContinuation.Clear();
        if (!me->IsInEvadeMode())
            EnterEvadeMode(EVADE_REASON_NO_HOSTILES);
        return;
    }
    if (!me->CanCreatureAttack(target))
    {
        _attackContinuation.Clear();
        if (!me->IsInEvadeMode())
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
        return;
    }
    if (me->IsNonMeleeSpellCast(false))
        return;

    float const range = PreferredCombatRangeYards();
    if (me->GetVictim() != target)
        AttackStartOwned(target);
    else if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE ||
             _appliedCombatRangeYards != range)
    {
        if (_appliedCombatRangeYards != range)
            me->Attack(target, range == 0.0f);
        me->GetMotionMaster()->MoveChase(target, range);
        _appliedCombatRangeYards = range;
    }
    DoMeleeAttackIfReady();
}

void WojAI::UpdateAI(uint32 diff)
{
    // The live kill switch. GetAI only gates creation; a WojAI that already
    // exists would otherwise keep driving its NPC after the operator sets
    // WorldOfJevs.Enable = 0 and reloads. Reading WojConfig here is safe:
    // this is the game thread, the same thread OnAfterConfigLoad writes it
    // from.
    if (!WojConfig::Instance().Enabled || !WojConfig::Instance().BehaviorPolicyValid)
    {
        CancelAggroEventContinuation("disabled");
        // M4: the other half of ending a NONE streak - see
        // ResetSuppressedNone(). Without this, disabling the module mid-
        // outage freezes _suppressedNoneCount/_lastNoneLog instead of
        // ending the streak, and whatever streak is running when the
        // module comes back inherits them.
        ResetSuppressedNone();
        return;
    }

    uint32_t const currentEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
    if (currentEpoch != _behaviorEpoch)
    {
        ClearJevCastContinuation();
        if (_pendingCast.active)
            FailPendingCast("cancelled", "epoch_changed");
        CancelAggroEventContinuation("epoch_changed");
        InitializeBehavior("epoch");
    }
    if (_pendingCast.active && std::chrono::steady_clock::now() >= _pendingCast.expiresAt)
    {
        uint32_t const expiredSpell = _pendingCast.spell;
        // Сначала закрываем token: синхронный cancel callback от interrupt
        // тогда не сможет породить второй terminal outcome этого cast.
        FailPendingCast("failed", "cast_timeout");
        // InterruptNonMeleeSpells does not touch the queued next-swing slot.
        // Cancel only our exact spell so a late swing cannot cast after the
        // token has already failed and spuriously enable another decision.
        if (Spell* queued = me->GetCurrentSpell(CURRENT_MELEE_SPELL);
            queued && queued->GetSpellInfo()->Id == expiredSpell)
            me->InterruptSpell(CURRENT_MELEE_SPELL, false, true, true);
        me->InterruptNonMeleeSpells(false, expiredSpell, true, true);
    }

    bool const inCombat = me->IsInCombat();
    if (inCombat && !_wasInCombat)
    {
        AppendBehaviorEvent("", 0, "combat_enter", "", "core");
        if (_aggroTalkAttemptedThisCombat)
            if (TriggerRuntime* talk = FindTrigger("aggro_talk"))
            {
                ++talk->view.generation;
                talk->view.status = "blocked";
                talk->view.blockedBy = "combat";
                AppendBehaviorEvent(talk->view.id, talk->view.generation,
                    "rearmed", "", "combat_enter");
            }
        _aggroTalkAttemptedThisCombat = false;
        AttemptAggroTalk();
        if (TriggerRuntime* ambient = FindTrigger("ambient_talk");
            ambient && ambient->timerArmed && !ambient->timerPaused)
        {
            ambient->view.waitMs = static_cast<uint32_t>(std::max<int64_t>(0,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    ambient->deadline - std::chrono::steady_clock::now()).count()));
            ambient->timerPaused = true;
        }
        for (TriggerRuntime& trigger : _triggers)
            if (trigger.spell && !trigger.eventDriven && !trigger.ticksOutOfCombat)
            {
                if (!trigger.timerArmed)
                    ArmTimedTrigger(trigger, true, "combat_enter");
                else if (trigger.timerPaused)
                {
                    trigger.deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(trigger.view.waitMs);
                    trigger.timerPaused = false;
                }
            }
        if (!_hasCombatAnchor)
        {
            _combatAnchor = {me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), me->GetOrientation()};
            _hasCombatAnchor = true;
        }
        uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
        if (_routineInstalled && _routineAuthorizedEpoch == epoch)
        {
            SuspendRoutineForCombat();
            WojCombatLog::Instance().WriteRoutineEvent("routine_suspend", me, epoch,
                RoutineProfileName(_routineProfile), _routinePathId, "", "combat");
        }
    }
    else if (!inCombat && _wasInCombat)
    {
        AppendBehaviorEvent("", 0, "combat_exit", "", "core");
        _nextSecondaryYieldAt = {};
        _secondaryDecisionWaitUntil = {};
        CancelAggroEventContinuation("combat_exit");
        _aggroEventArmed = false;
        _aggroInvoker.Clear();
        if (TriggerRuntime* talk = FindTrigger("aggro_talk"); talk && talk->view.status == "eligible")
        {
            talk->view.status = "blocked";
            talk->view.blockedBy = "combat";
            AppendBehaviorEvent(talk->view.id, talk->view.generation,
                "cancelled", "", "combat_exit");
        }
        auto const now = std::chrono::steady_clock::now();
        if (TriggerRuntime* ambient = FindTrigger("ambient_talk");
            ambient && ambient->timerArmed && ambient->timerPaused)
        {
            ambient->deadline = now + std::chrono::milliseconds(ambient->view.waitMs);
            ambient->timerPaused = false;
        }
        for (TriggerRuntime& trigger : _triggers)
            if (trigger.spell && !trigger.eventDriven && !trigger.ticksOutOfCombat && trigger.timerArmed &&
                !(_pendingCast.active && _pendingCast.spell == trigger.spell))
            {
                trigger.view.waitMs = static_cast<uint32_t>(std::max<int64_t>(0,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        trigger.deadline - now).count()));
                trigger.timerPaused = true;
            }
    }
    _wasInCombat = inCombat;
    AdvanceAggroEventContinuation();

    if (_routineSuspendedForCombat && !inCombat && !_ownsTimedFlee && !_ownsAssistFlight &&
        !me->IsInEvadeMode())
        ReleaseRoutineSuspension();

    // RandomMovementGenerator не реализует Pause(). Флаг ставится только
    // когда random снова оказался верхним motor: chase/flee/home он не
    // замораживает, а generator и его исходный центр остаются прежними.
    if (_routineSuspendedForCombat && inCombat)
        SuspendRoutineForCombat();

    auto const motionNow = std::chrono::steady_clock::now();
    bool const outsideHomeLeash = !WithinHomeLeash(me, MovementAnchor());
    if (_ownsAssistFlight && (motionNow >= _assistFlightUntil || outsideHomeLeash))
    {
        if (CancelOwnFlee())
        {
            me->StopMoving();
            if (me->IsInCombat())
                SuspendRoutineForCombat();
            else
                RestoreRoutineMotion();
        }
    }
    if (_ownsTimedFlee)
    {
        if (motionNow >= _ownTimedFleeUntil || outsideHomeLeash)
        {
            if (CancelOwnFlee())
            {
                me->StopMoving();
                if (me->IsInCombat())
                    SuspendRoutineForCombat();
                else
                    RestoreRoutineMotion();
            }
        }
        else if (me->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) != TIMED_FLEEING_MOTION_TYPE)
            _ownsTimedFlee = false;
    }

    // Phase choreography is encounter physics. While it owns movement and
    // stance, do not enqueue a model request that cannot legally execute.
    if (UpdateHealthPhases())
    {
        _sinceDecision = 0;
        return;
    }
    UpdateHealthEvents();
    UpdateAggroEvents();

    auto started = std::chrono::steady_clock::now();

    WojActorKey const actor = WojRuntimeKey(me);

    // Round 2: the keep-alive self-heal that used to live here moved to
    // WojKeepAlive::Sweep(), called from WorldScript::OnUpdate - this AI
    // does not exist at all when WorldOfJevs.Enable=0 (NEW-3), does not
    // know the keep-alive safety cap (NEW-1), and a one-time setActive()
    // has no business being charged against this tick's budget (NEW-5).
    // See WojKeepAlive.h for the fuller reasoning.

    uint32 const epoch = WojRegistry::Instance().CurrentEpoch(actor);
    bool const decisionActive = RefreshDecisionActivation(diff, actor, epoch);

    WojAction action;
    if (decisionActive && WojRegistry::Instance().TakeAction(actor, epoch, action))
    {
        // ApplyAction can path-find (MovePoint) or broadcast a chat packet
        // (Say); neither belongs in the budget below, which only watches
        // our own bookkeeping. Exclude it by moving the clock's start
        // forward by whatever it took, instead of stopping and restarting
        // the clock.
        auto const beforeApply = std::chrono::steady_clock::now();

        WojConfig const& config = WojConfig::Instance();
        uint32_t effectiveFreshnessMs = std::min(action.freshnessLimitMs,
            me->IsInCombat() ? config.CombatFreshnessMs : config.IdleFreshnessMs);
        uint32_t const totalAgeMs = static_cast<uint32_t>(std::min<int64>(
            std::max<int64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                beforeApply - action.capturedAt).count(), 0), std::numeric_limits<uint32_t>::max()));
        char const* timingStatus = "unusable";
        bool stale = false;
        bool invalidValidity = false;
        if (action.kind != WojActionKind::None && action.validForSeconds == 0)
            invalidValidity = true;
        else if (action.kind != WojActionKind::None)
        {
            uint64_t const gatewayLimit = static_cast<uint64_t>(action.validForSeconds) * 1000;
            effectiveFreshnessMs = std::min<uint32_t>(effectiveFreshnessMs,
                static_cast<uint32_t>(std::min<uint64_t>(gatewayLimit, std::numeric_limits<uint32_t>::max())));
            stale = totalAgeMs > effectiveFreshnessMs;
            timingStatus = stale ? "stale" : "fresh";
        }
        WojCombatLog::Instance().WriteDecisionTiming(me, action, timingStatus,
            effectiveFreshnessMs, beforeApply);
        if (invalidValidity || stale)
        {
            WojAction unusable;
            unusable.thought = invalidValidity
                ? "decision " + action.decisionId + " has invalid valid_for_seconds=0"
                : "decision " + action.decisionId + " stale: limit " +
                    std::to_string(effectiveFreshnessMs) + "ms, consumed " +
                    std::to_string(totalAgeMs) + "ms after capture";
            action = std::move(unusable);
        }

        ApplyAction(action);
        started += std::chrono::steady_clock::now() - beforeApply;
    }

    auto const beforeContinuation = std::chrono::steady_clock::now();
    ContinueAttack();
    started += std::chrono::steady_clock::now() - beforeContinuation;

    // This is not another decision: it is the still-current Jev CAST intent
    // reaching its exact source timer. A fresh usable Jev answer was already
    // consumed above and either refreshed this same intent or cancelled it.
    if (decisionActive)
        ContinueJevCast();

    _sinceAction += diff;
    if (!decisionActive)
    {
        _sinceDecision = 0;
        return;
    }
    _sinceDecision += diff;
    uint32 const decisionTick = me->IsInCombat() ? WojConfig::Instance().CombatTickMs : WojConfig::Instance().TickMs;
    if (_sinceDecision >= decisionTick)
    {
        WojSnapshot snap;
        snap.capturedAt = std::chrono::steady_clock::now();
        snap.epoch = epoch;
        BuildSnapshot(snap);
        snap.seq = ++_seq;
        WojConfig const& config = WojConfig::Instance();
        snap.freshnessLimitMs = snap.inCombat ? config.CombatFreshnessMs : config.IdleFreshnessMs;
        snap.secondsSinceLastDecision = static_cast<float>(_sinceDecision) / 1000.0f;
        snap.secondsSinceLastAction = static_cast<float>(_sinceAction) / 1000.0f;
        snap.enqueuedAt = std::chrono::steady_clock::now();
        WojRegistry::Instance().PutSnapshot(snap);
        _sinceDecision = 0;
    }

    auto spent = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (spent > TICK_BUDGET_US)
    {
        auto const now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - _lastBudgetWarning).count() >= BUDGET_WARNING_COOLDOWN_SECONDS)
        {
            LOG_WARN("module", "mod-world-of-jevs: guid {} spent {} us this tick, budget {}",
                     _policyActorId, spent, TICK_BUDGET_US);
            _lastBudgetWarning = now;
        }
    }
}

void WojAI::JustDied(Unit* /*killer*/)
{
    ClearJevCastContinuation();
    _lifecycleNeedsRespawnRoll = true;
    CancelAggroEventContinuation("death");
    if (_pendingCast.active)
        FailPendingCast("cancelled", "death");
    AppendBehaviorEvent("", 0, "died", "", "death");
    _attackContinuation.Clear();
    StopRoutine("death");
    WojActorKey const actor = WojRuntimeKey(me);
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(actor);
    WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(actor, epoch), "death");
    WojRegistry::Instance().BumpEpoch(actor, "death");
}

void WojAI::OnDespawn()
{
}

void WojAI::EnterEvadeMode(EvadeReason why)
{
    if (me->IsInEvadeMode())
        return;
    CancelAggroEventContinuation("evade");
    if (_pendingCast.active)
        FailPendingCast("cancelled", "evade");
    if (_wasInCombat)
        AppendBehaviorEvent("", 0, "combat_exit", "", "evade");
    _wasInCombat = false;
    _attackContinuation.Clear();
    RestoreHealthPhaseBaseline("evade_reset");
    _consumedHealthEvents.clear();
    _consumedAggroEvents.clear();
    _aggroEventArmed = false;
    _aggroInvoker.Clear();
    ReleaseRoutineSuspension();
    WojActorKey const actor = WojRuntimeKey(me);
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(actor);
    WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(actor, epoch), "evade");
    WojRegistry::Instance().BumpEpoch(actor, "evade");
    InitializeBehavior("evade");
    if (_routineInstalled)
    {
        // Смена epoch инвалидирует ответы старого снимка, но не отменяет уже
        // выбранный Jev мирный motor. Core снимет HOME overlay и продолжит
        // тот же random/waypoint generator с прежним центром или узлом.
        _routineAuthorizedEpoch = WojRegistry::Instance().CurrentEpoch(WojRuntimeKey(me));
        WojCombatLog::Instance().WriteRoutineEvent("routine_return", me,
            _routineAuthorizedEpoch, RoutineProfileName(_routineProfile),
            _routinePathId, "", "evade");
    }
    _fleeEmoteSentThisCombat = false;
    _fleeConsumedThisEpoch = false;
    CreatureAI::EnterEvadeMode(why);
}

void WojAI::JustReachedHome()
{
    _hasCombatAnchor = false;
}

void WojAI::MovementInform(uint32 type, uint32 id)
{
    if (type != POINT_MOTION_TYPE || id != 0x574F50 || !_healthPhase.active || _healthPhase.arrived)
        return;
    auto const now = std::chrono::steady_clock::now();
    _healthPhase.arrived = true;
    _healthPhase.equipmentAt = now + std::chrono::milliseconds(_healthPhase.behavior.equipmentDelayMs);
    _healthPhase.restoreAt = now + std::chrono::milliseconds(_healthPhase.behavior.restoreCombatMs);
    me->SetTarget();
    me->SetFacingTo(_healthPhase.behavior.destination.o);
    me->SetStandState(UNIT_STAND_STATE_KNEEL);
    if (Unit* victim = ObjectAccessor::GetUnit(*me, _healthPhase.victim))
        me->SendMeleeAttackStop(victim);
    WojCombatLog::Instance().WriteHealthPhase(me, _healthPhase.behavior, "arrived",
        _behaviorEpoch, HealthPct(me), _healthPhase.decisionId, _healthPhase.generation,
        _healthPhase.decisionEpoch);
}

void WojAI::JustRespawned()
{
    bool const physicalRespawn = _lifecycleNeedsRespawnRoll;
    if (physicalRespawn)
    {
        _lifecycleNeedsRespawnRoll = false;
        WojCombatLog::Instance().ResetAiInitDespawnRoll(me);
        ApplyAiInitDespawn();
    }
    CancelAggroEventContinuation("respawn");
    // Clear the previous life: without this, the first snapshot after
    // resurrection would carry _lastActions from before death and a
    // secondsSinceLastAction around three hundred seconds, including the
    // time spent as a corpse. For guid 89965's actual dynamic-mode respawn
    // this body runs on a brand new object whose members are already at
    // these same defaults (see the corrected comment on the declaration in
    // WojAI.h) - harmless, not load-bearing, kept correct for whichever
    // spawn config actually reuses the object.
    StopRoutine("respawn");
    RestoreHealthPhaseBaseline("respawn_reset");
    _consumedHealthEvents.clear();
    _consumedAggroEvents.clear();
    _aggroEventArmed = false;
    _aggroInvoker.Clear();
    _lastActions.clear();
    _attackContinuation.Clear();
    _oneShotOrder.clear();
    _oneShotDecisions.clear();
    _lastSuccessfulCast.clear();
    _ownsTimedFlee = false;
    _ownsAssistFlight = false;
    _hasCombatAnchor = false;
    _combatAnchor = _spawnOrigin;
    _wasInCombat = false;
    _fleeEmoteSentThisCombat = false;
    _fleeConsumedThisEpoch = false;
    _sinceAction = 0;
    _sinceDecision = 0;
    WojActorKey const actor = WojRuntimeKey(me);
    uint32_t const epoch = WojRegistry::Instance().CurrentEpoch(actor);
    WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(actor, epoch), "respawn");
    WojRegistry::Instance().BumpEpoch(actor, "respawn");
    InitializeBehavior("respawn");
    (void)physicalRespawn;
}
