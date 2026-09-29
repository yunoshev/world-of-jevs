#ifndef WOJ_REGISTRY_H
#define WOJ_REGISTRY_H
#include "WojTypes.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <unordered_map>

// One latest-snapshot slot and one in-flight flag per NPC. A new snapshot
// overwrites an unsent one; while a request is in flight no new one leaves.
// The global cap is an emergency brake, not a working mode.
class WojRegistry
{
public:
    static WojRegistry& Instance();

    void PutSnapshot(WojSnapshot const& snap);
    bool TakePendingSnapshot(WojSnapshot& out);
    void DropDispatch(WojActorKey const& actor, uint32_t epoch, uint64_t seq);
    // Returns an empty string when the result was queued for the live AI.
    // A non-empty value is the exact terminal reason when the request
    // completed after its actor had already crossed a cancellation fence.
    std::string SubmitDecision(WojActorKey const& actor, uint32_t epoch, uint64_t seq,
                               WojAction const& action);
    bool TakeAction(WojActorKey const& actor, uint32_t epoch, WojAction& out);
    // Stop bridge work for a sleeping actor without changing its behavior
    // epoch. An already-running HTTP request remains the sole in-flight
    // request, but its eventual result is discarded.
    void CancelPending(WojActorKey const& actor, char const* reason = "deactivated");
    void BumpEpoch(WojActorKey const& actor, char const* reason = "epoch_changed");
    uint32_t CurrentEpoch(WojActorKey const& actor);

    void PushTransportEvent(WojTransportEvent const& event) noexcept;
    // Atomically fence a worker's dispatch against death/deactivation of the
    // snapshot it took earlier. False means no HTTP request may be sent.
    bool TryPushDispatch(WojSnapshot const& snap, WojTransportEvent const& event) noexcept;
    void DrainTransportEvents(std::vector<WojTransportEvent>& out, uint32_t& dropped,
                              size_t maxEvents);

    // C1 (round 3): TakePendingSnapshot and SubmitDecision are called only
    // from WojBridge::Run - the worker thread - which the project's iron
    // rule says must never call LOG_*. Both used to log directly anyway,
    // which held only up to the lexical boundary of Run() itself. They now
    // count instead, in atomics safe to touch from either thread; the game
    // thread drains and logs the *increments*, from WojWorldScript::OnUpdate,
    // alongside the existing keep-alive sweep. Draining is destructive on
    // purpose (exchange, not load): the point is "how many since last time
    // we looked", not a running total that needs its own subtraction.
    void DrainDiagnostics(uint32_t& staleInFlightReleased, uint32_t& staleDecisionsDropped);

    static constexpr size_t MAX_RECORDS = 1000;
    // Insurance against a vanished worker (crashed, killed mid-request, or
    // took a timeout branch that skipped SubmitDecision), not a normal
    // operating mode: a record should never actually sit inFlight this long.
    static constexpr uint32_t IN_FLIGHT_DEADLINE_SECONDS = 30;
    static constexpr size_t MAX_TRANSPORT_EVENTS = 4096;

private:
    struct Record
    {
        WojSnapshot snapshot;
        bool hasSnapshot = false;
        bool queued = false;
        bool inFlight = false;
        uint32_t activeEpoch = 0;
        uint64_t activeSeq = 0;
        bool discardInFlight = false;
        std::string discardReason;
        std::chrono::steady_clock::time_point sentAt{};
        WojAction action;
        bool hasAction = false;
        uint32_t epoch = 1;
    };

    std::mutex _mutex;
    std::unordered_map<WojActorKey, Record, WojActorKeyHash> _records;

    // C1: incremented on the worker thread (inside the mutex-protected
    // functions below, so no extra synchronization is buying anything
    // real - atomic is what lets the game thread drain them without taking
    // that same mutex just to read two counters).
    std::atomic<uint32_t> _staleInFlightReleased{0};
    std::atomic<uint32_t> _staleDecisionsDropped{0};
    std::deque<WojTransportEvent> _transportEvents;
    std::atomic<uint32_t> _transportEventsDropped{0};
    // FIFO of guids with a snapshot waiting to be taken. Without this,
    // TakePendingSnapshot scanned _records from begin() every time, which on
    // several NPCs with a slow gateway starves whichever ones land near the
    // end of unordered_map's (stable, post-warmup) bucket order — they never
    // get served. Queuing by arrival order fixes that.
    std::deque<WojActorKey> _pending;

    void QueueTerminalLocked(Record const& record, char const* reason);
};
#endif
