#include "WojRegistry.h"
#include "Log.h"
#include <algorithm>
#include <limits>
#include <utility>

WojRegistry& WojRegistry::Instance()
{
    static WojRegistry instance;
    return instance;
}

void WojRegistry::PutSnapshot(WojSnapshot const& snap)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_records.size() >= MAX_RECORDS && !_records.count(snap.actor))
    {
        LOG_ERROR("module", "mod-world-of-jevs: registry full at {} records, dropping guid {}",
                  MAX_RECORDS, snap.actor.rawGuid);
        return;
    }
    Record& rec = _records[snap.actor];
    // Queue the guid only on the hasSnapshot false -> true transition, not on
    // every call: this is a Tick loop calling in every NPC's TickMs, so
    // queuing unconditionally would let the same guid pile up in _pending
    // many times over while its one existing entry has not even been taken
    // yet.
    rec.snapshot = snap;
    rec.hasSnapshot = true;   // overwrites an unsent one on purpose
    if (!rec.queued)
    {
        _pending.push_back(snap.actor);
        rec.queued = true;
    }
}

bool WojRegistry::TakePendingSnapshot(WojSnapshot& out)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto const now = std::chrono::steady_clock::now();
    // Recovery sweep: a record can only leave inFlight through
    // SubmitDecision, so a worker that died, was killed mid-request, or took
    // a timeout branch around that call leaves it stuck forever otherwise.
    // Records survive death, evade, respawn and grid unload (BumpEpoch only
    // touches hasSnapshot/hasAction), so nothing else would ever clear it.
    for (auto& [actor, rec] : _records)
    {
        if (rec.inFlight &&
            std::chrono::duration_cast<std::chrono::seconds>(now - rec.sentAt).count() >= IN_FLIGHT_DEADLINE_SECONDS)
        {
            // C1: this function is called only from WojBridge::Run (the
            // worker thread) - LOG_WARN here used to violate the module's
            // own iron rule about that thread never touching sLog. Count
            // instead; the game thread logs the increment from
            // WojWorldScript::OnUpdate (see DrainDiagnostics below).
            _staleInFlightReleased.fetch_add(1, std::memory_order_relaxed);
            rec.inFlight = false;
            rec.activeEpoch = 0;
            rec.activeSeq = 0;
        }
    }

    // Bound the scan to the queue's current size. A guid can be popped here
    // while its record is still inFlight (a fresher snapshot arrived and
    // queued a second entry for the same guid while the first was still out)
    // — that one goes back to the tail to be retried once the in-flight
    // request completes, rather than being dropped and potentially lost.
    // Anything with nothing left to send is simply dropped: PutSnapshot's
    // next false -> true transition will queue it again.
    // A newly engaged creature must not wait behind an entire backlog of
    // idle snapshots. Preserve FIFO within each class and leave idle work
    // dispatchable when no combat snapshot is ready. This changes dispatch
    // order only; it never chooses or fabricates an NPC action.
    for (bool combatOnly : {true, false})
    {
        size_t attempts = _pending.size();
        while (attempts-- > 0)
        {
            WojActorKey const actor = _pending.front();
            _pending.pop_front();

            auto it = _records.find(actor);
            if (it == _records.end())
                continue;
            it->second.queued = false;
            if (!it->second.hasSnapshot)
                continue; // record gone, or snapshot already taken

            if (it->second.inFlight || it->second.hasAction ||
                (combatOnly && !it->second.snapshot.inCombat))
            {
                _pending.push_back(actor);
                it->second.queued = true;
                continue;
            }

            out = it->second.snapshot;
            it->second.hasSnapshot = false;
            it->second.inFlight = true;
            it->second.discardInFlight = false;
            it->second.activeEpoch = out.epoch;
            it->second.activeSeq = out.seq;
            it->second.sentAt = now;
            return true;
        }
    }
    return false;
}

