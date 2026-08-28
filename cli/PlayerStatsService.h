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

namespace cli {

// Sequential automatic lobby lookup pipeline. It is intentionally separate
// from HypixelService's user-facing request so automatic roster traffic can
// never overwrite a manual result or its cancellation state.
class PlayerStatsService
{
public:
    explicit PlayerStatsService(WinApiKeyStore *apiKeys) : m_apiKeys(apiKeys) {}
    ~PlayerStatsService() { shutdown(); }

    PlayerStatsService(const PlayerStatsService &) = delete;
    PlayerStatsService &operator=(const PlayerStatsService &) = delete;

    void start();
    void shutdown();

    // Main-thread API.
    void enqueuePlayer(const std::wstring &playerName, const std::wstring &teamPrefix);
    void setMatchActive(bool active);
    void reloadConfiguration();
    void cancel();

    HANDLE event() const noexcept { return m_queue.event(); }
    void processMessages();

    // Main-thread notifications.
    std::function<void(const std::wstring &, const std::wstring &, int, double, int)> onStatsReady;
    std::function<void(const std::wstring &, const std::wstring &)> onStatsFailed;

private:
    struct Job
    {
        std::wstring playerName;
        std::wstring teamPrefix;
        std::string apiKey; // snapshot taken on the main thread
        uint64_t generation = 0;
    };

    struct CachedStats
    {
        int stars = 0;
        double fkdr = 0.0;
        int level = 0;
        unsigned long long expiresAtTick = 0;
    };

    enum class MessageKind { Ready, Failed };

    struct Message
    {
        MessageKind kind = MessageKind::Failed;
        std::wstring playerName;
        std::wstring teamPrefix;
        std::wstring reason;
        int stars = 0;
        double fkdr = 0.0;
        int level = 0;
    };

    void workerLoop();

    [[nodiscard]] static bool validName(const std::wstring &name);
    [[nodiscard]] static bool validTeamPrefix(const std::wstring &prefix);

    WinApiKeyStore *m_apiKeys = nullptr;
    std::thread m_worker;
    WaitQueue<Job> m_jobs;
    WaitQueue<Message> m_queue;
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<HANDLE> m_activeRequest{nullptr};
    WinHttpClient m_http; // worker-owned

    // Main-thread state.
    bool m_matchActive = false;
    std::map<std::wstring, unsigned long long> m_deduplicateUntil;
    std::map<std::wstring, Job> m_discoveredPlayers;
    std::map<std::wstring, CachedStats> m_statsCache;
};

} // namespace cli
