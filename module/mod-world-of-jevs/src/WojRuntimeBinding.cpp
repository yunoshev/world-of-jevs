#include "WojRuntimeBinding.h"
#include "WojAI.h"
#include "WojCombatLog.h"
#include "WojConfig.h"
#include "WojRuntimeIdentity.h"
#include "Creature.h"
#include "GameObject.h"
#include "Log.h"
#include "Spell.h"
#include "TemporarySummon.h"
#include <chrono>

namespace
{
uint64_t MonotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

WojActorKey GameObjectKey(GameObject const* go)
{
    return {go->GetMapId(), go->GetInstanceId(), go->GetGUID().GetRawValue()};
}
}

WojRuntimeBinding& WojRuntimeBinding::Instance()
{
    static WojRuntimeBinding instance;
    return instance;
}

uint32_t WojRuntimeBinding::Resolve(Creature const* creature, bool allowBind)
{
    if (!creature || !creature->GetMap())
        return 0;
    if (uint32_t const spawnId = creature->GetSpawnId())
        return spawnId;
    if (!allowBind)
    {
        WojActorKey const key = WojRuntimeKey(creature);
        std::lock_guard<std::mutex> guard(_lock);
        if (_rejected.count(key))
            return 0;
        auto found = _records.find(key);
        if (found == _records.end())
            return 0;
        WojActorBinding const* actor = WojConfig::Instance().FindActor(found->second.policyActorId);
        if (!actor || !actor->runtimeSummon || actor->entry != creature->GetEntry() ||
            actor->mapId != creature->GetMapId() || actor->sourceActorId != found->second.sourceActorId ||
            actor->sourceGameObject != found->second.sourceGameObject ||
            actor->sourceGameObjectReportUse != found->second.sourceGameObjectReportUse ||
            actor->sourceGameObjectEntry != found->second.sourceGameObjectEntry ||
            actor->sourceGameObjectSummonPosition.x != found->second.sourceGameObjectSummonPosition.x ||
            actor->sourceGameObjectSummonPosition.y != found->second.sourceGameObjectSummonPosition.y ||
            actor->sourceGameObjectSummonPosition.z != found->second.sourceGameObjectSummonPosition.z ||
            actor->createdBySpell != found->second.createdBySpell ||
            actor->summonIndex != found->second.summonIndex)
        {
            // Preserve provenance so a later AI re-selection can reject the
            // now-incompatible child instead of treating it as unmanaged.
            return 0;
        }
        found->second.policyRevision = WojConfig::Instance().Behavior.revision;
        found->second.bindingGeneration = actor->generation;
        return found->second.policyActorId;
    }
    WojRuntimeResolution const resolved = ResolveForSelection(creature);
    return resolved.status == WojRuntimeResolutionStatus::Bound ? resolved.policyActorId : 0;
}

