#ifndef WOJ_CONFIG_H
#define WOJ_CONFIG_H
#include "WojTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class WojMode
{
    Ours,
    Original
};

enum class WojControl : uint8_t { Inherit, Jev, Original };

enum class WojActivationMode : uint8_t { Always, NearPlayer };

struct WojActivationBehavior
{
    WojActivationMode mode = WojActivationMode::Always;
    float radiusYards = 0.0f;
};

struct WojAiInitDespawn
{
    uint32_t chancePct = 0;
    uint32_t delayMs = 0;
};

struct WojActorBinding
{
    // Static actors use their DB spawn id as the policy id. Runtime summons
    // use an explicit high-range actor id so the wire/catalog identity stays
    // positive and stable without pretending that the summon has a DB spawn.
    uint32_t spawnId = 0;
    uint32_t entry = 0;
    uint32_t mapId = 0;
    bool runtimeSummon = false;
    bool sourceGameObject = false;
    bool sourceGameObjectReportUse = false;
    uint32_t sourceActorId = 0;
    uint32_t sourceGameObjectEntry = 0;
    uint32_t createdBySpell = 0;
    // SmartGameObjectAI summons retain the GO raw GUID, not a Unit summoner.
    // Their exact DB source and physical spawn position select one child.
    // One stable policy identity per physical child of a configured cast.
    uint32_t summonIndex = 1;
    WojPoint sourceGameObjectSummonPosition;
    std::string groupId;
    WojControl control = WojControl::Inherit;
    WojControl groupControl = WojControl::Original;
    bool groupEnabled = true;
    bool aggroTalk = false;
    bool ambientTalk = false;
    bool preserveStockRoutine = false;
    bool hasPreferredCombatRange = false;
    float preferredCombatRangeYards = 0.0f;
    // A deliberately actor-scoped executor continuation permit.  This is
    // not a general scheduling policy: schema v3 admits it only for the
    // pinned Taskmaster Shoot observation actor/spell pair.
    bool hasExecutorRepeatContinuationSpell = false;
    uint32_t executorRepeatContinuationSpell = 0;
    // Most actors inherit the group's original flee rule. Runtime children
    // such as Remote-Controlled Golems share their summoner's ownership
    // group but must be able to opt out without splitting assistance scope.
    bool hasFleeEnabledOverride = false;
    bool fleeEnabledOverride = false;
    bool hasAiInitDespawn = false;
    WojAiInitDespawn aiInitDespawn;
    WojActivationBehavior activation;
    uint32_t generation = 1;
};

struct WojTimerRange
{
    uint32_t minimum = 0;
    uint32_t maximum = 0;
};

struct WojSpellBehavior
{
    uint32_t entry = 0;
    uint32_t spell = 0;
    WojTimerRange initial;
    WojTimerRange repeat;
    bool ticksOutOfCombat = false;
    uint32_t chancePct = 100;
    std::string target;
    // Relation (target) and selection are deliberately separate. "enemy"
    // describes what Jev may address; these fields preserve stock selectors
    // such as VICTIM and HOSTILE_RANDOM-within-radius at the executor edge.
    std::string targetSelector = "any";
    bool hasTargetRadius = false;
    float targetRadiusYards = 0.0f;
    bool requireCasterCombat = true;
    bool requireCasterOutOfCombat = false;
    bool requireTargetInCombat = false;
    bool requireEngaged = false;
    bool hasMaxDistance = false;
    float maxDistanceYards = 0.0f;
    // A deliberately narrow admission floor for a source ranged spell whose
    // target can close during the asynchronous Jev decision round.  This is
    // not a replacement for SpellInfo's native minimum-range check.
    bool hasMinSnapshotDistance = false;
    float minSnapshotDistanceYards = 0.0f;
    bool requireAuraAbsent = false;
    bool hasMaxTargetHealthPct = false;
    uint32_t maxTargetHealthPct = 0;
    bool hasMaxCasterHealthPct = false;
    uint32_t maxCasterHealthPct = 0;
    // FRIENDLY_HEALTH_PCT в stock SmartAI не имеет отдельного периода:
    // готовность проверяется с AI tick. Такой trigger не должен получать
    // выдуманный timer только ради общего формата policy.
    bool eventDriven = false;
    bool allowSelf = false;
    bool oncePerEpoch = false;
    std::string completion = "cast";
};

// Reusable source-authored boss choreography. Jev still chooses every
// tactical action between these rails; the executor owns only the exact
// health transition which the stock encounter does not allow an actor to
// skip or reorder.
struct WojHealthPhaseBehavior
{
    std::string id;
    uint32_t actorId = 0;
    uint32_t entry = 0;
    uint32_t triggerBelowHealthPct = 0;
    uint32_t transitionSpell = 0;
    uint32_t talkGroup = 0;
    WojPoint destination;
    uint32_t equipmentId = 0;
    bool dualWield = false;
    uint32_t equipmentDelayMs = 0;
    uint32_t restoreCombatMs = 0;
    uint32_t tacticalDelayMs = 0;
};

