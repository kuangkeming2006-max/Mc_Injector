#include "PlayerStatsService.h"

#include "Json.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

namespace cli {
namespace {

constexpr uint64_t kDedupeMilliseconds = 10ULL * 60ULL * 1000ULL;
constexpr uint64_t kStatsCacheMilliseconds = 6ULL * 60ULL * 60ULL * 1000ULL;
constexpr uint64_t kHypixelSpacingMilliseconds = 1100ULL;
constexpr uint64_t kIdentityLimit = 64ULL * 1024ULL;
constexpr uint64_t kHypixelLimit = 4ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumRememberedPlayers = 512;
constexpr std::size_t kMaximumCurrentMatchPlayers = 64;

std::wstring toLower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](const wchar_t c) { return towlower(c); });
    return text;
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

int networkLevel(const double experience)
{
    if (!std::isfinite(experience) || experience <= 0.0)
        return 1;
    return std::max(1, static_cast<int>(std::floor(
                           (std::sqrt(2.0 * experience + 30625.0) / 50.0) - 2.5)));
}

} // namespace

bool PlayerStatsService::validName(const std::wstring &name)
{
    if (name.empty() || name.size() > 16)
        return false;
    return std::all_of(name.cbegin(), name.cend(), [](const wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
            || (c >= L'0' && c <= L'9') || c == L'_';
    });
}

bool PlayerStatsService::validTeamPrefix(const std::wstring &prefix)
{
    if (prefix.size() != 2 || prefix.at(0) != static_cast<wchar_t>(0x00A7))
        return false;
    const wchar_t color = prefix.at(1);
    return (color >= L'0' && color <= L'9') || (color >= L'a' && color <= L'f');
}

void PlayerStatsService::start()
{
    if (m_worker.joinable())
        return;
    m_worker = std::thread(&PlayerStatsService::workerLoop, this);
}

void PlayerStatsService::shutdown()
{
    if (!m_worker.joinable())
        return;
    m_shutdown.store(true);
    m_generation.fetch_add(1);
    (void) WinHttpClient::cancelRequest(m_activeRequest);
    if (m_worker.get_id() != std::this_thread::get_id())
        m_worker.join();
    else
        m_worker.detach();
}

void PlayerStatsService::enqueuePlayer(const std::wstring &playerName,
                                       const std::wstring &teamPrefix)
{
    if (!m_matchActive)
        return;

    std::wstring name = playerName;
    while (!name.empty() && iswspace(name.back()) != 0)
        name.pop_back();
    while (!name.empty() && iswspace(name.front()) != 0)
        name.erase(name.begin());
    const std::wstring team = toLower(teamPrefix);
    if (!validName(name) || !validTeamPrefix(team))
        return;

    const std::wstring key = toLower(name);
    if (m_discoveredPlayers.size() >= kMaximumCurrentMatchPlayers
        && m_discoveredPlayers.find(key) == m_discoveredPlayers.end()) {
        return;
    }
    m_discoveredPlayers[key] = Job{name, team, {}, 0};

    const unsigned long long now = ::GetTickCount64();
    for (auto iterator = m_statsCache.begin(); iterator != m_statsCache.end();) {
        if (iterator->second.expiresAtTick <= now)
            iterator = m_statsCache.erase(iterator);
        else
            ++iterator;
    }
    const auto cached = m_statsCache.find(key);
    if (cached != m_statsCache.end()) {
        if (onStatsReady)
            onStatsReady(name, team, cached->second.stars, cached->second.fkdr,
                         cached->second.level);
        m_deduplicateUntil[key] = now + kDedupeMilliseconds;
        return;
    }

    // Discovery is still remembered while no key exists. As soon as the user
    // saves one, reloadConfiguration() restarts this current-match roster.
    if (m_apiKeys == nullptr || !m_apiKeys->configured()) {
        if (onStatsFailed)
            onStatsFailed(name, L"Hypixel API key is not configured");
        return;
    }

    for (auto iterator = m_deduplicateUntil.begin();
         iterator != m_deduplicateUntil.end();) {
        if (iterator->second <= now)
            iterator = m_deduplicateUntil.erase(iterator);
        else
            ++iterator;
    }
    const auto deduplicated = m_deduplicateUntil.find(key);
    if (deduplicated != m_deduplicateUntil.end() && deduplicated->second > now)
        return;
    if (m_deduplicateUntil.size() >= kMaximumRememberedPlayers)
        m_deduplicateUntil.erase(m_deduplicateUntil.begin());
    m_deduplicateUntil[key] = now + kDedupeMilliseconds;

    m_jobs.push(Job{name, team, m_apiKeys->apiKey(), m_generation.load()});
}

