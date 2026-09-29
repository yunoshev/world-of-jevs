#include "WojAI.h"
#include "WojBridge.h"
#include "WojCombatLog.h"
#include "WojCommands.h"
#include "WojConfig.h"
#include "WojKeepAlive.h"
#include "WojHelp.h"
#include "WojRegistry.h"
#include "WojRuntimeIdentity.h"
#include "WojRuntimeBinding.h"
#include "AllCreatureScript.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ScriptMgr.h"
#include <unordered_set>


class WojWorldScript : public WorldScript
{
public:
    WojWorldScript() : WorldScript("WojWorldScript") { }

    void OnAfterConfigLoad(bool reload) override
    {
        WojConfig& config = WojConfig::Instance();
        bool const oldEnabled = config.Enabled;
        WojMode const oldMode = config.Mode;
        auto const oldOwned = config.OwnedGuids;
        auto const oldSpells = config.Spells;
        WojBehaviorPolicy const oldBehavior = config.Behavior;
        config.Load(reload);
        WojBridge::Instance().UpdateTimeout(config.TimeoutMs);

        // On first boot the world is not up yet; OnStartup below still owns
        // bringing the bridge up. On a ".reload config" the world already
        // is up, and nothing else ever re-checks Enabled against whether
        // the bridge is actually running - without this, an operator who
        // starts disabled and later flips Enable = 1 finds WojAI::UpdateAI's
        // live kill switch open and PutSnapshot filling the registry with
        // nothing ever there to drain it: every owned NPC sits at
        // hasSnapshot = true forever, TakeAction never returns anything, and
        // no log line explains why. Start()/Stop() are both idempotent
        // (WojBridge::_running.exchange), so calling the one that is
        // already in the right state is a no-op.
        if (!reload)
            return;

        std::unordered_set<uint32_t> const changedBindings = config.ChangedBindingActors(oldBehavior);
        bool const v3 = oldBehavior.schemaVersion == 3 || config.Behavior.schemaVersion == 3;
        bool const legacyOwnershipChanged = !v3 &&
            (oldMode != config.Mode || oldOwned != config.OwnedGuids || oldSpells != config.Spells);
        bool const bindingTransition = oldEnabled != config.Enabled || legacyOwnershipChanged ||
            !changedBindings.empty();
        if (bindingTransition)
        {
            // A v3 rules-only revision stays in WojAI and preserves native
            // motion.  Only the changed ownership/entry/map/group union is
            // fenced and handed back through the selector.
            if (v3 && oldEnabled == config.Enabled)
                WojReinitializeActors(oldBehavior, changedBindings, "config_reload");
            else
                WojReinitializeTransition(oldBehavior, "config_reload");
        }
        else if (config.BehaviorPolicyValid && config.BehaviorPolicyChanged)
        {
            // Policy-only reload не трогает Creature и его native motion:
            // меняются только epoch, trigger ledger и таймеры.
            WojReloadBehaviorOwned();
        }

        // Reload normally keeps the last-good v3 catalog, but retain the
        // same fail-closed bridge gate as startup should a future load path
        // leave bindings unverified.
        if (config.Enabled && config.BehaviorPolicyValid)
            WojBridge::Instance().Start();
        else
            WojBridge::Instance().Stop();
    }