struct WojHealthEventBehavior
{
    std::string id;
    uint32_t actorId = 0;
    uint32_t entry = 0;
    uint32_t healthPctMin = 0;
    uint32_t healthPctMax = 0;
    uint32_t castSpell = 0;
    uint32_t talkGroup = 0;
};

enum class WojAggroEventEffectKind : uint8_t { Cast, RemoveAura, Talk };

struct WojAggroEventEffect
{
    WojAggroEventEffectKind kind = WojAggroEventEffectKind::Talk;
    uint32_t spell = 0;
    uint32_t textGroup = 0;
};

struct WojAggroEventBehavior
{
    std::string id;
    uint32_t actorId = 0;
    uint32_t entry = 0;
    std::vector<WojAggroEventEffect> actions;
};

struct WojFleeBehavior
{
    bool enabled = true;
    uint32_t maxHealthPct = 15;
    bool oncePerEpoch = true;
    bool requireInCombat = true;
    bool requireNotCasting = true;
    bool requireEngaged = true;
    float maxDistanceYards = 40.0f;
};

struct WojAggroTalkBehavior
{
    bool enabled = false;
    uint32_t chancePct = 0;
    uint32_t textGroup = 0;
};

// Stock SmartAI UPDATE_OOC talk represented as a reusable timed capability.
// The timer advances only out of combat and dispatches the original
// CreatureTextMgr text/sound path after Jev selects the eligible action.
struct WojAmbientTalkBehavior
{
    bool enabled = false;
    WojTimerRange initial;
    WojTimerRange repeat;
    uint32_t chancePct = 0;
    uint32_t textGroup = 0;
};

// Declarative, hot-reloadable instance progression performed after one Jev
// actor dies. This is intentionally a small generic primitive: encounter
// policies bind the actor, door and instance data without boss-specific C++.
enum class WojDeathEffectKind : uint8_t
{
    CastSpell,
    ActivateGameObject
};

struct WojDeathEffectBehavior
{
    std::string id;
    WojDeathEffectKind kind = WojDeathEffectKind::ActivateGameObject;
    uint32_t actorId = 0;
    uint32_t entry = 0;
    uint32_t spell = 0;
    uint32_t expectedSummonActorId = 0;
    uint32_t expectedSummonCount = 0;
    uint32_t gameObjectSpawnId = 0;
    uint32_t gameObjectEntry = 0;
    uint32_t initialObjectState = 0;
    uint32_t finalObjectState = 0;
    bool setInstanceData = false;
    uint32_t instanceDataIndex = 0;
    uint32_t initialInstanceDataValue = 0;
    uint32_t instanceDataValue = 0;
    uint32_t instanceSaveTokenIndex = 0;
};

struct WojBehaviorPolicy
{
    uint32_t revision = 0;
    std::string sceneId;
    uint32_t mapId = 0;
    bool hasSceneRoster = false;
    std::unordered_set<uint32_t> ownedGuids;
    std::unordered_map<uint32_t, WojSpellBehavior> spells;
    WojFleeBehavior flee;
    WojAggroTalkBehavior aggroTalk;
    WojAmbientTalkBehavior ambientTalk;
    std::string canonical;
    uint32_t schemaVersion = 1;
    std::unordered_map<uint32_t, WojActorBinding> actors;
    std::unordered_map<std::string, WojControl> groupControls;
    std::unordered_map<std::string, WojPoint> arenaAnchors;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, WojSpellBehavior>> actorSpells;
    std::unordered_map<uint32_t, WojFleeBehavior> actorFlee;
    std::unordered_map<uint32_t, WojAggroTalkBehavior> actorAggroTalk;
    std::unordered_map<uint32_t, WojAmbientTalkBehavior> actorAmbientTalk;
    std::unordered_map<uint32_t, std::vector<WojDeathEffectBehavior>> actorDeathEffects;
    std::unordered_map<uint32_t, std::vector<WojHealthPhaseBehavior>> actorHealthPhases;
    std::unordered_map<uint32_t, std::vector<WojHealthEventBehavior>> actorHealthEvents;
    std::unordered_map<uint32_t, std::vector<WojAggroEventBehavior>> actorAggroEvents;
};


class WojConfig
{
public:
    static WojConfig& Instance();
    // At initial config load ObjectMgr has not loaded creature spawns yet.
    // Reloads may preflight a candidate before publish; OnStartup performs
    // the deferred first-boot check before the bridge/AI can act.
    void Load(bool bindingDataReady = false);
    bool ValidateBehaviorBindings();
    void RebuildDerivedCatalog();

