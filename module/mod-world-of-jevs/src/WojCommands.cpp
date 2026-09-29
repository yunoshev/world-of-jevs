#include "WojCommands.h"
#include "WojAI.h"
#include "WojCombatLog.h"
#include "WojConfig.h"
#include "WojHelp.h"
#include "WojRegistry.h"
#include "WojRuntimeIdentity.h"
#include "WojRuntimeBinding.h"
#include "AccountMgr.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Creature.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "RBAC.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{

    Creature* FindOwned(Map* map, uint32_t spawnId)
    {
        auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
        return bounds.first == bounds.second ? nullptr : bounds.first->second;
    }


    // A static spawn id is repeated in every live copy of an instance.  Hot
    // ownership/policy transitions therefore have to visit all loaded maps
    // with the configured map id, not just instance 0.  The callback remains
    // on the world thread; no Creature pointer escapes this enumeration.
    template <class Worker>
    void ForEachLoadedSpawn(uint32_t mapId, uint32_t spawnId, Worker&& worker)
    {
        sMapMgr->DoForAllMapsWithMapId(mapId, [&](Map* map)
        {
            auto const bounds = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
            for (auto it = bounds.first; it != bounds.second; ++it)
                if (it->second)
                    worker(it->second);
        });
    }
}

uint32_t WojReinitializeOwned(char const* reason)
{
    Map* map = sMapMgr->FindMap(WojConfig::Instance().SceneMapId, 0);
    if (!map)
        return 0;

    uint32_t changed = 0;
    WojConfig const& config = WojConfig::Instance();
    for (uint32_t spawnId : config.OwnedGuids)
    {
        Creature* creature = FindOwned(map, spawnId);
        if (!creature || !creature->IsAlive())
            continue;

        if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
            ai->PrepareForReplacement(reason);
        WojRegistry::Instance().BumpEpoch(WojRuntimeKey(creature), reason);
        if (config.Mode == WojMode::Original)
            // WojAI использует REACT_DEFENSIVE для stealth alert. SmartAI
            // сам штатную реакцию не восстанавливает, поэтому это обязано
            // предшествовать замене AI.
            creature->InitializeReactState();
        if (creature->AIM_Initialize())
            ++changed;
    }
    return changed;
}

uint32_t WojReinitializeTransition(WojBehaviorPolicy const& previous, char const* reason)
{
    WojConfig const& config = WojConfig::Instance();
    std::unordered_set<uint32_t> candidates;
    for (auto const& [spawnId, unused] : previous.actors)
    {
        (void)unused;
        candidates.insert(spawnId);
    }
    for (auto const& [spawnId, unused] : config.Behavior.actors)
    {
        (void)unused;
        candidates.insert(spawnId);
    }
    // v1/v2 retain their roster behavior while an operator migrates to v3.
    candidates.insert(previous.ownedGuids.begin(), previous.ownedGuids.end());
    candidates.insert(config.Behavior.ownedGuids.begin(), config.Behavior.ownedGuids.end());
    return WojReinitializeActors(previous, candidates, reason);
}