    void OnStartup() override
    {
        WojCombatLog::Instance().Initialize();
        if (!WojConfig::Instance().ValidateBehaviorBindings())
            LOG_ERROR("module", "mod-world-of-jevs: v3 binding preflight failed at startup; Jev ownership is fail-closed");
        if (WojConfig::Instance().Enabled && WojConfig::Instance().BehaviorPolicyValid)
            WojBridge::Instance().Start();

        // Part B: independent of WorldOfJevs.Enable/the bridge - keep-alive
        // is about the world ticking a creature at all, not about who is
        // deciding its actions. Apply() itself is one-shot (see
        // WojKeepAlive.h): there is no ".reload config" hook for it, and
        // does not need one - the grid it loads never unloads
        // (Map::UnloadAll() at map shutdown is the only unload this core
        // has, docs/world-liveness.md).
        //
        // The creature's own active flag is a different story: guid 89965's
        // spawn is dynamic-mode, not compatibility-mode (spawn_group has no
        // row for it, the default spawn_group_template lacks the
        // compatibility flag, Respawn.ForceCompatibilityMode = 0 - checked
        // against the live world DB and worldserver.conf, not assumed), so
        // death destroys this Creature outright and
        // Map::ProcessCreatureRespawn() later builds an entirely new one
        // with isActiveObject() back at its unset default. Apply() running
        // once at startup cannot reach forward in time to flip that future
        // object's flag - see WojKeepAlive::Sweep(), called from OnUpdate
        // below, for the fix. (Round 2: this used to be
        // WojKeepAlive::ShouldKeepAlive(), called from WojAI::UpdateAI -
        // moved out because that AI does not exist at all when
        // WorldOfJevs.Enable=0, which quietly broke the independence this
        // comment promises - see the round 2 fix report, NEW-3.)
        WojKeepAlive::Apply();
    }


    void OnUpdate(uint32 diff) override
    {
        WojWriteHelpExpired(WojHelp::Instance().Expire(), "ttl");
        WojCombatLog::Instance().Update();

        // Workers записывают только plain-data telemetry. Дренировать её
        // нужно каждый world tick: десятисекундный keep-alive cadence ниже
        // слишком поздний для live monitor. Лимит защищает один tick от
        // старой очереди после временной задержки game thread.
        std::vector<WojTransportEvent> transportEvents;
        uint32_t transportEventsDropped = 0;
        WojRegistry::Instance().DrainTransportEvents(transportEvents, transportEventsDropped,
            MAX_TRANSPORT_EVENTS_PER_UPDATE);
        if (transportEventsDropped)
            WojCombatLog::Instance().WriteDecisionTransportOverflow(transportEventsDropped);
        for (WojTransportEvent const& event : transportEvents)
            WojCombatLog::Instance().WriteDecisionTransport(event);

        // Round 2 (NEW-3): deliberately NOT gated on
        // WojConfig::Instance().Enabled, for the same reason Apply() above
        // is not - keep-alive is about whether the world ticks this spawn
        // at all, independent of who (if anyone) is deciding its actions.
        // Gating it the way the old WojAI::UpdateAI self-heal was gated
        // (behind the AI's own kill switch) is the bug this round exists to
        // fix: WojAI does not even exist when Enable=0.
        //
        // Accumulates diff and calls Sweep() at most once every
        // KEEP_ALIVE_SWEEP_INTERVAL_MS rather than every tick - the core's
        // own comment on this virtual warns against heavy per-tick work
        // (WorldScript.h), and Sweep() has no reason to run on that fine a
        // grain anyway: the respawn window it exists to catch is minutes
        // wide (spawntimesecs), not milliseconds.
        _keepAliveSweepAccumulator += diff;
        if (_keepAliveSweepAccumulator < KEEP_ALIVE_SWEEP_INTERVAL_MS)
            return;
        _keepAliveSweepAccumulator = 0;
        WojKeepAlive::Sweep();

        // C1 (round 3): the only safe place to log what WojRegistry counted
        // on the worker thread's behalf - see WojRegistry::DrainDiagnostics
        // and the call sites it replaces (watchdog release, pre-dispatch
        // freshness drop, stale epoch/token reply). Reuses this
        // same 10s rhythm rather than inventing a second one, per the fix
        // brief; printed only when a counter actually grew, as an increment
        // ("N since last check"), never as a running total.
        uint32_t staleInFlightReleased = 0;
        uint32_t staleDecisionsDropped = 0;
        WojRegistry::Instance().DrainDiagnostics(staleInFlightReleased, staleDecisionsDropped);
        if (staleInFlightReleased)
            LOG_WARN("module",
                     "mod-world-of-jevs: released {} stuck in-flight record(s) in the last {}ms",
                     staleInFlightReleased, KEEP_ALIVE_SWEEP_INTERVAL_MS);
        if (staleDecisionsDropped)
            LOG_DEBUG("module",
                      "mod-world-of-jevs: dropped {} stale dispatch/decision(s) in the last {}ms",
                      staleDecisionsDropped, KEEP_ALIVE_SWEEP_INTERVAL_MS);
    }

