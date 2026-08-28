#include "HypixelService.h"

#include "Json.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

namespace cli {
namespace {

constexpr uint64_t kCacheLifetimeMs = 6ULL * 60ULL * 60ULL * 1000ULL;
constexpr uint64_t kMinimumRequestIntervalMs = 1000ULL;
constexpr uint64_t kMaximumResponseBytes = 4ULL * 1024ULL * 1024ULL;
constexpr uint64_t kIdentityResponseBytes = 64ULL * 1024ULL;
constexpr std::size_t kMaximumCacheEntries = 256;

long long headerInteger(const std::map<std::wstring, std::wstring> &headers,
                        const std::wstring &name)
{
    const auto found = headers.find(name);
    if (found == headers.cend())
        return -1;
    wchar_t *end = nullptr;
    const long long value = wcstoll(found->second.c_str(), &end, 10);
    return (end != found->second.c_str() && *end == L'\0') ? value : -1;
}

std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    (void) ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                 static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::wstring urlEncodeName(const std::wstring &name)
{
    // Validated names are [A-Za-z0-9_] only, so this is a no-op; kept for
    // parity with the Qt build's toPercentEncoding path.
    return name;
}

} // namespace

std::wstring HypixelService::normalizeUuid(const std::wstring &uuid)
{
    std::wstring normalized;
    normalized.reserve(uuid.size());
    for (const wchar_t character : uuid) {
        if (iswspace(character) != 0)
            continue;
        if (character == L'-')
            continue;
        normalized += towlower(character);
    }
    return validUuid(normalized) ? normalized : std::wstring{};
}

bool HypixelService::isValidPlayerName(const std::wstring &playerId)
{
    if (playerId.empty() || playerId.size() > 16)
        return false;
    return std::all_of(playerId.cbegin(), playerId.cend(), [](const wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
            || (c >= L'0' && c <= L'9') || c == L'_';
    });
}

bool HypixelService::validUuid(const std::wstring &uuid)
{
    if (uuid.size() != 32)
        return false;
    return std::all_of(uuid.cbegin(), uuid.cend(), [](const wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
    });
}

void HypixelService::start()
{
    if (m_worker.joinable())
        return;
    m_worker = std::thread(&HypixelService::workerLoop, this);
}

void HypixelService::shutdown()
{
    if (!m_worker.joinable())
        return;
    m_shutdown.store(true);
    m_generation.fetch_add(1);
    (void) WinHttpClient::cancelRequest(m_activeRequest);
    if (m_worker.joinable()) {
        if (m_worker.get_id() != std::this_thread::get_id())
            m_worker.join();
        else
            m_worker.detach();
    }
}

void HypixelService::workerLoop()
{
    for (;;) {
        Job job;
        if (!m_jobs.pop(job)) {
            if (m_shutdown.load())
                return;
            ::WaitForSingleObject(m_jobs.event(), 250);
            continue;
        }
        if (m_shutdown.load())
            return;
        if (job.generation != m_generation.load())
            continue;

        m_cancel.store(false);
        std::wstring uuid = normalizeUuid(job.playerId);
        if (uuid.empty()) {
            // Resolve the player name through Minecraft Services first.
            std::wstring error;
            if (!performIdentity(job.playerId, m_http, m_activeRequest,
                                 m_cancel, uuid, error)) {
                if (m_shutdown.load())
                    return;
                m_queue.push(Message{MessageKind::Error, {}, std::move(error)});
                continue;
            }
        }

        Result result;
        std::wstring error;
        if (!performHypixel(job.apiKey, uuid, m_http, m_activeRequest, m_cancel,
                            result, error)) {
            if (m_shutdown.load())
                return;
            m_queue.push(Message{MessageKind::Error, {}, std::move(error)});
            continue;
        }
        if (m_shutdown.load())
            return;
        m_queue.push(Message{MessageKind::Result, std::move(result), {}});
    }
}

bool HypixelService::performIdentity(const std::wstring &name,
                                     WinHttpClient &http,
                                     std::atomic<HANDLE> &activeSlot,
                                     const std::atomic<bool> &cancelFlag,
                                     std::wstring &uuid,
                                     std::wstring &error)
{
    const std::wstring url =
        L"https://api.minecraftservices.com/minecraft/profile/lookup/name/"
        + urlEncodeName(name);
    HttpResponse response;
    if (!http.get(url,
                  {{L"Accept", L"application/json"},
                   {L"User-Agent", L"JavaOverlayStudio/1.0"}},
                  kIdentityResponseBytes, response, &activeSlot, &cancelFlag)) {
        if (response.canceled)
            return false;
        if (response.status == 404) {
            error = L"No Minecraft player exists with the name " + name;
        } else {
            error = L"Minecraft player lookup failed ("
                + std::to_wstring(response.status) + L"): " + response.error;
        }
        return false;
    }

    const std::string body(reinterpret_cast<const char *>(response.body.data()),
                           response.body.size());
    json::Value document;
    std::string parseError;
    if (!json::parse(body, document, parseError)) {
        error = L"Minecraft Services returned invalid JSON";
        return false;
    }
    uuid = document.find(L"id") ? document.find(L"id")->asString() : std::wstring{};
    for (auto &character : uuid)
        character = towlower(character);
    if (!validUuid(uuid)) {
        error = L"Minecraft Services returned an invalid player ID";
        return false;
    }
    return true;
}

bool HypixelService::performHypixel(const std::string &apiKey,
                                    const std::wstring &uuid,
                                    WinHttpClient &http,
                                    std::atomic<HANDLE> &activeSlot,
                                    const std::atomic<bool> &cancelFlag,
                                    Result &result,
                                    std::wstring &error)
{
    const std::wstring url = L"https://api.hypixel.net/v2/player?uuid=" + uuid;
    std::vector<std::pair<std::wstring, std::wstring>> headers = {
        {L"API-Key", utf8ToWide(apiKey)},
        {L"Accept", L"application/json"},
        {L"User-Agent", L"JavaOverlayStudio/1.0 (registered Hypixel application)"}};
    HttpResponse response;
    if (!http.get(url, headers, kMaximumResponseBytes, response, &activeSlot,
                  &cancelFlag)) {
        if (response.canceled)
            return false;
        result.rateLimit = static_cast<int>(headerInteger(
            response.headers, L"ratelimit-limit"));
        result.rateRemaining = static_cast<int>(headerInteger(
            response.headers, L"ratelimit-remaining"));
        result.rateResetSeconds = static_cast<int>(headerInteger(
            response.headers, L"ratelimit-reset"));
        if (response.status == 429) {
            error = L"Hypixel rate limit reached; retry after "
                + std::to_wstring(std::max(0, result.rateResetSeconds))
                + L" seconds";
        } else if (response.status == 403) {
            error = L"Hypixel rejected the registered API key";
        } else {
            error = L"Hypixel request failed ("
                + std::to_wstring(response.status) + L"): " + response.error;
        }
        return false;
    }

    const std::string body(reinterpret_cast<const char *>(response.body.data()),
                           response.body.size());
    json::Value document;
    std::string parseError;
    if (!json::parse(body, document, parseError)) {
        error = L"Hypixel returned invalid JSON";
        return false;
    }
    if (!document.find(L"success")->asBool(false)) {
        const json::Value *cause = document.find(L"cause");
        error = cause ? cause->asString(L"Hypixel reported an unsuccessful response")
                      : L"Hypixel reported an unsuccessful response";
        return false;
    }
    const json::Value *player = document.find(L"player");
    if (player == nullptr || player->type == json::Value::Type::Null) {
        error = L"No Hypixel player exists for this UUID";
        return false;
    }

    const json::Value *stats = player->find(L"stats");
    const json::Value *bedWars = stats ? stats->find(L"Bedwars") : nullptr;
    result.uuid = uuid;
    result.displayName = player->find(L"displayname")
        ? player->find(L"displayname")->asString(uuid)
        : uuid;
    result.wins = bedWars ? std::max<long long>(0, bedWars->find(L"wins_bedwars")
        ? bedWars->find(L"wins_bedwars")->integer(0) : 0) : 0;
    result.losses = bedWars ? std::max<long long>(0, bedWars->find(L"losses_bedwars")
        ? bedWars->find(L"losses_bedwars")->integer(0) : 0) : 0;
    result.finalKills = bedWars ? std::max<long long>(0, bedWars->find(L"final_kills_bedwars")
        ? bedWars->find(L"final_kills_bedwars")->integer(0) : 0) : 0;
    result.finalDeaths = bedWars ? std::max<long long>(0, bedWars->find(L"final_deaths_bedwars")
        ? bedWars->find(L"final_deaths_bedwars")->integer(0) : 0) : 0;
    result.bedsBroken = bedWars ? std::max<long long>(0, bedWars->find(L"beds_broken_bedwars")
        ? bedWars->find(L"beds_broken_bedwars")->integer(0) : 0) : 0;
    result.bedsLost = bedWars ? std::max<long long>(0, bedWars->find(L"beds_lost_bedwars")
        ? bedWars->find(L"beds_lost_bedwars")->integer(0) : 0) : 0;
    result.winRate = static_cast<double>(result.wins)
        / static_cast<double>(std::max<long long>(1, result.losses));
    result.fkdr = static_cast<double>(result.finalKills)
        / static_cast<double>(std::max<long long>(1, result.finalDeaths));
    return true;
}

void HypixelService::lookupPlayer(const std::wstring &playerId)
{
    if (busy()) {
        setStatus(L"A Hypixel request is already in progress");
        return;
    }

    std::wstring requestedId = playerId;
    while (!requestedId.empty() && iswspace(requestedId.back()) != 0)
        requestedId.pop_back();
    while (!requestedId.empty() && iswspace(requestedId.front()) != 0)
        requestedId.erase(requestedId.begin());

    const std::wstring normalized = normalizeUuid(requestedId);
    if (normalized.empty() && !isValidPlayerName(requestedId)) {
        setError(L"Enter a valid Minecraft player name");
        return;
    }
    if (m_apiKeys == nullptr || m_apiKeys->apiKey().empty()) {
        setError(L"HYPIXEL_API_KEY is not configured for this developer-owned build");
        return;
    }

    const unsigned long long now = ::GetTickCount64();

    // The /v2/player contract requires a UUID; serve cached UUID lookups
    // directly (six-hour policy cache).
    if (!normalized.empty()) {
        for (auto iterator = m_cache.begin(); iterator != m_cache.end();) {
            if (iterator->second.expiresAtTick <= now)
                iterator = m_cache.erase(iterator);
            else
                ++iterator;
        }
        const auto cached = m_cache.find(normalized);
        if (cached != m_cache.cend()) {
            cached->second.lastAccessedTick = now;
            applyStats(cached->second.result,
                       L"Using a cached result (six-hour policy cache)");
            return;
        }
    }

    if (now - m_lastRequestTick < kMinimumRequestIntervalMs) {
        setError(L"Please wait one second before another API request");
        return;
    }
    m_lastRequestTick = now;

    m_error.clear();
    setStatus(L"Requesting Bed Wars statistics from Hypixel…");
    m_state = State::Loading;
    if (onChanged)
        onChanged();

    Job job{requestedId, m_apiKeys->apiKey(), m_generation.load()};
    m_jobs.push(std::move(job));
}

void HypixelService::cancel()
{
    ++m_generation;
    m_cancel.store(true);
    (void) WinHttpClient::cancelRequest(m_activeRequest);
    if (m_state == State::Loading) {
        m_state = State::Idle;
        setStatus(L"Hypixel request cancelled");
        if (onChanged)
            onChanged();
    }
}

void HypixelService::reloadConfiguration()
{
    setStatus(m_apiKeys != nullptr && m_apiKeys->configured()
                  ? L"Hypixel API configuration detected"
                  : L"Set HYPIXEL_API_KEY for a registered developer-owned application");
    if (onChanged)
        onChanged();
}

void HypixelService::processMessages()
{
    Message message;
    while (m_queue.pop(message)) {
        switch (message.kind) {
        case MessageKind::Result:
            if (m_state == State::Loading) {
                const unsigned long long now = ::GetTickCount64();
                for (auto iterator = m_cache.begin(); iterator != m_cache.end();) {
                    if (iterator->second.expiresAtTick <= now)
                        iterator = m_cache.erase(iterator);
                    else
                        ++iterator;
                }
                if (m_cache.size() >= kMaximumCacheEntries)
                    m_cache.erase(m_cache.begin());
                m_cache[message.result.uuid] = CacheEntry{
                    message.result, now + kCacheLifetimeMs, now};
                applyStats(message.result,
                           L"Bed Wars statistics updated from Hypixel");
            }
            break;
        case MessageKind::Error:
            if (m_state == State::Loading)
                setError(message.text);
            break;
        case MessageKind::Status:
            break;
        }
    }
}

void HypixelService::applyStats(const Result &result, const std::wstring &status)
{
    m_result = result;
    m_error.clear();
    setStatus(status);
    m_state = State::Ready;
    if (onChanged)
        onChanged();
    if (onResultReady)
        onResultReady();
}

void HypixelService::setError(const std::wstring &message)
{
    m_error = message;
    setStatus(L"Hypixel statistics unavailable");
    m_state = State::Error;
    if (onChanged)
        onChanged();
}

void HypixelService::setStatus(const std::wstring &message)
{
    m_status = message;
}

} // namespace cli
