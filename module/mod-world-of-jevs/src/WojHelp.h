#ifndef WOJ_HELP_H
#define WOJ_HELP_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "WojTypes.h"

struct WojHelpRecord
{
    std::string id;
    std::string decisionId;
    uint64_t requester = 0;
    WojActorKey requesterKey;
    uint32_t requesterSpawn = 0;
    uint32_t requesterEpoch = 0;
    uint64_t recipient = 0;
    WojActorKey recipientKey;
    uint32_t recipientSpawn = 0;
    uint32_t recipientEpoch = 0;
    uint64_t target = 0;
    WojActorKey targetKey;
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point expiresAt;
};

// Координатор живёт только на игровом потоке и хранит только значения.
// Указатели мира сюда не попадают: каждый исполнитель заново разрешает GUID.
class WojHelp
{
public:
    static WojHelp& Instance();

    static constexpr uint32_t TTL_MS = 10000;
    static constexpr size_t MAX_INBOX = 3;
    static constexpr size_t MAX_RECORDS = 128;

    bool Create(WojHelpRecord& record);
    bool HasOutstandingRequest(WojActorKey const& requester, uint32_t requesterEpoch) const;
    bool Peek(std::string const& id, WojActorKey const& recipient, uint32_t recipientEpoch, WojHelpRecord& out) const;
    bool Consume(std::string const& id);
    std::vector<WojHelpRecord> Inbox(WojActorKey const& recipient, uint32_t recipientEpoch) const;
    std::vector<WojHelpRecord> Expire();
    std::vector<WojHelpRecord> CancelParticipant(WojActorKey const& participant, uint32_t epoch);
    std::vector<WojHelpRecord> CancelTarget(WojActorKey const& target);
    std::vector<WojHelpRecord> Clear();

private:
    WojHelp() = default;
    void RememberDecision(std::string const& decisionId);

    uint64_t _nextId = 1;
    mutable std::mutex _mutex;
    std::vector<WojHelpRecord> _records;
    std::deque<std::string> _seenOrder;
    std::unordered_set<std::string> _seenDecisions;
};

// Запись терминального события остаётся на игровом потоке. Если один из
// объектов уже выгружен, журнал честно получает null вместо старого указателя.
void WojWriteHelpExpired(WojHelpRecord const& record, char const* reason);
void WojWriteHelpExpired(std::vector<WojHelpRecord> const& records, char const* reason);

#endif
