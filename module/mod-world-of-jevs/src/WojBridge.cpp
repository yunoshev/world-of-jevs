#include "WojBridge.h"
#include "WojConfig.h"
#include "WojRegistry.h"
#include "Log.h"

// No CPPHTTPLIB_OPENSSL_SUPPORT define here, and do not add one "set to 0":
// httplib guards on #ifdef, not on the value, so defining it to 0 turns TLS
// ON and drags <openssl/ssl.h> plus its version checks into this translation
// unit. We speak plain HTTP to a loopback service on the same machine.
//
// SIGPIPE: cpp-httplib does not ignore it itself. This process currently
// runs with it ignored (docker exec ac-worldserver grep SigIgn
// /proc/1/status: bit 12 set), almost certainly set by the MySQL client
// library, a dependency this module does not control. If that dependency
// ever changes, a reset connection during Post() below would raise SIGPIPE
// with the default disposition (process termination) instead of just
// failing the call.
//
// N2 (2): without this, httplib.h:422 only ever *checks*
// CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO, never defines it, so
// getaddrinfo_with_timeout (httplib.h:6671) falls straight through to a
// plain, unbounded getaddrinfo(). Verified against the vendored copy by
// building ac-worldserver with this defined: it compiles and links clean
// on this image (Ubuntu 24.04/glibc), which takes the getaddrinfo_a()/
// gai_suspend() branch (httplib.h:6900), not the generic thread-per-call
// fallback.
//
// I1: this define does NOT bound DNS resolution the way the comment here
// used to claim, at any WorldOfJevs.TimeoutMs. Two separate reasons, found
// by tracing the call chain rather than assumed:
//
// 1. The timeout passed in is connection_timeout_sec_, in whole seconds
//    only (httplib.h's "// Pass DNS timeout" comment on the
//    create_client_socket call), and getaddrinfo_with_timeout treats a
//    timeout_sec <= 0 as "no timeout given" and takes the plain,
//    unbounded getaddrinfo() path regardless of this define
//    (httplib.h:6674-6678). Below TimeoutMs = 1000 the seconds part
//    truncates to 0 (see ConfigureClientTimeouts below), so under 1000 this
//    define buys nothing at all.
// 2. At TimeoutMs >= 1000 it still only bounds the *wait* for the
//    resolver, not the *cancellation* of a wait that timed out: the
//    glibc branch this build takes cancels with gai_cancel() and then
//    loops on gai_suspend(..., nullptr) - no timeout argument -
//    (httplib.h:6936-6940), and httplib's own comment on that branch
//    prices the trade-off directly: "a wedged DNS server can hold this
//    thread for the system resolver timeout (~30s by default)"
//    (httplib.h:6902-6907). That wait is on the far side of every
//    timeout this module configures.
//
// So: name resolution is not bounded by anything this module controls, at
// any TimeoutMs. What this define buys is real but narrower than advertised
// before this comment was corrected - see Stop()'s comment for the actual
// consequence and how rarely it is reachable. Left defined anyway: it is
// free correctness (bounds the common, fast-resolving case's wait instead
// of leaving even that undefined), and this comment exists so the next
// person does not repeat the "TimeoutMs >= 1000 means the world is safe"
// conclusion that was already written into conf/mod_world_of_jevs.conf.dist
// once, from an earlier version of this same comment.
#define CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO
#include "httplib.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

using json = nlohmann::json;

namespace
{
    // The ceiling set_max_timeout is given below, as a multiple of the
    // configured per-wait timeout. See the comment at the call site (C3):
    // this bounds the whole request, not one wait inside it.
    constexpr uint32_t REQUEST_TIMEOUT_MULTIPLIER = 2;

    void ConfigureClientTimeouts(httplib::Client& client, uint32_t timeoutMs)
    {
        time_t const timeoutSec = static_cast<time_t>(timeoutMs / 1000);
        time_t const timeoutUsec = static_cast<time_t>((timeoutMs % 1000) * 1000);
        client.set_connection_timeout(timeoutSec, timeoutUsec);
        client.set_read_timeout(timeoutSec, timeoutUsec);
        client.set_write_timeout(timeoutSec, timeoutUsec);
        client.set_max_timeout(static_cast<time_t>(timeoutMs) * REQUEST_TIMEOUT_MULTIPLIER);
    }