WojRuntimeResolution WojRuntimeBinding::ResolveForSelection(Creature const* creature)
{
    if (!creature || !creature->GetMap())
        return {};
    if (uint32_t const spawnId = creature->GetSpawnId())
        return {WojRuntimeResolutionStatus::Bound, spawnId};

    WojActorKey const key = WojRuntimeKey(creature);
    {
        std::lock_guard<std::mutex> guard(_lock);
        if (_rejected.count(key))
            return {WojRuntimeResolutionStatus::Rejected, 0};
        auto found = _records.find(key);
        if (found != _records.end())
        {
            WojActorBinding const* actor = WojConfig::Instance().FindActor(found->second.policyActorId);
            if (actor && actor->runtimeSummon && actor->entry == creature->GetEntry() &&
                actor->mapId == creature->GetMapId() &&
                actor->sourceActorId == found->second.sourceActorId &&
                actor->sourceGameObject == found->second.sourceGameObject &&
                actor->sourceGameObjectReportUse == found->second.sourceGameObjectReportUse &&
                actor->sourceGameObjectEntry == found->second.sourceGameObjectEntry &&
                actor->sourceGameObjectSummonPosition.x == found->second.sourceGameObjectSummonPosition.x &&
                actor->sourceGameObjectSummonPosition.y == found->second.sourceGameObjectSummonPosition.y &&
                actor->sourceGameObjectSummonPosition.z == found->second.sourceGameObjectSummonPosition.z &&
                actor->createdBySpell == found->second.createdBySpell &&
                actor->summonIndex == found->second.summonIndex)
            {
                found->second.policyRevision = WojConfig::Instance().Behavior.revision;
                found->second.bindingGeneration = actor->generation;
                return {WojRuntimeResolutionStatus::Bound, found->second.policyActorId};
            }
            // The physical child is known to have been created through a
            // Jev permit. A later incompatible policy must keep it
            // fail-closed for the rest of its lifetime, never erase its
            // provenance and let native SmartAI take over.
            _rejected.insert(key);
            return {WojRuntimeResolutionStatus::Rejected, 0};
        }
    }

    if (!creature->IsSummon())
        return {};
    TempSummon const* summon = creature->ToTempSummon();
    GameObject* sourceGo = summon ? summon->GetSummonerGameObject() : nullptr;
    if (sourceGo && sourceGo->GetMap() == creature->GetMap() &&
        sourceGo->GetInstanceId() == creature->GetInstanceId())
    {
        WojActorBinding const* candidate = nullptr;
        bool hasCandidate = false;
        bool jevCandidate = false;
        bool ambiguous = false;
        for (auto const& [actorId, actor] : WojConfig::Instance().Behavior.actors)
        {
            (void)actorId;
            if (!actor.runtimeSummon || !actor.sourceGameObject ||
                actor.mapId != creature->GetMapId() ||
                actor.sourceActorId != sourceGo->GetSpawnId() ||
                actor.sourceGameObjectEntry != sourceGo->GetEntry())
                continue;
            hasCandidate = true;
            jevCandidate = jevCandidate || WojConfig::Instance().IsJevActor(
                actor.spawnId, actor.entry, actor.mapId);
            if (actor.entry != creature->GetEntry())
                continue;
            float const dx = actor.sourceGameObjectSummonPosition.x - creature->GetPositionX();
            float const dy = actor.sourceGameObjectSummonPosition.y - creature->GetPositionY();
            float const dz = actor.sourceGameObjectSummonPosition.z - creature->GetPositionZ();
            if (dx * dx + dy * dy + dz * dz > 1.0f)
                continue;
            if (candidate)
            {
                ambiguous = true;
                continue;
            }
            candidate = &actor;
        }
        if (hasCandidate)
        {
            // Original ownership keeps stock SmartAI. A Jev-owned GO child
            // without the item-cast permit must never silently use stock AI.
            if (!jevCandidate)
                return {WojRuntimeResolutionStatus::Unmanaged, 0};
            if (!candidate || ambiguous || !WojConfig::Instance().IsJevActor(candidate->spawnId,
                    creature->GetEntry(), creature->GetMapId()))
            {
                std::lock_guard<std::mutex> guard(_lock);
                _rejected.insert(key);
                return {WojRuntimeResolutionStatus::Rejected, 0};
            }
            WojActorKey const sourceKey = GameObjectKey(sourceGo);
            {
                std::lock_guard<std::mutex> guard(_lock);
                auto permit = _gameObjectPermits.find(sourceKey);
                if (permit == _gameObjectPermits.end() || permit->second.invalid ||
                    permit->second.expiresMs < MonotonicMs() ||
                    permit->second.spell != candidate->createdBySpell ||
                    permit->second.reportUse != candidate->sourceGameObjectReportUse ||
                    permit->second.expectedEntry != creature->GetEntry() ||
                    permit->second.consumed + 1 != candidate->summonIndex ||
                    permit->second.consumed >= permit->second.expectedCount)
                {
                    _rejected.insert(key);
                    return {WojRuntimeResolutionStatus::Rejected, 0};
                }
                ++permit->second.consumed;
                WojRuntimeBindingRecord record;
                record.actor = key;
                record.policyActorId = candidate->spawnId;
                record.entry = creature->GetEntry();
                record.source = sourceKey;
                record.sourceActorId = sourceGo->GetSpawnId();
                record.sourceGameObject = true;
                record.sourceGameObjectReportUse = candidate->sourceGameObjectReportUse;
                record.sourceGameObjectEntry = sourceGo->GetEntry();
                record.sourceGameObjectSummonPosition = candidate->sourceGameObjectSummonPosition;
                record.createdBySpell = candidate->createdBySpell;
                record.summonIndex = candidate->summonIndex;
                record.policyRevision = WojConfig::Instance().Behavior.revision;
                record.bindingGeneration = candidate->generation;
                _records[key] = record;
            }
            LOG_INFO("module", "mod-world-of-jevs: GO dynamic actor bound policy_actor={} entry={} source_go={} spell={} map={} instance={} raw_guid={}",
                candidate->spawnId, creature->GetEntry(), sourceGo->GetSpawnId(),
                candidate->createdBySpell, creature->GetMapId(), creature->GetInstanceId(),
                creature->GetGUID().GetRawValue());
            WojCombatLog::Instance().WriteGoDynamicActorBinding(
                creature, sourceGo, candidate->spawnId, candidate->createdBySpell,
                candidate->summonIndex, candidate->sourceGameObjectReportUse);
            return {WojRuntimeResolutionStatus::Bound, candidate->spawnId};
        }
    }
    Unit* sourceUnit = summon ? summon->GetSummonerUnit() : nullptr;
    Creature* source = sourceUnit ? sourceUnit->ToCreature() : nullptr;
    if (!source || source->GetMap() != creature->GetMap() ||
        source->GetInstanceId() != creature->GetInstanceId())
        return {};

    WojAI const* sourceAI = dynamic_cast<WojAI const*>(source->AI());
    uint32_t const sourceActorId = sourceAI ? sourceAI->PolicyActorId() : source->GetSpawnId();
    WojActorKey const sourceKey = WojRuntimeKey(source);
    bool hasCandidate = false;
    for (auto const& [actorId, actor] : WojConfig::Instance().Behavior.actors)
        if (actor.runtimeSummon && !actor.sourceGameObject && actor.entry == creature->GetEntry() &&
            actor.mapId == creature->GetMapId() && actor.sourceActorId == sourceActorId)
        {
            (void)actorId;
            hasCandidate = true;
        }
    bool activePermit = false;
    {
        std::lock_guard<std::mutex> guard(_lock);
        activePermit = _permits.count(sourceKey) != 0;
    }
    if (!hasCandidate)
    {
        if (activePermit)
        {
            std::lock_guard<std::mutex> guard(_lock);
            _rejected.insert(key);
            return {WojRuntimeResolutionStatus::Rejected, 0};
        }
        return {WojRuntimeResolutionStatus::Unmanaged, 0};
    }
    // Original-mode stock summons must retain SmartAI. A configured runtime
    // candidate becomes fail-closed only when a WojAI source produced it.
    if (!sourceAI)
    {
        if (activePermit)
        {
            std::lock_guard<std::mutex> guard(_lock);
            _rejected.insert(key);
            return {WojRuntimeResolutionStatus::Rejected, 0};
        }
        return {WojRuntimeResolutionStatus::Unmanaged, 0};
    }

    uint32_t const observedSpell = creature->GetUInt32Value(UNIT_CREATED_BY_SPELL);
    uint32_t candidateId = 0;
    uint32_t createdBySpell = 0;
    {
        WojRuntimeBindingRecord record;
        std::lock_guard<std::mutex> guard(_lock);
        auto permit = _permits.find(sourceKey);
        if (permit == _permits.end() || permit->second.expectedEntry != creature->GetEntry() ||
            (observedSpell && observedSpell != permit->second.spell) ||
            permit->second.consumed >= permit->second.expectedCount)
        {
            _rejected.insert(key);
            return {WojRuntimeResolutionStatus::Rejected, 0};
        }
        WojActorBinding const* candidate = WojConfig::Instance().FindRuntimeSummonForCastIndex(
            sourceActorId, permit->second.spell, permit->second.consumed + 1);
        WojActorBinding const* anchor = WojConfig::Instance().FindRuntimeSummonForCast(
            sourceActorId, permit->second.spell);
        if (!anchor || anchor->spawnId != permit->second.policyActorId ||
            WojConfig::Instance().RuntimeSummonCountForCast(sourceActorId, permit->second.spell) !=
                permit->second.expectedCount ||
            !candidate || candidate->entry != creature->GetEntry() ||
            candidate->mapId != creature->GetMapId())
        {
            _rejected.insert(key);
            return {WojRuntimeResolutionStatus::Rejected, 0};
        }
        ++permit->second.consumed;
        candidateId = candidate->spawnId;
        createdBySpell = candidate->createdBySpell;
        record.actor = key;
        record.policyActorId = candidateId;
        record.entry = creature->GetEntry();
        record.source = sourceKey;
        record.sourceActorId = sourceActorId;
        record.createdBySpell = createdBySpell;
        record.summonIndex = candidate->summonIndex;
        record.policyRevision = WojConfig::Instance().Behavior.revision;
        record.bindingGeneration = candidate->generation;
        _records[key] = record;
    }
    LOG_INFO("module", "mod-world-of-jevs: dynamic actor bound policy_actor={} entry={} source_actor={} spell={} map={} instance={} raw_guid={}",
        candidateId, creature->GetEntry(), sourceActorId, createdBySpell,
        creature->GetMapId(), creature->GetInstanceId(), creature->GetGUID().GetRawValue());
    WojCombatLog::Instance().WriteDynamicActorBinding(
        creature, source, candidateId, createdBySpell);
    return {WojRuntimeResolutionStatus::Bound, candidateId};
}

