#ifndef WOJ_BRIDGE_H
#define WOJ_BRIDGE_H
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// Forward-declared, not included: httplib.h is vendored and lives in exactly
// one translation unit, WojBridge.cpp (see mod-world-of-jevs.cmake). Naming
// the type here is enough to declare Run() below; only the .cpp ever needs
// the full definition, to construct one, move it into the thread, and use it.
namespace httplib { class Client; }
struct WojSnapshot;

// Сетевые workers видят только plain snapshots/actions и никогда не получают
// игровых объектов. У каждого worker собственный HTTP client.
class WojBridge
{
public:
    static WojBridge& Instance();
    // Reads WojConfig and builds the httplib::Client on the GAME thread,
    // then hands the worker the finished client by move. The worker must
    // never see WojConfig: OnAfterConfigLoad rewrites that singleton on
    // every `.reload config`, including reassigning the std::string
    // GatewayUrl. A worker holding a reference into it would be racing a
    // GM's console command inside the worldserver process. Building the
    // client here (N1) also means a bad GatewayUrl is refused with a log
    // line before any thread exists, instead of being discovered only
    // through a permanently silent worker.
    void Start();
    void Stop();
    // Timeout можно менять через `.reload config`: workers считывают
    // атомарную копию только между HTTP-запросами и настраивают каждый свой
    // client, не останавливая мир и не создавая второй запрос тому же NPC.
    void UpdateTimeout(uint32_t timeoutMs);
    // Один идентификатор связывает decisions.jsonl и WojCombat.log.
    static std::string const& ServerBootId();
    // Same wire serializer used by /v1/decide, without dispatching HTTP.
    static std::string SerializeRequest(WojSnapshot const& snapshot,
                                        std::string const& requestId,
                                        uint32_t captureToEnqueueMs = 0,
                                        uint32_t ageAtSendMs = 0);

private:
    // Takes ownership of a client Start() already validated and configured.
    // Run не читает WojConfig: между запросами он применяет только атомарную
    // копию timeout, опубликованную game thread.
    void Run(httplib::Client client);

    static constexpr size_t WORKER_COUNT = 4;
    std::array<std::thread, WORKER_COUNT> _threads;
    std::atomic<bool> _running{false};
    std::atomic<uint32_t> _timeoutMs{3000};
};
#endif