    uint32_t Milliseconds(std::chrono::steady_clock::time_point end,
                          std::chrono::steady_clock::time_point begin)
    {
        int64_t const value = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
        return static_cast<uint32_t>(std::min<int64_t>(
            std::max<int64_t>(value, 0), std::numeric_limits<uint32_t>::max()));
    }

    char const* NormalizeTransportError(httplib::Error error)
    {
        switch (error)
        {
            case httplib::Error::Connection: return "connection";
            case httplib::Error::ConnectionTimeout: return "connection_timeout";
            case httplib::Error::Read: return "read";
            case httplib::Error::Write: return "write";
            case httplib::Error::Timeout: return "timeout";
            case httplib::Error::ConnectionClosed: return "connection_closed";
            case httplib::Error::Canceled: return "canceled";
            default: return "other";
        }
    }

    WojTransportEvent TransportEvent(WojSnapshot const& snap, std::string const& requestId,
                                     uint32_t captureToEnqueueMs, uint32_t ageAtSendMs,
                                     uint32_t httpTimeoutMs,
                                     std::chrono::steady_clock::time_point sentAt)
    {
        WojTransportEvent event;
        event.actorId = snap.id;
        event.actorGuidSpawn = snap.guid;
        event.actorMapId = snap.actor.mapId;
        event.actorInstanceId = snap.actor.instanceId;
        event.actorRawGuid = snap.actor.rawGuid;
        event.actorEntry = snap.entry;
        event.actorName = snap.name;
        event.arenaId = snap.arenaId;
        event.requestId = requestId;
        event.epoch = snap.epoch;
        event.snapshotSeq = snap.seq;
        event.policyRevision = snap.behavior.policyRevision;
        event.bindingGeneration = snap.behavior.bindingGeneration;
        event.captureToEnqueueMs = captureToEnqueueMs;
        event.ageAtSendMs = ageAtSendMs;
        event.freshnessLimitMs = snap.freshnessLimitMs;
        event.httpTimeoutMs = httpTimeoutMs;
        event.capturedAt = snap.capturedAt;
        event.sentAt = sentAt;
        return event;
    }