uint32_t WojReinitializeActors(WojBehaviorPolicy const& previous,
                               std::unordered_set<uint32_t> const& candidates, char const* reason)
{
    WojConfig const& config = WojConfig::Instance();
    uint32_t changed = 0;
    for (uint32_t spawnId : candidates)
    {
        uint32_t mapId = config.SceneMapId;
        if (WojActorBinding const* next = config.FindActor(spawnId))
            mapId = next->mapId;
        else if (auto const old = previous.actors.find(spawnId); old != previous.actors.end())
            mapId = old->second.mapId;
        ForEachLoadedSpawn(mapId, spawnId, [&](Creature* creature)
        {
            if (!creature->IsAlive())
                return;

            // All cancellation and the epoch fence precede publishing through
            // AIM_Initialize.  A result from the old owner can therefore neither
            // act nor remain queued for the new binding.
            if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
                ai->PrepareForReplacement(reason);
            WojActorKey const actor = WojRuntimeKey(creature);
            uint32_t const oldEpoch = WojRegistry::Instance().CurrentEpoch(actor);
            WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(actor, oldEpoch), reason);
            WojRegistry::Instance().BumpEpoch(actor, reason);
            bool const wantsJev = config.IsJevActor(spawnId, creature->GetEntry(), creature->GetMapId());
            if (!wantsJev)
                creature->InitializeReactState();
            if (!creature->AIM_Initialize())
            {
                LOG_ERROR("module", "mod-world-of-jevs: binding transition '{}' refused AIM_Initialize for spawn {} instance {}",
                    reason, spawnId, creature->GetInstanceId());
                return;
            }
            bool const gotJev = dynamic_cast<WojAI*>(creature->AI()) != nullptr;
            bool const rejected = dynamic_cast<WojRejectedRuntimeAI*>(creature->AI()) != nullptr;
            if (rejected || gotJev != wantsJev)
            {
                LOG_ERROR("module", "mod-world-of-jevs: binding transition '{}' selected wrong AI for spawn {} instance {} expected={}",
                    reason, spawnId, creature->GetInstanceId(), wantsJev ? "jev" : "original");
                return;
            }
            if (WojActorBinding const* binding = config.FindActor(spawnId);
                binding && binding->entry != creature->GetEntry())
            {
                LOG_ERROR("module", "mod-world-of-jevs: binding transition '{}' rejected live entry for spawn {} instance {} policy={} live={}",
                    reason, spawnId, creature->GetInstanceId(), binding->entry, creature->GetEntry());
                return;
            }
            ++changed;
        });
    }
    for (WojRuntimeBindingRecord const& record : WojRuntimeBinding::Instance().Records(candidates))
    {
        Map* map = sMapMgr->FindMap(record.actor.mapId, record.actor.instanceId);
        Creature* creature = map ? map->GetCreature(ObjectGuid(record.actor.rawGuid)) : nullptr;
        if (!creature || !creature->IsAlive() || creature->GetEntry() != record.entry)
            continue;
        if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
            ai->PrepareForReplacement(reason);
        WojActorKey const actor = WojRuntimeKey(creature);
        uint32_t const oldEpoch = WojRegistry::Instance().CurrentEpoch(actor);
        WojWriteHelpExpired(WojHelp::Instance().CancelParticipant(actor, oldEpoch), reason);
        WojRegistry::Instance().BumpEpoch(actor, reason);
        bool const wantsJev = config.IsJevActor(
            record.policyActorId, creature->GetEntry(), creature->GetMapId());
        if (!wantsJev)
            creature->InitializeReactState();
        if (!creature->AIM_Initialize())
        {
            LOG_ERROR("module", "mod-world-of-jevs: runtime binding transition '{}' refused AIM_Initialize for actor {} instance {}",
                reason, record.policyActorId, creature->GetInstanceId());
            continue;
        }
        bool const gotJev = dynamic_cast<WojAI*>(creature->AI()) != nullptr;
        bool const rejected = dynamic_cast<WojRejectedRuntimeAI*>(creature->AI()) != nullptr;
        if (rejected || gotJev != wantsJev)
        {
            LOG_ERROR("module", "mod-world-of-jevs: runtime binding transition '{}' selected wrong AI for actor {} instance {} expected={}",
                reason, record.policyActorId, creature->GetInstanceId(), wantsJev ? "jev" : "original");
            continue;
        }
        ++changed;
    }
    return changed;
}

uint32_t WojReloadBehaviorOwned()
{
    uint32_t changed = 0;
    WojConfig const& config = WojConfig::Instance();
    for (uint32_t spawnId : config.OwnedGuids)
    {
        uint32_t mapId = config.SceneMapId;
        if (WojActorBinding const* actor = config.FindActor(spawnId))
            mapId = actor->mapId;
        ForEachLoadedSpawn(mapId, spawnId, [&](Creature* creature)
        {
            if (creature->IsAlive())
            {
                if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
                {
                    ai->ReloadBehaviorPolicy();
                    ++changed;
                }
            }
        });
    }
    for (WojRuntimeBindingRecord const& record : WojRuntimeBinding::Instance().Records(config.OwnedGuids))
    {
        Map* map = sMapMgr->FindMap(record.actor.mapId, record.actor.instanceId);
        Creature* creature = map ? map->GetCreature(ObjectGuid(record.actor.rawGuid)) : nullptr;
        if (!creature || !creature->IsAlive() || creature->GetEntry() != record.entry)
            continue;
        if (WojAI* ai = dynamic_cast<WojAI*>(creature->AI()))
        {
            ai->ReloadBehaviorPolicy();
            ++changed;
        }
    }
    return changed;
}

