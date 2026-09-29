#ifndef WOJ_RUNTIME_BINDING_H
#define WOJ_RUNTIME_BINDING_H

#include "WojTypes.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Creature;
class GameObject;
class Spell;
class Unit;
class SpellInfo;

struct WojRuntimeBindingRecord
{
    WojActorKey actor;
    uint32_t policyActorId = 0;
    uint32_t entry = 0;
    WojActorKey source;
    uint32_t sourceActorId = 0;
    bool sourceGameObject = false;
    bool sourceGameObjectReportUse = false;
    uint32_t sourceGameObjectEntry = 0;
    WojPoint sourceGameObjectSummonPosition;
    uint32_t createdBySpell = 0;
    uint32_t summonIndex = 1;
    uint32_t policyRevision = 0;
    uint32_t bindingGeneration = 0;
};

enum class WojRuntimeResolutionStatus : uint8_t { Unmanaged, Bound, Rejected };

struct WojRuntimeResolution
{
    WojRuntimeResolutionStatus status = WojRuntimeResolutionStatus::Unmanaged;
    uint32_t policyActorId = 0;
};

// Process-local binding for creatures which have no DB spawn id. The
// durable policy selects them by exact summon lineage; the live table keeps
// that decision available after the summoner corpse leaves the map and lets
// hot ownership transitions find every active instance copy safely.
class WojRuntimeBinding
{
public:
    static WojRuntimeBinding& Instance();

    // Returns a static spawn id or a validated runtime actor id. With
    // allowBind=false this never creates a lineage binding.
    uint32_t Resolve(Creature const* creature, bool allowBind = false);
    WojRuntimeResolution ResolveForSelection(Creature const* creature);
    // Some pinned-core summon paths erase UNIT_CREATED_BY_SPELL. A scoped
    // permit supplies exact provenance during a configured Jev cast (or a
    // synchronous death cast); GetCreatureAI consumes it reentrantly without
    // holding the lock through CastSpell.
    bool BeginSummonPermit(Creature const* source, uint32_t spell,
                           uint32_t expectedActorId, uint32_t expectedCount);
    bool FinishSummonPermit(Creature const* source, uint32_t spell,
                            uint32_t expectedActorId, uint32_t expectedCount);
    // Item-backed GO cast authorizes a short, exact GO-origin summon window.
    void ObserveGameObjectItemCast(Spell const* spell, Unit const* caster,
                                   SpellInfo const* spellInfo, uint64_t itemRawGuid);
    bool BeginGameObjectReportUsePermit(GameObject const* source);
    bool FinishGameObjectReportUsePermit(GameObject const* source);
    void Remove(Creature const* creature);
    std::vector<WojRuntimeBindingRecord> Records(
        std::unordered_set<uint32_t> const& policyActorIds) const;

private:
    struct SummonPermit
    {
        uint32_t spell = 0;
        uint32_t policyActorId = 0;
        uint32_t expectedEntry = 0;
        uint32_t expectedCount = 0;
        uint32_t consumed = 0;
    };
    mutable std::mutex _lock;
    std::unordered_map<WojActorKey, WojRuntimeBindingRecord, WojActorKeyHash> _records;
    std::unordered_map<WojActorKey, SummonPermit, WojActorKeyHash> _permits;
    struct GameObjectSummonPermit
    {
        uint32_t spell = 0;
        uint32_t expectedEntry = 0;
        uint32_t expectedCount = 0;
        uint32_t consumed = 0;
        uint64_t expiresMs = 0;
        bool invalid = false;
        bool reportUse = false;
    };
    std::unordered_map<WojActorKey, GameObjectSummonPermit, WojActorKeyHash> _gameObjectPermits;
    // A runtime child rejected once stays fail-closed until physical removal.
    std::unordered_set<WojActorKey, WojActorKeyHash> _rejected;
};

#endif