    void OnShutdown() override
    {
        WojCombatLog::Instance().StopSceneObservation("restart");
        WojBridge::Instance().Stop();
    }

private:
    uint32 _keepAliveSweepAccumulator = 0;
    static constexpr uint32 KEEP_ALIVE_SWEEP_INTERVAL_MS = 10000;
    static constexpr size_t MAX_TRANSPORT_EVENTS_PER_UPDATE = 128;
};


// A configured successor created by a Jev-owned actor with invalid or stale
// lineage must not silently receive SmartAI. It remains physically present
// (so the harness can report the broken transition) but cannot make stock
// tactical decisions or issue provider requests.
class WojRejectedCreatureAI : public CreatureAI, public WojRejectedRuntimeAI
{
public:
    explicit WojRejectedCreatureAI(Creature* creature) : CreatureAI(creature)
    {
        me->SetReactState(REACT_PASSIVE);
        me->AttackStop();
        me->GetMotionMaster()->MoveIdle();
        LOG_ERROR("module", "mod-world-of-jevs: fail-closed runtime actor entry={} map={} instance={} raw_guid={}",
            me->GetEntry(), me->GetMapId(), me->GetInstanceId(), me->GetGUID().GetRawValue());
        WojCombatLog::Instance().WriteDynamicActorRejected(me);
    }

    void MoveInLineOfSight(Unit* /*who*/) override { }
    void UpdateAI(uint32 /*diff*/) override { }
};

class WojCreatureScript : public CreatureScript
{
public:
    WojCreatureScript() : CreatureScript("npc_woj_murloc") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        // Compatibility only for old DB ScriptName assignments.  Do not
        // manufacture SmartAI in original mode: returning nullptr makes the
        // selector continue to the creature's genuine native factory.
        WojConfig const& cfg = WojConfig::Instance();
        WojRuntimeResolution const resolved = WojRuntimeBinding::Instance().ResolveForSelection(creature);
        if (resolved.status == WojRuntimeResolutionStatus::Rejected)
            return new WojRejectedCreatureAI(creature);
        uint32_t const policyActorId = resolved.policyActorId;
        if (!creature->GetMap() ||
            !cfg.IsJevActor(policyActorId, creature->GetEntry(), creature->GetMapId()))
            return nullptr;
        WojRegistry::Instance().BumpEpoch(WojRuntimeKey(creature), "ai_initialize");
        return new WojAI(creature, policyActorId);
    }
};

class WojAllCreatureScript : public AllCreatureScript
{
public:
    WojAllCreatureScript() : AllCreatureScript("WojAllCreatureScript") { }

    CreatureAI* GetCreatureAI(Creature* creature) const override
    {
        WojConfig const& cfg = WojConfig::Instance();
        WojRuntimeResolution const resolved = WojRuntimeBinding::Instance().ResolveForSelection(creature);
        if (resolved.status == WojRuntimeResolutionStatus::Rejected)
            return new WojRejectedCreatureAI(creature);
        uint32_t const policyActorId = resolved.policyActorId;
        if (!creature->GetMap() ||
            !cfg.IsJevActor(policyActorId, creature->GetEntry(), creature->GetMapId()))
            return nullptr;
        WojRegistry::Instance().BumpEpoch(WojRuntimeKey(creature), "ai_initialize");
        return new WojAI(creature, policyActorId);
    }

    void OnCreatureRemoveWorld(Creature* creature) override
    {
        WojCombatLog::Instance().WriteCreatureLifecycle(creature, "remove_world");
        WojCombatLog::Instance().ForgetOriginalJevRequest(creature);
        WojRuntimeBinding::Instance().Remove(creature);
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 /*diff*/) override
    {
        WojCombatLog::Instance().RecordOriginalJevRequest(creature);
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        WojCombatLog::Instance().WriteCreatureLifecycle(creature, "add_world");
    }
};

void Addmod_world_of_jevsScripts()
{
    new WojWorldScript();
    new WojAllCreatureScript();
    new WojCreatureScript();
    AddWojCommandScripts();
    AddWojCombatLogScripts();
}