class WojCommandScript : public CommandScript
{
public:
    WojCommandScript() : CommandScript("WojCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wojCommands =
        {
            { "mode", HandleMode, rbac::RBAC_PERM_COMMAND_RESPAWN, Console::Yes },
            { "group", HandleGroupMode, rbac::RBAC_PERM_COMMAND_RESPAWN, Console::Yes },
            { "npc", HandleNpcMode, rbac::RBAC_PERM_COMMAND_RESPAWN, Console::Yes }
        };
        static ChatCommandTable commands =
        {
            { "woj", wojCommands }
        };
        return commands;
    }

    static bool Authorize(ChatHandler* handler)
    {
        // Console/SOAP уже проходит штатную проверку ACSoap на
        // SEC_ADMINISTRATOR. Для игровой сессии RBAC id команды сам по себе
        // не доказывает GM3, поэтому уровень проверяется явно.
        if (WorldSession* session = handler->GetSession())
        {
            if (session->GetSecurity() < SEC_ADMINISTRATOR)
            {
                handler->SendErrorMessage("WOJ commands require GM level 3");
                return false;
            }
        }
        return true;
    }

    static bool HandleMode(ChatHandler* handler, std::string mode)
    {
        if (!Authorize(handler))
            return false;
        std::transform(mode.begin(), mode.end(), mode.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        WojMode next;
        if (mode == "ours")
            next = WojMode::Ours;
        else if (mode == "original")
            next = WojMode::Original;
        else
        {
            handler->SendErrorMessage("Usage: .woj mode <ours|original>");
            return false;
        }

        WojConfig& config = WojConfig::Instance();
        if (config.Behavior.schemaVersion == 3)
        {
            handler->SendErrorMessage("schema v3 uses .woj group or .woj npc mode overlays");
            return false;
        }
        std::string const previous = config.ModeName();
        WojWriteHelpExpired(WojHelp::Instance().Clear(), "mode");
        config.SetMode(next);
        uint32_t const changed = WojReinitializeOwned("mode");
        WojCombatLog::Instance().WriteMode(previous.c_str(), config.ModeName());
        handler->PSendSysMessage("WOJ mode: {} (reinitialized={})", config.ModeName(), changed);
        return true;
    }

    static bool ParseBindingMode(std::string mode, WojControl& out)
    {
        std::transform(mode.begin(), mode.end(), mode.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (mode == "inherit")
            out = WojControl::Inherit;
        else if (mode == "jev")
            out = WojControl::Jev;
        else if (mode == "original")
            out = WojControl::Original;
        else
            return false;
        return true;
    }

    static bool HandleGroupMode(ChatHandler* handler, std::string id, std::string verb, std::string mode)
    {
        if (!Authorize(handler))
            return false;
        WojControl control;
        if (verb != "mode" || !ParseBindingMode(mode, control))
        {
            handler->SendErrorMessage("Usage: .woj group <group_id> mode <inherit|jev|original>");
            return false;
        }
        WojConfig& config = WojConfig::Instance();
        WojBehaviorPolicy const old = config.Behavior;
        if (!config.SetGroupOverlay(id, control))
        {
            handler->SendErrorMessage("WOJ group is unknown");
            return false;
        }
        uint32_t const changed = WojReinitializeActors(old, config.GroupActors(id), "group_overlay");
        handler->PSendSysMessage("WOJ group {} mode={} (reinitialized={})", id, mode, changed);
        return true;
    }

    static bool HandleNpcMode(ChatHandler* handler, uint32_t spawnId, std::string verb, std::string mode)
    {
        if (!Authorize(handler))
            return false;
        WojControl control;
        if (spawnId == 0 || verb != "mode" || !ParseBindingMode(mode, control))
        {
            handler->SendErrorMessage("Usage: .woj npc <spawn_id> mode <inherit|jev|original>");
            return false;
        }
        WojConfig& config = WojConfig::Instance();
        WojBehaviorPolicy const old = config.Behavior;
        if (!config.SetActorOverlay(spawnId, control))
        {
            handler->SendErrorMessage("WOJ NPC binding is unknown");
            return false;
        }
        std::unordered_set<uint32_t> const actor { spawnId };
        uint32_t const changed = WojReinitializeActors(old, actor, "npc_overlay");
        handler->PSendSysMessage("WOJ npc {} mode={} (reinitialized={})", spawnId, mode, changed);
        return true;
    }



};

void AddWojCommandScripts()
{
    new WojCommandScript();
}