    std::string RandomHex(size_t bytes)
    {
        // A single 32-bit random_device call seeds only 32 bits of
        // mt19937_64's 19937 bits of state. Gather several words through
        // seed_seq instead so the generator actually starts from a large
        // random state.
        static thread_local std::mt19937_64 rng = []
        {
            std::random_device rd;
            std::array<uint32_t, 8> seedData{};
            for (uint32_t& word : seedData)
                word = rd();
            std::seed_seq seq(seedData.begin(), seedData.end());
            return std::mt19937_64(seq);
        }();

        std::ostringstream out;
        for (size_t i = 0; i < bytes; ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << ((rng() >> 8) & 0xFF);
        return out.str();
    }

    std::string const& BootId()
    {
        static std::string const id = RandomHex(8);
        return id;
    }

    json UnitToJson(WojUnit const& unit)
    {
        return {{"id", unit.id}, {"entry", unit.entry}, {"name", unit.name}, {"level", unit.level},
                {"is_player", unit.isPlayer}, {"health_pct", unit.healthPct}, {"distance", unit.distance},
                {"engaged", unit.engaged}, {"attacking_ally", unit.attackingAlly}};
    }

    json AllyToJson(WojAlly const& ally)
    {
        return {{"id", ally.id}, {"guid_spawn", ally.guidSpawn}, {"entry", ally.entry}, {"name", ally.name},
                {"health_pct", ally.healthPct}, {"distance", ally.distance}, {"in_combat", ally.inCombat}};
    }

    char const* RoutineProfileName(WojRoutineProfile profile)
    {
        switch (profile)
        {
            case WojRoutineProfile::Random: return "random";
            case WojRoutineProfile::Waypoint: return "waypoint";
            default: return "idle";
        }
    }

    json SnapshotToJson(WojSnapshot const& s, std::string const& requestId,
                        uint32_t captureToEnqueueMs, uint32_t ageAtSendMs)
    {
        json state = {
            {"position", {{"x", s.position.x}, {"y", s.position.y}, {"z", s.position.z}, {"o", s.position.o}}},
            {"health", {{"current", s.health}, {"max", s.healthMax}}},
            {"mana", {{"current", s.mana}, {"max", s.manaMax}}},
            {"in_combat", s.inCombat},
            {"fleeing", s.fleeing},
            {"home", {{"x", s.home.x}, {"y", s.home.y}, {"z", s.home.z}, {"o", s.home.o}}},
            {"last_actions", json(s.lastActions)},
            {"seconds_since_last_decision", s.secondsSinceLastDecision},
            {"seconds_since_last_action", s.secondsSinceLastAction},
        };
        state["victim"] = s.hasVictim ? json(UnitToJson(s.victim)) : json(nullptr);
        state["nearest_enemy"] = s.hasNearestEnemy ? json(UnitToJson(s.nearestEnemy)) : json(nullptr);
        state["attackers"] = json::array();
        for (WojUnit const& attacker : s.attackers)
            state["attackers"].push_back(UnitToJson(attacker));
        state["enemies"] = json::array();
        for (WojUnit const& enemy : s.enemies)
            state["enemies"].push_back(UnitToJson(enemy));
        state["attack_targets"] = s.attackTargets;
        state["allies"] = json::array();
        for (WojAlly const& ally : s.allies)
            state["allies"].push_back(AllyToJson(ally));
        state["heal_allies"] = json::array();
        for (WojAlly const& ally : s.healAllies)
            state["heal_allies"].push_back(AllyToJson(ally));
        state["call_help_targets"] = s.callHelpTargets;
        state["flee_assist_targets"] = s.fleeAssistTargets;
        state["flee_targets"] = s.fleeTargets;
        state["spells"] = json::array();
        for (WojSpell const& spell : s.spells)
        {
            json value = {{"id", spell.id}, {"ready", spell.ready}, {"cooldown_ms", spell.cooldownMs},
                          {"valid_targets", spell.validTargets}, {"aura_targets", spell.auraTargets},
                          {"last_cast_age_ms", nullptr}};
            if (spell.hasLastCastAge)
                value["last_cast_age_ms"] = spell.lastCastAgeMs;
            state["spells"].push_back(std::move(value));
        }
        state["help_requests"] = json::array();
        for (WojHelpRequest const& request : s.helpRequests)
            state["help_requests"].push_back({{"request_id", request.requestId},
                {"requester", AllyToJson(request.requester)}, {"requester_epoch", request.requesterEpoch},
                {"target", UnitToJson(request.target)}, {"expires_in_ms", request.expiresInMs}});
        state["routine"] = {
            {"profile", RoutineProfileName(s.routine.profile)},
            {"wander_radius", s.routine.wanderRadius},
            {"path_id", s.routine.pathId ? json(s.routine.pathId) : json(nullptr)},
            {"point_count", s.routine.pointCount},
            {"active", s.routine.active},
            {"can_resume", s.routine.canResume}
        };
        state["behavior"] = {
            {"policy_revision", s.behavior.policyRevision},
            {"seq", s.behavior.seq},
            {"floor_seq", s.behavior.floorSeq},
            {"triggers", json::array()},
            {"events", json::array()}
        };
        if (!s.behavior.groupId.empty())
        {
            state["behavior"]["group_id"] = s.behavior.groupId;
            state["behavior"]["binding_generation"] = s.behavior.bindingGeneration;
        }
        else
            state["behavior"]["scene_id"] = s.behavior.sceneId;
        for (WojBehaviorTrigger const& trigger : s.behavior.triggers)
            state["behavior"]["triggers"].push_back({
                {"id", trigger.id}, {"generation", trigger.generation},
                {"status", trigger.status},
                {"wait_ms", trigger.hasWaitMs ? json(trigger.waitMs) : json(nullptr)},
                {"blocked_by", trigger.blockedBy.empty() ? json(nullptr) : json(trigger.blockedBy)}
            });
        for (WojBehaviorEvent const& event : s.behavior.events)
            state["behavior"]["events"].push_back({
                {"seq", event.seq},
                {"trigger_id", event.triggerId.empty() ? json(nullptr) : json(event.triggerId)},
                {"generation", event.generation}, {"kind", event.kind},
                {"decision_id", event.decisionId.empty() ? json(nullptr) : json(event.decisionId)},
                {"reason", event.reason.empty() ? json(nullptr) : json(event.reason)}
            });

        return json{
            {"schema_version", s.behavior.groupId.empty() ? "1.7" : "1.8"},
            {"request_id", requestId},
            {"server_boot_id", BootId()},
            {"npc", {{"id", s.id}, {"guid", s.guid}, {"entry", s.entry}, {"name", s.name}, {"epoch", s.epoch},
                     {"map_id", s.actor.mapId}, {"instance_id", s.actor.instanceId},
                     {"raw_guid", std::to_string(s.actor.rawGuid)}}},
            {"snapshot_seq", s.seq},
            {"transport", {{"capture_to_enqueue_ms", captureToEnqueueMs},
                           {"age_at_send_ms", ageAtSendMs},
                           {"freshness_limit_ms", s.freshnessLimitMs}}},
            {"state", state},
        };
    }
}

WojBridge& WojBridge::Instance()
{
    static WojBridge instance;
    return instance;
}

std::string const& WojBridge::ServerBootId()
{
    return BootId();
}

std::string WojBridge::SerializeRequest(WojSnapshot const& snapshot,
                                        std::string const& requestId,
                                        uint32_t captureToEnqueueMs,
                                        uint32_t ageAtSendMs)
{
    return SnapshotToJson(snapshot, requestId, captureToEnqueueMs, ageAtSendMs)
        .dump(-1, ' ', false, json::error_handler_t::replace);
}

void WojBridge::Start()
{
    if (_running.load())
        return;
    // После исключения в одном worker остальные видят running=false. Перед
    // любой попыткой нового Start все прежние thread-объекты собраны, даже
    // если новая конфигурация окажется недопустимой.
    for (std::thread& worker : _threads)
        if (worker.joinable())
            worker.join();
    std::string const gatewayUrl = WojConfig::Instance().GatewayUrl;
    uint32_t const timeoutMs = WojConfig::Instance().TimeoutMs;
    UpdateTimeout(timeoutMs);

    if (gatewayUrl.rfind("http://", 0) != 0)
    {
        LOG_ERROR("module",
                  "mod-world-of-jevs: WorldOfJevs.GatewayUrl '{}' does not start with http:// "
                  "(this build has no TLS support) - bridge NOT started", gatewayUrl);
        _running.store(false);
        return;
    }

    try
    {
        // Старый пул уже полностью собран; только теперь новые workers могут
        // увидеть running=true. Иначе Start воскресил бы уцелевший старый
        // worker прямо перед join и завис навсегда.
        _running.store(true);
        for (std::thread& worker : _threads)
        {
            // Client не разделяется между потоками: соединение и его
            // внутреннее состояние принадлежат ровно одному worker.
            httplib::Client client(gatewayUrl.c_str());
            if (!client.is_valid())
                throw std::invalid_argument("bad host or port");
            ConfigureClientTimeouts(client, timeoutMs);
            // cpp-httplib не повторяет обычный HTTP POST после сетевой
            // ошибки. Keep-alive только переиспользует живой socket или
            // открывает новый до отправки, сокращая Docker->host churn без
            // риска второго оплачиваемого решения с тем же request_id.
            client.set_keep_alive(true);
            worker = std::thread(&WojBridge::Run, this, std::move(client));
        }
    }
    catch (std::exception const& error)
    {
        _running.store(false);
        for (std::thread& worker : _threads)
            if (worker.joinable())
                worker.join();
        LOG_ERROR("module", "mod-world-of-jevs: bridge workers NOT started for gateway {}: {}",
                  gatewayUrl, error.what());
        return;
    }
    catch (...)
    {
        _running.store(false);
        for (std::thread& worker : _threads)
            if (worker.joinable())
                worker.join();
        LOG_ERROR("module", "mod-world-of-jevs: bridge workers NOT started for gateway {}",
                  gatewayUrl);
        return;
    }

    LOG_INFO("module", "mod-world-of-jevs: {} bridge workers started, gateway {}, server_boot_id={}",
             WORKER_COUNT, gatewayUrl, BootId());
}

void WojBridge::UpdateTimeout(uint32_t timeoutMs)
{
    _timeoutMs.store(timeoutMs, std::memory_order_release);
}

void WojBridge::Stop()
{
    bool const wasRunning = _running.exchange(false);
    // Ожидание Post ограничено HTTP timeout, но отмена зависшего системного
    // DNS resolver этим пределом не покрывается и может задержать join.
    for (std::thread& worker : _threads)
        if (worker.joinable())
            worker.join();

    if (!wasRunning)
        return;
    LOG_INFO("module", "mod-world-of-jevs: bridge workers stopped");
}

void WojBridge::Run(httplib::Client client)
{
    // The client arrives already validated and fully configured (is_valid(),
    // all four timeouts) - see Start(). This thread must not read WojConfig
    // at all, and must never call LOG_*: Log::write reads sLog's
    // _ioContext/_strand without synchronization, and the game thread tears
    // both down on shutdown (SetSynchronous(), two lines before
    // OnShutdown() stops this bridge) and rebuilds them on ".reload config"
    // (Log::Close()). Every reason this function has for not producing a
    // usable action instead travels to the game thread inside
    // action.thought, which WojAI::ApplyAction logs from its None branch -
    // the one place logging it is safe.
    //
    // The whole body is also wrapped in try/catch(...) (C1): no exception
    // may ever unwind out of a std::thread entry function, because that
    // calls std::terminate() and takes the whole process down with it.
    try
    {
        uint32_t appliedTimeoutMs = 0;
        while (_running.load())
        {
            WojSnapshot snap;
            WojAction action;
            bool dispatched = false;
            uint32_t captureToEnqueueMs = 0;
            uint32_t ageAtSendMs = 0;
            std::string requestId;
            std::chrono::steady_clock::time_point httpStartedAt{};
            std::chrono::steady_clock::time_point receivedAt{};
            WojTransportEvent transportEvent;
            bool transportStarted = false;

            try
            {
                uint32_t const timeoutMs = _timeoutMs.load(std::memory_order_acquire);
                if (timeoutMs != appliedTimeoutMs)
                {
                    ConfigureClientTimeouts(client, timeoutMs);
                    appliedTimeoutMs = timeoutMs;
                }
                if (!WojRegistry::Instance().TakePendingSnapshot(snap))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                dispatched = true;

                auto const requestStartedAt = std::chrono::steady_clock::now();
                captureToEnqueueMs = Milliseconds(snap.enqueuedAt, snap.capturedAt);
                ageAtSendMs = Milliseconds(requestStartedAt, snap.capturedAt);
                if (ageAtSendMs > snap.freshnessLimitMs)
                {
                    WojRegistry::Instance().DropDispatch(snap.actor, snap.epoch, snap.seq);
                    continue;
                }
                requestId = RandomHex(8);
                httpStartedAt = requestStartedAt;

                {
                    // I1: error_handler_t::replace instead of dump()'s
                    // default "strict" handler. This makes the serializer's
                    // own encoding guarantee explicit at the one place a
                    // future snapshot field could reopen the question,
                    // instead of relying on the next person to re-derive
                    // that UTF-8 was already safe here. json::parse below,
                    // on the request_id we get back, is the matching half
                    // of that guarantee on the read side.
                    std::string const body = SnapshotToJson(snap, requestId,
                        captureToEnqueueMs, ageAtSendMs)
                        .dump(-1, ' ', false, json::error_handler_t::replace);

                    // Anchor остаётся перед сериализацией: тогда три стадии
                    // timing складываются в total_age без скрытого зазора.
                    transportEvent = TransportEvent(snap, requestId,
                        captureToEnqueueMs, ageAtSendMs, appliedTimeoutMs, httpStartedAt);
                    if (!WojRegistry::Instance().TryPushDispatch(snap, transportEvent))
                    {
                        WojRegistry::Instance().DropDispatch(snap.actor, snap.epoch, snap.seq);
                        continue;
                    }
                    transportStarted = true;
                    auto res = client.Post("/v1/decide", body, "application/json");
                    receivedAt = std::chrono::steady_clock::now();

                    if (!res)
                    {
                        action.thought = "gateway unreachable: " + httplib::to_string(res.error());
                        transportEvent.outcome = "transport_error";
                        transportEvent.transportError = NormalizeTransportError(res.error());
                    }
                    else if (res->status != 200)
                    {
                        action.thought = "gateway bad status " + std::to_string(res->status);
                        transportEvent.outcome = "http_status";
                        transportEvent.hasHttpStatus = true;
                        transportEvent.httpStatus = static_cast<uint32_t>(std::max(res->status, 0));
                    }
                    else
                    {
                        transportEvent.outcome = "invalid_response";
                        transportEvent.hasHttpStatus = true;
                        transportEvent.httpStatus = 200;
                        json const parsed = json::parse(res->body);
                        if (parsed.at("request_id").get<std::string>() != requestId)
                        {
                            action.thought = "request_id mismatch";
                            transportEvent.outcome = "request_id_mismatch";
                        }
                        else
                        {
                            json const& a = parsed.at("action");
                            std::string const kind = a.at("kind").get<std::string>();
                            auto readTriggerGeneration = [&]() -> uint32_t
                            {
                                json const& value = a.at("trigger_generation");
                                if (value.is_number_unsigned())
                                {
                                    uint64_t const unsignedValue = value.get<uint64_t>();
                                    if (unsignedValue == 0 || unsignedValue > std::numeric_limits<uint32_t>::max())
                                        throw std::runtime_error("bad trigger_generation");
                                    return static_cast<uint32_t>(unsignedValue);
                                }
                                if (value.is_number_integer())
                                {
                                    int64_t const signedValue = value.get<int64_t>();
                                    if (signedValue <= 0 || static_cast<uint64_t>(signedValue) >
                                        std::numeric_limits<uint32_t>::max())
                                        throw std::runtime_error("bad trigger_generation");
                                    return static_cast<uint32_t>(signedValue);
                                }
                                throw std::runtime_error("bad trigger_generation");
                            };
                            action.decisionId = parsed.value("decision_id", "");
                            action.thought = parsed.value("thought", "");
                            action.marker = parsed.value("marker", "");
                            // Тот же приём, что и у marker выше: value() с
                            // дефолтом, а не at() со строгой проверкой. Ярус —
                            // это диагностика для лога и отчёта, а не то, от
                            // чего зависит применимость действия; неизвестное
                            // или отсутствующее значение (будущий ярус Laya,
                            // старый гейтвей) должно дойти до WojAI пустой
                            // строкой, а не уронить всё решение исключением.
                            action.tier = parsed.value("tier", "");
                            action.validForSeconds = parsed.value("valid_for_seconds", 0u);

                            if (kind == "MOVE_TO")
                            {
                                action.kind = WojActionKind::MoveTo;
                                action.point.x = a.at("x").get<float>();
                                action.point.y = a.at("y").get<float>();
                                action.point.z = a.at("z").get<float>();
                                if (!std::isfinite(action.point.x) || !std::isfinite(action.point.y) || !std::isfinite(action.point.z))
                                    throw std::runtime_error("non-finite coordinate");
                            }
                            else if (kind == "SAY")
                            {
                                action.kind = WojActionKind::Say;
                                action.text = a.at("text").get<std::string>();
                                if (action.text.empty() || action.text.size() > 255)
                                    throw std::runtime_error("bad say text");
                            }
                            else if (kind == "STOCK_TALK")
                            {
                                action.kind = WojActionKind::StockTalk;
                                if (a.at("text_group").get<uint32_t>() > 255)
                                    throw std::runtime_error("bad stock talk group");
                                action.triggerId = a.value("trigger_id", "aggro_talk");
                                if (action.triggerId != "aggro_talk" && action.triggerId != "ambient_talk")
                                    throw std::runtime_error("bad stock talk trigger");
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "IDLE")
                            {
                                action.kind = WojActionKind::Idle;
                            }
                            else if (kind == "ATTACK")
                            {
                                action.kind = WojActionKind::Attack;
                                action.target = a.at("target").get<std::string>();
                            }
                            else if (kind == "CAST")
                            {
                                action.kind = WojActionKind::Cast;
                                action.spell = a.at("spell").get<uint32_t>();
                                action.target = a.at("target").get<std::string>();
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "START_PHASE")
                            {
                                action.kind = WojActionKind::StartPhase;
                                action.phaseId = a.at("phase_id").get<std::string>();
                                if (action.phaseId.empty() || action.phaseId.size() > 32)
                                    throw std::runtime_error("bad health phase id");
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "HEALTH_EVENT")
                            {
                                action.kind = WojActionKind::HealthEvent;
                                action.healthEventId = a.at("event_id").get<std::string>();
                                if (action.healthEventId.empty() || action.healthEventId.size() > 32)
                                    throw std::runtime_error("bad health event id");
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "AGGRO_EVENT")
                            {
                                action.kind = WojActionKind::AggroEvent;
                                action.aggroEventId = a.at("event_id").get<std::string>();
                                if (action.aggroEventId.empty() || action.aggroEventId.size() > 32)
                                    throw std::runtime_error("bad aggro event id");
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "FLEE_FOR_ASSIST")
                            {
                                action.kind = WojActionKind::FleeForAssist;
                                action.recipient = a.at("recipient").get<std::string>();
                                action.target = a.at("target").get<std::string>();
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "CALL_HELP")
                            {
                                action.kind = WojActionKind::CallHelp;
                                action.recipient = a.at("recipient").get<std::string>();
                                action.target = a.at("target").get<std::string>();
                            }
                            else if (kind == "FLEE")
                            {
                                action.kind = WojActionKind::Flee;
                                action.target = a.at("target").get<std::string>();
                                action.triggerGeneration = readTriggerGeneration();
                            }
                            else if (kind == "ANSWER_HELP")
                            {
                                action.kind = WojActionKind::AnswerHelp;
                                action.helpRequestId = a.at("help_request_id").get<std::string>();
                            }
                            else if (kind == "STOP_ATTACK")
                            {
                                action.kind = WojActionKind::StopAttack;
                            }
                            else if (kind == "EVADE")
                            {
                                action.kind = WojActionKind::Evade;
                            }
                            else if (kind == "RESUME_ROUTINE")
                            {
                                action.kind = WojActionKind::ResumeRoutine;
                            }
                            else
                            {
                                throw std::runtime_error("unknown action kind: " + kind);
                            }
                            transportEvent.outcome = "ok";
                        }
                    }
                }
            }
            catch (std::exception const& e)
            {
                // Malformed JSON, an unexpected shape, a non-finite
                // coordinate, a bad SAY string, or an unrecognised kind all
                // land here. action may already hold partial fields from
                // before the throw; reset it so a half-built action can
                // never reach ApplyAction.
                action = WojAction{};
                action.thought = std::string("bad response: ") + e.what();
                if (receivedAt == std::chrono::steady_clock::time_point{})
                    receivedAt = std::chrono::steady_clock::now();
            }
            catch (...)
            {
                // Insurance (C1): nothing above should be able to throw
                // something that is not a std::exception, but this loop
                // iteration ends in a None decision either way, never in an
                // escaping exception.
                action = WojAction{};
                action.thought = "unexpected error while handling gateway response";
                if (receivedAt == std::chrono::steady_clock::time_point{})
                    receivedAt = std::chrono::steady_clock::now();
            }

            if (transportStarted)
            {
                transportEvent.kind = WojTransportEventKind::Result;
                transportEvent.receivedAt = receivedAt == std::chrono::steady_clock::time_point{}
                    ? std::chrono::steady_clock::now() : receivedAt;
                if (transportEvent.outcome.empty())
                    transportEvent.outcome = "internal_error";
                WojRegistry::Instance().PushTransportEvent(transportEvent);
            }

            if (!dispatched)
                continue;
            if (requestId.empty())
            {
                WojRegistry::Instance().DropDispatch(snap.actor, snap.epoch, snap.seq);
                continue;
            }

            // Метаданные принадлежат текущей попытке, а не разобранному
            // ответу. Поэтому они восстанавливаются и после любого catch.
            action.requestId = requestId;
            action.snapshotEpoch = snap.epoch;
            action.snapshotSeq = snap.seq;
            action.policyRevision = snap.behavior.policyRevision;
            action.bindingGeneration = snap.behavior.bindingGeneration;
            action.freshnessLimitMs = snap.freshnessLimitMs;
            action.captureToEnqueueMs = captureToEnqueueMs;
            action.ageAtSendMs = ageAtSendMs;
            action.capturedAt = snap.capturedAt;
            action.httpStartedAt = httpStartedAt;
            action.receivedAt = receivedAt;

            // Every path above must reach this call, including both catch
            // blocks: see the invariant on WojRegistry::SubmitDecision and
            // WojTypes.h. Skip it and this NPC goes silent for up to
            // IN_FLIGHT_DEADLINE_SECONDS before the registry's own watchdog
            // notices - and that watchdog lives inside TakePendingSnapshot,
            // which only this (possibly now-dead) worker ever calls, so it
            // cannot pay out on its own.
            //
            // N3: this call gets its own try, nested inside the outer one
            // below. By the time execution reaches here TakePendingSnapshot
            // has already flipped this guid's record to inFlight - true
            // regardless of which of the three paths above produced
            // `action` - so if SubmitDecision itself were to throw (it
            // should not; it is just mutex-protected map bookkeeping) and
            // that were left to reach the outer catch, this NPC's record
            // would stay inFlight forever with no worker left alive to ever
            // release it. Catching it right here instead keeps the loop -
            // and every other NPC's traffic - alive for the next iteration.
            try
            {
                std::string const terminalReason = WojRegistry::Instance().SubmitDecision(
                    snap.actor, snap.epoch, snap.seq, action);
                if (!terminalReason.empty())
                {
                    WojTransportEvent terminalEvent = transportEvent;
                    terminalEvent.kind = WojTransportEventKind::Terminal;
                    terminalEvent.decisionId = action.decisionId;
                    terminalEvent.terminalReason = terminalReason;
                    WojRegistry::Instance().PushTransportEvent(terminalEvent);
                }
            }
            catch (...)
            {
                // Nothing more to do for this one decision, but the loop
                // survives to try the next snapshot.
            }
        }
    }
    catch (...)
    {
        // C1's insurance policy paying out. With SubmitDecision now guarded
        // by its own try above, the only things left that can land here are
        // TakePendingSnapshot itself throwing, or something escaping a
        // catch handler's own body - in either case this thread is ending
        // for good. _running must not be left true: it is the only signal
        // Start() checks (`_running.exchange(true)`), so leaving it true
        // would make a future Start() from ".reload config" treat this
        // already-dead bridge as already running and silently do nothing -
        // only a full worldserver restart would have brought it back.
        _running.store(false);
    }
}