    bool Enabled = false;
    // Observe configured original-AI actors without taking ownership.
    bool RecordOriginal = false;
    std::string GatewayUrl;
    // Дефолт обязан совпадать с mod_world_of_jevs.conf.dist: если файл
    // конфигурации потерян или не
    // прочитался, TimeoutMs должен остаться тем же безопасным значением,
    // а не тихо откатиться на другое и выглядеть как необъяснимые обрывы
    // связи с гейтвеем.
    uint32_t TimeoutMs = 3000;
    uint32_t TickMs = 2000;
    uint32_t CombatTickMs = 2000;
    uint32_t CombatFreshnessMs = 4500;
    uint32_t IdleFreshnessMs = 5000;
    std::unordered_set<uint32_t> OwnedGuids;
    std::unordered_map<uint32_t, std::vector<uint32_t>> Spells;
    std::string BehaviorFile;
    WojBehaviorPolicy Behavior;
    bool BehaviorPolicyValid = false;
    bool BehaviorPolicyChanged = false;
    WojSpellBehavior const* FindBehaviorSpell(uint32_t spell) const;
    std::vector<WojSpellBehavior const*> BehaviorSpellsForEntry(uint32_t entry) const;
    std::string SceneId;
    uint32_t SceneMapId = 0;
    WojMode Mode = WojMode::Ours;
    char const* ModeName() const;
    void SetMode(WojMode mode);
    // Геометрия арены закреплена вместе с roster сцены. Нельзя выводить её
    // из случайно загруженных существ в момент команды: reset, смерть или
    // текущий узел патруля не должны сдвигать цель противника.
    bool ArenaDestination(float& x, float& y) const;
    // Empty = off. See WojKeepAlive.cpp for the values understood so far.
    std::string KeepAlive;
    uint32_t BindingGeneration = 1;
    // These are deliberately process-local overrides.  `inherit` removes the
    // corresponding key; policy reloads keep them only while the named actor
    // or group still exists.
    std::unordered_map<std::string, WojControl> GroupOverlays;
    std::unordered_map<uint32_t, WojControl> ActorOverlays;
    WojActorBinding const* FindActor(uint32_t spawnId) const;
    bool IsBoundActor(uint32_t spawnId, uint32_t entry, uint32_t mapId) const;
    WojControl EffectiveControl(uint32_t spawnId) const;
    bool IsJevActor(uint32_t spawnId, uint32_t entry, uint32_t mapId) const;
    bool IsRecordingOriginalActor(uint32_t spawnId, uint32_t entry, uint32_t mapId) const;
    bool SameGroup(uint32_t first, uint32_t second) const;
    std::string GroupId(uint32_t spawnId) const;
    WojSpellBehavior const* FindBehaviorSpellForActor(uint32_t spawnId, uint32_t spell) const;
    WojActorBinding const* FindRuntimeSummonForCast(uint32_t sourceActorId, uint32_t spell) const;
    WojActorBinding const* FindRuntimeSummonForCastIndex(uint32_t sourceActorId, uint32_t spell,
        uint32_t summonIndex) const;
    uint32_t RuntimeSummonCountForCast(uint32_t sourceActorId, uint32_t spell) const;
    std::vector<WojSpellBehavior const*> BehaviorSpellsForActor(uint32_t spawnId, uint32_t entry) const;
    bool ActorAggroTalk(uint32_t spawnId) const;
    bool ActorAmbientTalk(uint32_t spawnId) const;
    WojActivationBehavior const& ActivationForActor(uint32_t spawnId) const;
    WojFleeBehavior const& FleeForActor(uint32_t spawnId) const;
    WojAggroTalkBehavior const& AggroTalkForActor(uint32_t spawnId) const;
    WojAmbientTalkBehavior const& AmbientTalkForActor(uint32_t spawnId) const;
    std::vector<WojDeathEffectBehavior> const& DeathEffectsForActor(uint32_t spawnId) const;
    std::vector<WojHealthPhaseBehavior> const& HealthPhasesForActor(uint32_t spawnId) const;
    std::vector<WojHealthEventBehavior> const& HealthEventsForActor(uint32_t spawnId) const;
    std::vector<WojAggroEventBehavior> const& AggroEventsForActor(uint32_t spawnId) const;
    bool GroupArenaAnchor(std::string const& groupId, WojPoint& out) const;
    bool GroupMapId(std::string const& groupId, uint32_t& out) const;
    std::unordered_set<uint32_t> BindingActors() const;
    std::unordered_set<uint32_t> ChangedBindingActors(WojBehaviorPolicy const& previous) const;
    std::unordered_set<uint32_t> GroupActors(std::string const& id) const;
    bool SetGroupOverlay(std::string const& id, WojControl mode);
    bool SetActorOverlay(uint32_t spawnId, WojControl mode);
};
#endif