void WojRegistry::DropDispatch(WojActorKey const& actor, uint32_t epoch, uint64_t seq)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _records.find(actor);
    if (it == _records.end() || !it->second.inFlight ||
        it->second.activeEpoch != epoch || it->second.activeSeq != seq)
        return;
    it->second.inFlight = false;
    it->second.activeEpoch = 0;
    it->second.activeSeq = 0;
    _staleDecisionsDropped.fetch_add(1, std::memory_order_relaxed);
}

std::string WojRegistry::SubmitDecision(WojActorKey const& actor, uint32_t epoch, uint64_t seq,
                                        WojAction const& action)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _records.find(actor);
    if (it == _records.end() || !it->second.inFlight ||
        it->second.activeEpoch != epoch || it->second.activeSeq != seq)
    {
        _staleDecisionsDropped.fetch_add(1, std::memory_order_relaxed);
        return "stale_correlation";
    }
    it->second.inFlight = false;
    it->second.activeEpoch = 0;
    it->second.activeSeq = 0;
    bool const discarded = it->second.discardInFlight;
    it->second.discardInFlight = false;
    std::string discardReason = std::move(it->second.discardReason);
    it->second.discardReason.clear();
    if (discarded || it->second.epoch != epoch)
    {
        _staleDecisionsDropped.fetch_add(1, std::memory_order_relaxed);
        return discardReason.empty() ? "epoch_changed" : discardReason;
    }
    it->second.action = action;
    it->second.hasAction = true;
    return {};
}

void WojRegistry::CancelPending(WojActorKey const& actor, char const* reason)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _records.find(actor);
    if (it == _records.end())
        return;
    it->second.hasSnapshot = false;
    if (it->second.hasAction)
        QueueTerminalLocked(it->second, reason);
    it->second.hasAction = false;
    if (it->second.inFlight)
    {
        it->second.discardInFlight = true;
        it->second.discardReason = reason ? reason : "deactivated";
    }
}

bool WojRegistry::TakeAction(WojActorKey const& actor, uint32_t epoch, WojAction& out)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _records.find(actor);
    if (it == _records.end() || !it->second.hasAction || it->second.epoch != epoch)
        return false;
    out = it->second.action;
    it->second.hasAction = false;
    // Снимок, собранный до применения этого решения, уже не описывает
    // причинно последующее состояние. Новый появится по обычному cadence.
    it->second.hasSnapshot = false;
    return true;
}

void WojRegistry::BumpEpoch(WojActorKey const& actor, char const* reason)
{
    std::lock_guard<std::mutex> lock(_mutex);
    Record& rec = _records[actor];
    if (rec.hasAction)
        QueueTerminalLocked(rec, reason);
    ++rec.epoch;
    rec.hasSnapshot = false;
    rec.hasAction = false;
    rec.discardInFlight = rec.inFlight;
    rec.discardReason = rec.inFlight ? (reason ? reason : "epoch_changed") : "";
    // inFlight is deliberately NOT cleared here. Bumping the epoch forgets
    // what this registry is holding; it cannot forget a request the worker
    // is physically still making. Clearing the flag would only claim no
    // request is out while one is, letting a second dispatch leave for the
    // same guid - and the first reply would then clear the second's flag,
    // so a third could leave before the second ever answered. The stale
    // reply is already harmless: SubmitDecision drops its payload on the
    // epoch mismatch while still releasing the flag, so the next snapshot
    // goes out within one tick. A worker that dies without replying at all
    // is what IN_FLIGHT_DEADLINE_SECONDS is for.
}