bool WojRuntimeBinding::BeginSummonPermit(Creature const* source, uint32_t spell,
                                          uint32_t expectedActorId, uint32_t expectedCount)
{
    if (!source || !source->GetMap() || !spell || !expectedActorId || !expectedCount)
        return false;
    uint32_t const sourceActorId = Resolve(source);
    WojActorBinding const* actor = WojConfig::Instance().FindActor(expectedActorId);
    if (!actor || !actor->runtimeSummon || actor->sourceActorId != sourceActorId ||
        actor->mapId != source->GetMapId() || actor->createdBySpell != spell ||
        actor->summonIndex != 1 ||
        WojConfig::Instance().RuntimeSummonCountForCast(sourceActorId, spell) != expectedCount)
        return false;
    std::lock_guard<std::mutex> guard(_lock);
    return _permits.emplace(WojRuntimeKey(source),
        SummonPermit{spell, expectedActorId, actor->entry, expectedCount, 0}).second;
}

bool WojRuntimeBinding::FinishSummonPermit(Creature const* source, uint32_t spell,
                                           uint32_t expectedActorId, uint32_t expectedCount)
{
    if (!source)
        return false;
    std::lock_guard<std::mutex> guard(_lock);
    auto permit = _permits.find(WojRuntimeKey(source));
    if (permit == _permits.end())
        return false;
    bool const exact = permit->second.spell == spell &&
        permit->second.policyActorId == expectedActorId &&
        permit->second.expectedCount == expectedCount &&
        permit->second.consumed == expectedCount;
    _permits.erase(permit);
    return exact;
}

