#include "WojHelp.h"
#include "WojCombatLog.h"
#include "WojConfig.h"
#include "WojRegistry.h"
#include "Creature.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include <algorithm>
#include <utility>

namespace
{
    constexpr size_t MAX_SEEN_DECISIONS = 128;
}

WojHelp& WojHelp::Instance()
{
    static WojHelp instance;
    return instance;
}

void WojHelp::RememberDecision(std::string const& decisionId)
{
    if (decisionId.empty() || !_seenDecisions.insert(decisionId).second)
        return;
    _seenOrder.push_back(decisionId);
    while (_seenOrder.size() > MAX_SEEN_DECISIONS)
    {
        _seenDecisions.erase(_seenOrder.front());
        _seenOrder.pop_front();
    }
}

bool WojHelp::Create(WojHelpRecord& record)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (record.decisionId.empty() || _seenDecisions.count(record.decisionId))
        return false;
    // Exactly-once applies to attempts as well as successful records. A
    // decision rejected because another request is active must never become
    // executable later when replayed after that request disappears.
    RememberDecision(record.decisionId);

    bool const existing = std::any_of(_records.begin(), _records.end(), [&](WojHelpRecord const& item)
    {
        return item.requesterKey == record.requesterKey && item.requesterEpoch == record.requesterEpoch;
    });
    if (existing || _records.size() >= MAX_RECORDS)
        return false;

    if (_nextId == 0)
        _nextId = 1;
    record.id = std::to_string(_nextId++);
    record.createdAt = std::chrono::steady_clock::now();
    record.expiresAt = record.createdAt + std::chrono::milliseconds(TTL_MS);
    _records.push_back(record);
    return true;
}

bool WojHelp::HasOutstandingRequest(WojActorKey const& requester, uint32_t requesterEpoch) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return std::any_of(_records.begin(), _records.end(), [&](WojHelpRecord const& item)
    {
        // Expire() is the single removal boundary. Create() rejects this
        // record until then, so snapshot suppression must do the same.
        return item.requesterKey == requester && item.requesterEpoch == requesterEpoch;
    });
}

bool WojHelp::Peek(std::string const& id, WojActorKey const& recipient, uint32_t recipientEpoch, WojHelpRecord& out) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const now = std::chrono::steady_clock::now();
    auto const it = std::find_if(_records.begin(), _records.end(), [&](WojHelpRecord const& item)
    {
        return item.id == id && item.recipientKey == recipient && item.recipientEpoch == recipientEpoch;
    });
    if (it == _records.end() || it->expiresAt <= now)
        return false;
    out = *it;
    return true;
}

bool WojHelp::Consume(std::string const& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const it = std::find_if(_records.begin(), _records.end(), [&](WojHelpRecord const& item)
    {
        return item.id == id;
    });
    if (it == _records.end())
        return false;
    _records.erase(it);
    return true;
}

std::vector<WojHelpRecord> WojHelp::Inbox(WojActorKey const& recipient, uint32_t recipientEpoch) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const now = std::chrono::steady_clock::now();
    std::vector<WojHelpRecord> result;
    for (WojHelpRecord const& item : _records)
        if (item.recipientKey == recipient && item.recipientEpoch == recipientEpoch && item.expiresAt > now)
            result.push_back(item);
    std::sort(result.begin(), result.end(), [](WojHelpRecord const& left, WojHelpRecord const& right)
    {
        return left.expiresAt == right.expiresAt ? left.id < right.id : left.expiresAt < right.expiresAt;
    });
    if (result.size() > MAX_INBOX)
        result.resize(MAX_INBOX);
    return result;
}

std::vector<WojHelpRecord> WojHelp::Expire()
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto const now = std::chrono::steady_clock::now();
    std::vector<WojHelpRecord> result;
    for (auto it = _records.begin(); it != _records.end();)
    {
        if (it->expiresAt <= now)
        {
            result.push_back(*it);
            it = _records.erase(it);
        }
        else
            ++it;
    }
    return result;
}

std::vector<WojHelpRecord> WojHelp::CancelParticipant(WojActorKey const& participant, uint32_t epoch)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<WojHelpRecord> result;
    for (auto it = _records.begin(); it != _records.end();)
    {
        bool const requester = it->requesterKey == participant && it->requesterEpoch == epoch;
        bool const recipient = it->recipientKey == participant && it->recipientEpoch == epoch;
        if (requester || recipient)
        {
            result.push_back(*it);
            it = _records.erase(it);
        }
        else
            ++it;
    }
    return result;
}

std::vector<WojHelpRecord> WojHelp::CancelTarget(WojActorKey const& target)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<WojHelpRecord> result;
    for (auto it = _records.begin(); it != _records.end();)
    {
        if (it->targetKey == target)
        {
            result.push_back(*it);
            it = _records.erase(it);
        }
        else
            ++it;
    }
    return result;
}

std::vector<WojHelpRecord> WojHelp::Clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<WojHelpRecord> result = std::move(_records);
    _records.clear();
    _seenOrder.clear();
    _seenDecisions.clear();
    return result;
}

void WojWriteHelpExpired(WojHelpRecord const& record, char const* reason)
{
    // The recipient may already have asked Jev to answer this exact request.
    // Once its requester/target/TTL disappears, that snapshot is stale even
    // if the HTTP answer is still in flight. Fence it before the world can
    // consume an ANSWER_HELP that no longer has a physical request to answer.
    // Include the exact request id in the terminal reason for causal audit.
    if (WojRegistry::Instance().CurrentEpoch(record.recipientKey) == record.recipientEpoch)
    {
        std::string const terminalReason = "help_request_expired:" + record.id;
        WojRegistry::Instance().CancelPending(record.recipientKey, terminalReason.c_str());
    }
    Map* map = sMapMgr->FindMap(record.requesterKey.mapId, record.requesterKey.instanceId);
    Creature* requester = map ? map->GetCreature(ObjectGuid(record.requesterKey.rawGuid)) : nullptr;
    Creature* recipient = map ? map->GetCreature(ObjectGuid(record.recipientKey.rawGuid)) : nullptr;
    Unit* context = requester ? static_cast<Unit*>(requester) : static_cast<Unit*>(recipient);
    Unit* target = context ? ObjectAccessor::GetUnit(*context, ObjectGuid(record.targetKey.rawGuid)) : nullptr;
    WojCombatLog::Instance().WriteHelpExpired(requester, recipient, target,
        record.requesterEpoch, record.recipientEpoch, record.id, record.decisionId, reason);
}

void WojWriteHelpExpired(std::vector<WojHelpRecord> const& records, char const* reason)
{
    for (WojHelpRecord const& record : records)
        WojWriteHelpExpired(record, reason);
}