void WojRegistry::QueueTerminalLocked(Record const& record, char const* reason)
{
    if (_transportEvents.size() >= MAX_TRANSPORT_EVENTS)
    {
        _transportEventsDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    WojAction const& action = record.action;
    WojSnapshot const& snap = record.snapshot;
    WojTransportEvent event;
    event.kind = WojTransportEventKind::Terminal;
    event.actorId = snap.id;
    event.actorGuidSpawn = snap.guid;
    event.actorMapId = snap.actor.mapId;
    event.actorInstanceId = snap.actor.instanceId;
    event.actorRawGuid = snap.actor.rawGuid;
    event.actorEntry = snap.entry;
    event.actorName = snap.name;
    event.arenaId = snap.arenaId;
    event.requestId = action.requestId;
    event.decisionId = action.decisionId;
    event.terminalReason = reason ? reason : "epoch_changed";
    event.epoch = action.snapshotEpoch;
    event.snapshotSeq = action.snapshotSeq;
    event.policyRevision = action.policyRevision;
    event.bindingGeneration = action.bindingGeneration;
    event.captureToEnqueueMs = action.captureToEnqueueMs;
    event.ageAtSendMs = action.ageAtSendMs;
    event.freshnessLimitMs = action.freshnessLimitMs;
    event.capturedAt = action.capturedAt;
    event.sentAt = action.httpStartedAt;
    event.receivedAt = action.receivedAt;
    _transportEvents.push_back(std::move(event));
}

uint32_t WojRegistry::CurrentEpoch(WojActorKey const& actor)
{
    std::lock_guard<std::mutex> lock(_mutex);
    // find, not operator[]: this is called every tick for every NPC, so
    // operator[] would create a Record here and let MAX_RECORDS's check in
    // PutSnapshot be the only gate in name only. 1 is Record::epoch's own
    // initial value, so an absent record reads the same as a fresh one.
    auto it = _records.find(actor);
    return it != _records.end() ? it->second.epoch : 1;
}

void WojRegistry::PushTransportEvent(WojTransportEvent const& event) noexcept
{
    auto countDrop = [&]()
    {
        uint32_t current = _transportEventsDropped.load(std::memory_order_relaxed);
        while (current < std::numeric_limits<uint32_t>::max() &&
               !_transportEventsDropped.compare_exchange_weak(current, current + 1,
                   std::memory_order_relaxed, std::memory_order_relaxed)) { }
    };

    try
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_transportEvents.size() >= MAX_TRANSPORT_EVENTS)
        {
            countDrop();
            return;
        }
        _transportEvents.push_back(event);
    }
    catch (...)
    {
        // Наблюдаемость не должна менять судьбу HTTP-запроса даже при
        // нехватке памяти или системной ошибке mutex.
        countDrop();
    }
}

bool WojRegistry::TryPushDispatch(WojSnapshot const& snap,
                                  WojTransportEvent const& event) noexcept
{
    try
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto const it = _records.find(snap.actor);
        if (it == _records.end() || !it->second.inFlight ||
            it->second.activeEpoch != snap.epoch || it->second.activeSeq != snap.seq ||
            it->second.discardInFlight || it->second.epoch != snap.epoch)
            return false;
        if (_transportEvents.size() >= MAX_TRANSPORT_EVENTS)
        {
            _transportEventsDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        _transportEvents.push_back(event);
        return true;
    }
    catch (...)
    {
        _transportEventsDropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void WojRegistry::DrainTransportEvents(std::vector<WojTransportEvent>& out, uint32_t& dropped,
                                       size_t maxEvents)
{
    std::lock_guard<std::mutex> lock(_mutex);
    size_t const count = std::min(maxEvents, _transportEvents.size());
    out.reserve(out.size() + count);
    for (size_t i = 0; i < count; ++i)
    {
        out.push_back(std::move(_transportEvents.front()));
        _transportEvents.pop_front();
    }
    dropped = _transportEventsDropped.exchange(0, std::memory_order_relaxed);
}

void WojRegistry::DrainDiagnostics(uint32_t& staleInFlightReleased, uint32_t& staleDecisionsDropped)
{
    // exchange(0), not load(): the caller wants "how many since I last
    // asked", not a running total it would then have to subtract itself.
    // No _mutex needed - these two counters are independent of _records
    // and of each other, and atomic already makes the exchange itself safe
    // against the worker thread incrementing concurrently.
    staleInFlightReleased = _staleInFlightReleased.exchange(0, std::memory_order_relaxed);
    staleDecisionsDropped = _staleDecisionsDropped.exchange(0, std::memory_order_relaxed);
}
