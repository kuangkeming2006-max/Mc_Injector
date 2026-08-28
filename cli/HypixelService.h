#pragma once

#include "ThreadQueue.h"
#include "WinApiKeyStore.h"
#include "WinHttpClient.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>

namespace cli {

// Asynchronous, manually-triggered client for Hypixel's official /v2/player
// endpoint. A Minecraft player name is resolved to the UUID through Minecraft
// Services, then the Bed Wars aggregate fields are read from player.stats.
// The worker thread performs HTTPS; public state is updated on the main
// thread only.
class HypixelService
{
public:
    enum class State { Idle, Loading, Ready, Error };

    struct Result
    {
        std::wstring uuid;
        std::wstring displayName;
        long long wins = 0;
        long long losses = 0;
        long long finalKills = 0;
        long long finalDeaths = 0;
        long long bedsBroken = 0;
        long long bedsLost = 0;
        double winRate = 0.0;
        double fkdr = 0.0;
        int rateLimit = -1;
        int rateRemaining = -1;
        int rateResetSeconds = -1;
    };

    explicit HypixelService(WinApiKeyStore *apiKeys) : m_apiKeys(apiKeys) {}
    ~HypixelService() { shutdown(); }

    HypixelService(const HypixelService &) = delete;
    HypixelService &operator=(const HypixelService &) = delete;

    void start();
    void shutdown();

    // Main-thread API.
    void lookupPlayer(const std::wstring &playerId);
    void cancel();
    void reloadConfiguration();

    [[nodiscard]] State state() const noexcept { return m_state; }
    [[nodiscard]] bool busy() const noexcept { return m_state == State::Loading; }
    [[nodiscard]] const Result &result() const noexcept { return m_result; }
    [[nodiscard]] const std::wstring &statusMessage() const noexcept { return m_status; }
    [[nodiscard]] const std::wstring &errorMessage() const noexcept { return m_error; }

    HANDLE event() const noexcept { return m_queue.event(); }
    void processMessages();

    // Main-thread notifications.
    std::function<void()> onChanged;       // any public state change
    std::function<void()> onResultReady;   // new statistics snapshot

private:
    struct Job
    {
        std::wstring playerId;
        std::string apiKey; // snapshot taken on the main thread
        uint64_t generation = 0;
    };

    struct CacheEntry
    {
        Result result;
        unsigned long long expiresAtTick = 0;
        unsigned long long lastAccessedTick = 0;
    };

    enum class MessageKind { Result, Error, Status };

    struct Message
    {
        MessageKind kind = MessageKind::Status;
        Result result;
        std::wstring text;
    };

    void workerLoop();
    void applyStats(const Result &result, const std::wstring &status);
    void setError(const std::wstring &message);
    void setStatus(const std::wstring &message);

    [[nodiscard]] static std::wstring normalizeUuid(const std::wstring &uuid);
    [[nodiscard]] static bool isValidPlayerName(const std::wstring &playerId);
    [[nodiscard]] static bool validUuid(const std::wstring &uuid);
    static bool performIdentity(const std::wstring &name,
                                WinHttpClient &http,
                                std::atomic<HANDLE> &activeSlot,
                                const std::atomic<bool> &cancelFlag,
                                std::wstring &uuid,
                                std::wstring &error);
    static bool performHypixel(const std::string &apiKey,
                               const std::wstring &uuid,
                               WinHttpClient &http,
                               std::atomic<HANDLE> &activeSlot,
                               const std::atomic<bool> &cancelFlag,
                               Result &result,
                               std::wstring &error);

    WinApiKeyStore *m_apiKeys = nullptr;
    std::thread m_worker;
    WaitQueue<Job> m_jobs;
    WaitQueue<Message> m_queue;
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<HANDLE> m_activeRequest{nullptr};
    WinHttpClient m_http; // worker-owned

    // Public state — main thread only.
    State m_state = State::Idle;
    Result m_result;
    std::wstring m_status = L"Enter a Minecraft player name to query Bed Wars statistics";
    std::wstring m_error;
    unsigned long long m_lastRequestTick = 0;
    std::map<std::wstring, CacheEntry> m_cache;
};

} // namespace cli