void PlayerStatsService::setMatchActive(const bool active)
{
    if (m_matchActive == active)
        return;
    m_matchActive = active;
    cancel();
    // A new match is a new query session even when some names appeared in the
    // previous match. In-match duplicates are still suppressed for ten minutes.
    m_deduplicateUntil.clear();
    m_discoveredPlayers.clear();
}

void PlayerStatsService::reloadConfiguration()
{
    cancel();
    m_deduplicateUntil.clear();
    if (!m_matchActive)
        return;

    // Route through enqueuePlayer again so cached values are returned even if
    // the key was removed, while uncached values wait for a usable key.
    const std::map<std::wstring, Job> discovered = m_discoveredPlayers;
    m_discoveredPlayers.clear();
    for (const auto &entry : discovered)
        enqueuePlayer(entry.second.playerName, entry.second.teamPrefix);
}

void PlayerStatsService::cancel()
{
    ++m_generation;
    m_cancel.store(true);
    (void) WinHttpClient::cancelRequest(m_activeRequest);
}

void PlayerStatsService::workerLoop()
{
    unsigned long long lastHypixelRequestTick = 0;

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

        m_cancel.store(false);
        const auto generationStillCurrent = [&]() {
            return !m_shutdown.load() && !m_cancel.load()
                && job.generation == m_generation.load();
        };

        // 1) Resolve the player name to a UUID through Minecraft Services.
        const std::wstring identityUrl =
            L"https://api.minecraftservices.com/minecraft/profile/lookup/name/"
            + job.playerName;
        HttpResponse identityResponse;
        if (!m_http.get(identityUrl,
                        {{L"Accept", L"application/json"},
                         {L"User-Agent", L"JavaOverlayStudio/1.0"}},
                        kIdentityLimit, identityResponse, &m_activeRequest,
                        &m_cancel)) {
            if (generationStillCurrent())
                m_queue.push(Message{MessageKind::Failed, job.playerName,
                                     job.teamPrefix,
                                     L"Minecraft player ID lookup failed", 0, 0.0, 0});
            continue;
        }
        if (!generationStillCurrent())
            continue;

        const std::string identityBody(
            reinterpret_cast<const char *>(identityResponse.body.data()),
            identityResponse.body.size());
        json::Value identityDocument;
        std::string parseError;
        std::wstring uuid;
        if (json::parse(identityBody, identityDocument, parseError)) {
            const json::Value *id = identityDocument.find(L"id");
            if (id != nullptr)
                uuid = toLower(id->asString());
        }
        if (uuid.size() != 32
            || !std::all_of(uuid.cbegin(), uuid.cend(), [](const wchar_t c) {
                   return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
               })) {
            m_queue.push(Message{MessageKind::Failed, job.playerName,
                                 job.teamPrefix,
                                 L"Minecraft Services returned an invalid player ID",
                                 0, 0.0, 0});
            continue;
        }

        // 2) Space the official API calls at least 1100 ms apart.
        const unsigned long long now = ::GetTickCount64();
        const unsigned long long elapsed = now - lastHypixelRequestTick;
        if (elapsed < kHypixelSpacingMilliseconds) {
            const unsigned long long remaining = kHypixelSpacingMilliseconds - elapsed;
            unsigned long long waited = 0;
            bool abandon = false;
            while (waited < remaining) {
                ::Sleep(static_cast<DWORD>(std::min<uint64_t>(100, remaining - waited)));
                waited += 100;
                if (m_shutdown.load() || m_cancel.load()
                    || job.generation != m_generation.load()) {
                    abandon = true;
                    break;
                }
            }
            if (abandon)
                continue;
        }
        lastHypixelRequestTick = ::GetTickCount64();
        if (!generationStillCurrent())
            continue;

        // 3) Query Hypixel for the Bed Wars aggregate fields.
        if (job.apiKey.empty()) {
            m_queue.push(Message{MessageKind::Failed, job.playerName,
                                 job.teamPrefix,
                                 L"HYPIXEL_API_KEY is not configured", 0, 0.0, 0});
            continue;
        }
        const std::wstring hypixelUrl =
            L"https://api.hypixel.net/v2/player?uuid=" + uuid;
        HttpResponse hypixelResponse;
        if (!m_http.get(hypixelUrl,
                        {{L"API-Key", utf8ToWide(job.apiKey)},
                         {L"Accept", L"application/json"},
                         {L"User-Agent",
                          L"JavaOverlayStudio/1.0 (registered Hypixel application)"}},
                        kHypixelLimit, hypixelResponse, &m_activeRequest,
                        &m_cancel)) {
            if (generationStillCurrent()) {
                const std::wstring reason =
                    hypixelResponse.status == 429
                        ? L"Hypixel rate limit reached"
                        : L"Hypixel query failed";
                m_queue.push(Message{MessageKind::Failed, job.playerName,
                                     job.teamPrefix, reason, 0, 0.0, 0});
            }
            continue;
        }
        if (!generationStillCurrent())
            continue;

        const std::string hypixelBody(
            reinterpret_cast<const char *>(hypixelResponse.body.data()),
            hypixelResponse.body.size());
        json::Value document;
        if (!json::parse(hypixelBody, document, parseError)) {
            m_queue.push(Message{MessageKind::Failed, job.playerName,
                                 job.teamPrefix,
                                 L"Hypixel query failed: invalid JSON", 0, 0.0, 0});
            continue;
        }
        const json::Value *player = document.find(L"player");
        if (document.find(L"success") == nullptr
            || !document.find(L"success")->asBool(false)
            || player == nullptr || player->type == json::Value::Type::Null) {
            m_queue.push(Message{MessageKind::Failed, job.playerName,
                                 job.teamPrefix,
                                 L"Hypixel has no data for this player", 0, 0.0, 0});
            continue;
        }

        const json::Value *stats = player->find(L"stats");
        const json::Value *bedWars = stats ? stats->find(L"Bedwars") : nullptr;
        const json::Value *achievements = player->find(L"achievements");
        const long long finalKills = bedWars && bedWars->find(L"final_kills_bedwars")
            ? bedWars->find(L"final_kills_bedwars")->integer(0)
            : 0;
        const long long finalDeaths = bedWars && bedWars->find(L"final_deaths_bedwars")
            ? bedWars->find(L"final_deaths_bedwars")->integer(0)
            : 0;
        const int stars = static_cast<int>(std::clamp<long long>(
            achievements && achievements->find(L"bedwars_level")
                ? achievements->find(L"bedwars_level")->integer(0)
                : 0,
            0, 100000));
        const double fkdr = static_cast<double>(std::max<long long>(0, finalKills))
            / static_cast<double>(std::max<long long>(1, finalDeaths));
        const int level = networkLevel(player->find(L"networkExp")
                                           ? player->find(L"networkExp")->number
                                           : 0.0);

        m_queue.push(Message{MessageKind::Ready, job.playerName, job.teamPrefix,
                             {}, stars, fkdr, level});
    }
}

void PlayerStatsService::processMessages()
{
    Message message;
    while (m_queue.pop(message)) {
        if (message.kind == MessageKind::Ready) {
            const unsigned long long now = ::GetTickCount64();
            if (m_statsCache.size() >= 1024)
                m_statsCache.erase(m_statsCache.begin());
            m_statsCache[toLower(message.playerName)] = CachedStats{
                message.stars, message.fkdr, message.level,
                now + kStatsCacheMilliseconds};
            if (onStatsReady)
                onStatsReady(message.playerName, message.teamPrefix,
                             message.stars, message.fkdr, message.level);
        } else {
            if (onStatsFailed)
                onStatsFailed(message.playerName, message.reason);
        }
    }
}

} // namespace cli
