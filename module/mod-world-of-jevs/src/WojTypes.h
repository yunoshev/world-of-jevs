#ifndef WOJ_TYPES_H
#define WOJ_TYPES_H
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// Everything that crosses the thread boundary is plain data. No game pointers
// live here on purpose: the bridge thread must not be able to touch a Creature
// even by accident, because the object may be gone by the time it replies.

struct WojPoint { float x = 0.f, y = 0.f, z = 0.f, o = 0.f; };

// Static spawn id is a catalog binding, not a live-world identity: the same
// DB spawn can exist in two instances of a dungeon at once.  Every mutable
// registry/help slot is therefore keyed by this triple.  rawGuid deliberately
// remains a uint64_t here; the JSON boundary serializes it as a decimal string.
struct WojActorKey
{
    uint32_t mapId = 0;
    uint32_t instanceId = 0;
    uint64_t rawGuid = 0;

    bool operator==(WojActorKey const& other) const
    {
        return mapId == other.mapId && instanceId == other.instanceId && rawGuid == other.rawGuid;
    }
};

struct WojActorKeyHash
{
    size_t operator()(WojActorKey const& key) const noexcept
    {
        size_t value = std::hash<uint32_t>{}(key.mapId);
        value ^= std::hash<uint32_t>{}(key.instanceId) + 0x9e3779b9u + (value << 6) + (value >> 2);
        value ^= std::hash<uint64_t>{}(key.rawGuid) + 0x9e3779b9u + (value << 6) + (value >> 2);
        return value;
    }
};

struct WojUnit
{
    std::string id;
    uint32_t entry = 0;
    std::string name;
    uint32_t level = 0;
    bool isPlayer = false;
    float healthPct = 0.f;
    float distance = 0.f;
    bool engaged = false;
    bool attackingAlly = false;
};

struct WojAlly
{
    std::string id;
    uint32_t guidSpawn = 0;
    uint32_t entry = 0;
    std::string name;
    float healthPct = 0.f;
    float distance = 0.f;
    bool inCombat = false;
};

struct WojSpell
{
    uint32_t id = 0;
    bool ready = false;
    uint32_t cooldownMs = 0;
    std::vector<std::string> validTargets;
    std::vector<std::string> auraTargets;
    bool hasLastCastAge = false;
    uint32_t lastCastAgeMs = 0;
};

struct WojHelpRequest
{
    std::string requestId;
    WojAlly requester;
    uint32_t requesterEpoch = 0;
    WojUnit target;
    uint32_t expiresInMs = 0;
};

enum class WojRoutineProfile : uint8_t
{
    Idle,
    Random,
    Waypoint
};

struct WojRoutine
{
    WojRoutineProfile profile = WojRoutineProfile::Idle;
    float wanderRadius = 0.f;
    uint32_t pathId = 0;
    uint32_t pointCount = 0;
    bool active = false;
    bool canResume = false;
};

struct WojBehaviorTrigger
{
    std::string id;
    uint32_t generation = 1;
    std::string status;
    bool hasWaitMs = false;
    uint32_t waitMs = 0;
    std::string blockedBy;
};

struct WojBehaviorEvent
{
    uint64_t seq = 0;
    std::string triggerId;
    uint32_t generation = 0;
    std::string kind;
    std::string decisionId;
    std::string reason;
};

struct WojBehavior
{
    uint32_t policyRevision = 0;
    uint32_t bindingGeneration = 1;
    std::string groupId;
    std::string sceneId;
    uint64_t seq = 0;
    uint64_t floorSeq = 1;
    std::vector<WojBehaviorTrigger> triggers;
    std::vector<WojBehaviorEvent> events;
};

struct WojSnapshot
{
    WojActorKey actor;
    uint32_t guid = 0;
    uint32_t entry = 0;
    uint32_t epoch = 0;
    uint64_t seq = 0;
    std::string name;
    std::string id;
    std::string arenaId;