void WojRuntimeBinding::ObserveGameObjectItemCast(Spell const* spell, Unit const* caster,
                                                   SpellInfo const* spellInfo, uint64_t itemRawGuid)
{
    if (!spell || !caster || !caster->ToPlayer() || !spellInfo || !itemRawGuid)
        return;
    GameObject* source = spell->m_targets.GetGOTarget();
    if (!source || !source->GetSpawnId() || source->GetMap() != caster->GetMap() ||
        source->GetInstanceId() != caster->GetInstanceId())
        return;
    uint32_t expectedEntry = 0;
    uint32_t expectedCount = 0;
    for (auto const& [actorId, actor] : WojConfig::Instance().Behavior.actors)
    {
        (void)actorId;
        if (!actor.runtimeSummon || !actor.sourceGameObject || actor.sourceGameObjectReportUse ||
            actor.mapId != source->GetMapId() ||
            actor.sourceActorId != source->GetSpawnId() ||
            actor.sourceGameObjectEntry != source->GetEntry() ||
            actor.createdBySpell != spellInfo->Id ||
            !WojConfig::Instance().IsJevActor(actor.spawnId, actor.entry, actor.mapId))
            continue;
        if (expectedEntry && expectedEntry != actor.entry)
            return;
        expectedEntry = actor.entry;
        ++expectedCount;
    }
    if (!expectedCount)
        return;
    std::lock_guard<std::mutex> guard(_lock);
    WojActorKey const sourceKey = GameObjectKey(source);
    GameObjectSummonPermit const proposed{spellInfo->Id, expectedEntry, expectedCount, 0,
        MonotonicMs() + 15'000, false, false};
    auto [permit, inserted] = _gameObjectPermits.emplace(sourceKey, proposed);
    if (!inserted)
    {
        if (permit->second.expiresMs < MonotonicMs() ||
            permit->second.consumed == permit->second.expectedCount)
            permit->second = proposed;
        else
            // Two overlapping uses of one GO cannot be assigned to distinct
            // physical children without ambiguity; poison both lineages.
            permit->second.invalid = true;
    }
}

bool WojRuntimeBinding::BeginGameObjectReportUsePermit(GameObject const* source)
{
    if (!source || !source->GetMap() || !source->GetSpawnId())
        return false;
    uint32_t expectedEntry = 0;
    uint32_t expectedCount = 0;
    for (auto const& [actorId, actor] : WojConfig::Instance().Behavior.actors)
    {
        (void)actorId;
        if (!actor.runtimeSummon || !actor.sourceGameObject ||
            !actor.sourceGameObjectReportUse || actor.mapId != source->GetMapId() ||
            actor.sourceActorId != source->GetSpawnId() ||
            actor.sourceGameObjectEntry != source->GetEntry())
            continue;
        if (!WojConfig::Instance().IsJevActor(actor.spawnId, actor.entry, actor.mapId) ||
            (expectedEntry && expectedEntry != actor.entry))
            return false;
        expectedEntry = actor.entry;
        ++expectedCount;
    }
    if (!expectedCount || expectedCount > 8)
        return false;
    std::lock_guard<std::mutex> guard(_lock);
    WojActorKey const key = GameObjectKey(source);
    auto [permit, inserted] = _gameObjectPermits.emplace(key,
        GameObjectSummonPermit{0, expectedEntry, expectedCount, 0,
            MonotonicMs() + 30'000, false, true});
    if (!inserted)
    {
        permit->second.invalid = true;
        return false;
    }
    return true;
}

bool WojRuntimeBinding::FinishGameObjectReportUsePermit(GameObject const* source)
{
    if (!source)
        return false;
    std::lock_guard<std::mutex> guard(_lock);
    auto permit = _gameObjectPermits.find(GameObjectKey(source));
    if (permit == _gameObjectPermits.end())
        return false;
    bool const exact = !permit->second.invalid && permit->second.reportUse &&
        permit->second.spell == 0 && permit->second.expectedCount != 0 &&
        permit->second.consumed == permit->second.expectedCount &&
        permit->second.expiresMs >= MonotonicMs();
    if (exact || permit->second.invalid || permit->second.expiresMs < MonotonicMs())
    {
        LOG_INFO("module",
            "mod-world-of-jevs: GO report-use permit source_go={} map={} instance={} "
            "expected={} consumed={} invalid={} exact={}",
            source->GetSpawnId(), source->GetMapId(), source->GetInstanceId(),
            permit->second.expectedCount, permit->second.consumed,
            permit->second.invalid, exact);
        _gameObjectPermits.erase(permit);
    }
    return exact;
}

void WojRuntimeBinding::Remove(Creature const* creature)
{
    if (!creature)
        return;
    std::lock_guard<std::mutex> guard(_lock);
    WojActorKey const key = WojRuntimeKey(creature);
    if (!creature->GetSpawnId())
    {
        _records.erase(key);
        _rejected.erase(key);
    }
    _permits.erase(key);
}

std::vector<WojRuntimeBindingRecord> WojRuntimeBinding::Records(
    std::unordered_set<uint32_t> const& policyActorIds) const
{
    std::vector<WojRuntimeBindingRecord> result;
    std::lock_guard<std::mutex> guard(_lock);
    for (auto const& [key, record] : _records)
    {
        (void)key;
        if (policyActorIds.count(record.policyActorId))
            result.push_back(record);
    }
    return result;
}