    WojPoint position;
    WojPoint home;
    uint32_t health = 0;
    uint32_t healthMax = 0;
    uint32_t mana = 0;
    uint32_t manaMax = 0;
    bool inCombat = false;
    bool fleeing = false;
    bool hasVictim = false;
    WojUnit victim;
    std::vector<WojUnit> attackers;
    bool hasNearestEnemy = false;
    WojUnit nearestEnemy;
    std::vector<WojUnit> enemies;
    std::vector<std::string> attackTargets;
    std::vector<WojAlly> allies;
    std::vector<WojAlly> healAllies;
    std::vector<std::string> callHelpTargets;
    std::vector<std::string> fleeAssistTargets;
    std::vector<std::string> fleeTargets;
    std::vector<WojSpell> spells;
    std::vector<WojHelpRequest> helpRequests;
    WojRoutine routine;
    WojBehavior behavior;

    std::deque<std::string> lastActions;   // at most 3, oldest first
    float secondsSinceLastDecision = 0.f;  // the polling interval — near constant
    float secondsSinceLastAction = 0.f;    // since the NPC last actually did something
    uint32_t freshnessLimitMs = 0;
    std::chrono::steady_clock::time_point capturedAt{};
    std::chrono::steady_clock::time_point enqueuedAt{};
};

// None means "nothing usable came back" — a timeout, a bad response.
// Idle means "the decider looked and chose to do nothing". They lead to the
// same inaction but must stay distinguishable in the log.
enum class WojActionKind : uint8_t
{
    None,
    MoveTo,
    Say,
    StockTalk,
    Idle,
    Attack,
    Cast,
    StartPhase,
    HealthEvent,
    AggroEvent,
    CallHelp,
    FleeForAssist,
    Flee,
    AnswerHelp,
    StopAttack,
    Evade,
    ResumeRoutine
};

struct WojAction
{
    WojActionKind kind = WojActionKind::None;
    WojPoint point;
    std::string text;
    std::string target;
    std::string recipient;
    std::string helpRequestId;
    uint32_t spell = 0;
    std::string phaseId;
    std::string healthEventId;
    std::string aggroEventId;
    uint32_t triggerGeneration = 0;
    std::string triggerId;
    std::string thought;
    std::string marker;      // what to record in lastActions; the decider names it
    // Каким ярусом принято решение: "jev" или "heuristic". Нужно в логе:
    // без него нельзя отличить решение модели от аварийного, а доля
    // эвристики выше порога — признак поломки, а не шероховатости.
    std::string tier;
    std::string decisionId;
    std::string requestId;
    uint32_t snapshotEpoch = 0;
    uint64_t snapshotSeq = 0;
    uint32_t policyRevision = 0;
    uint32_t bindingGeneration = 0;
    uint32_t validForSeconds = 0;
    uint32_t freshnessLimitMs = 0;
    uint32_t captureToEnqueueMs = 0;
    uint32_t ageAtSendMs = 0;
    std::chrono::steady_clock::time_point capturedAt{};
    std::chrono::steady_clock::time_point httpStartedAt{};
    std::chrono::steady_clock::time_point receivedAt{};
};

enum class WojTransportEventKind : uint8_t
{
    Dispatch,
    Result,
    Terminal
};

// Plain-data копия одной попытки bridge. Worker может только положить её в
// очередь; в combat log её превращает game thread. Здесь намеренно нет ни
// указателя Unit, ни тела ответа/ошибки.
struct WojTransportEvent
{
    WojTransportEventKind kind = WojTransportEventKind::Dispatch;
    std::string actorId;
    uint32_t actorGuidSpawn = 0;
    uint32_t actorMapId = 0;
    uint32_t actorInstanceId = 0;
    uint64_t actorRawGuid = 0;
    uint32_t actorEntry = 0;
    std::string actorName;
    std::string arenaId;
    std::string requestId;
    std::string decisionId;
    std::string terminalReason;
    uint32_t epoch = 0;
    uint64_t snapshotSeq = 0;
    uint32_t policyRevision = 0;
    uint32_t bindingGeneration = 0;
    uint32_t captureToEnqueueMs = 0;
    uint32_t ageAtSendMs = 0;
    uint32_t freshnessLimitMs = 0;
    uint32_t httpTimeoutMs = 0;
    std::chrono::steady_clock::time_point capturedAt{};
    std::chrono::steady_clock::time_point sentAt{};
    std::chrono::steady_clock::time_point receivedAt{};
    std::string outcome;
    std::string transportError;
    bool hasHttpStatus = false;
    uint32_t httpStatus = 0;
};
#endif
