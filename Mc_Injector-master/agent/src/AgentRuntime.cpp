#include "AgentRuntime.h"

#include "AgentLog.h"
#include "bindings/GameBindings.h"
#include "ipc_client.h"
#include "jvm.h"
#include "opengl_hook.h"
#include "overlay_renderer.h"

#include <process.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>

namespace mcoverlay {
namespace {

std::mutex g_runtimeMutex;
AgentRuntime* g_runtime = nullptr;

class CallbackGuard final {
public:
    CallbackGuard(std::atomic<unsigned>& count, HANDLE idleEvent) noexcept
        : m_count(count), m_idleEvent(idleEvent)
    {
        if (m_count.fetch_add(1U, std::memory_order_acq_rel) == 0U) {
            ::ResetEvent(m_idleEvent);
        }
    }

    ~CallbackGuard()
    {
        if (m_count.fetch_sub(1U, std::memory_order_acq_rel) == 1U) {
            ::SetEvent(m_idleEvent);
        }
    }

private:
    std::atomic<unsigned>& m_count;
    HANDLE m_idleEvent;
};

class SrwExclusiveGuard final {
public:
    explicit SrwExclusiveGuard(SRWLOCK& lock) noexcept : m_lock(&lock) {}
    ~SrwExclusiveGuard() { ::ReleaseSRWLockExclusive(m_lock); }

    SrwExclusiveGuard(const SrwExclusiveGuard&) = delete;
    SrwExclusiveGuard& operator=(const SrwExclusiveGuard&) = delete;

private:
    SRWLOCK* m_lock;
};

bool parseFlag(std::string_view token, bool& result) noexcept
{
    unsigned parsed = 0U;
    const auto conversion = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != token.data() + token.size() ||
        parsed > 1U) {
        return false;
    }
    result = parsed != 0U;
    return true;
}

std::uint16_t packFeatures(const FeatureSettings& settings) noexcept
{
    return static_cast<std::uint16_t>((settings.espEnabled ? 0x01U : 0U) |
        (settings.entityEspEnabled ? 0x02U : 0U) |
        (settings.bedEspEnabled ? 0x04U : 0U) |
        (settings.labelsEnabled ? 0x08U : 0U) |
        (settings.hypixelPanelEnabled ? 0x10U : 0U) |
        (settings.bedThreatAlertsEnabled ? 0x20U : 0U) |
        (settings.bedDefensePanelEnabled ? 0x40U : 0U) |
        (settings.entityEspPlayersOnly ? 0x80U : 0U) |
        (settings.bedAutoRefreshEnabled ? 0x100U : 0U));
}

FeatureSettings unpackFeatures(const std::uint16_t bits, const int radius = 6) noexcept
{
    FeatureSettings s;
    s.espEnabled = (bits & 0x01U) != 0U;
    s.entityEspEnabled = (bits & 0x02U) != 0U;
    s.bedEspEnabled = (bits & 0x04U) != 0U;
    s.labelsEnabled = (bits & 0x08U) != 0U;
    s.hypixelPanelEnabled = (bits & 0x10U) != 0U;
    s.bedThreatAlertsEnabled = (bits & 0x20U) != 0U;
    s.bedDefensePanelEnabled = (bits & 0x40U) != 0U;
    s.entityEspPlayersOnly = (bits & 0x80U) != 0U;
    s.bedAutoRefreshEnabled = (bits & 0x100U) != 0U;
    s.bedDefenseRadius = std::clamp(radius, 3, 10);
    return s;
}

template<std::size_t Capacity>
bool percentDecode(const std::string_view source,
                   std::array<char, Capacity>& destination) noexcept
{
    destination.fill('\0');
    if (source == "-") return true;
    const auto hexValue = [](const char character) noexcept -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    std::size_t output = 0U;
    for (std::size_t index = 0U; index < source.size();) {
        if (output + 1U >= Capacity) return false;
        unsigned char byte = static_cast<unsigned char>(source[index++]);
        if (byte == '%') {
            if (index + 1U >= source.size()) return false;
            const int high = hexValue(source[index++]);
            const int low = hexValue(source[index++]);
            if (high < 0 || low < 0) return false;
            byte = static_cast<unsigned char>((high << 4) | low);
        }
        if (byte == 0U || byte == '\r' || byte == '\n') return false;
        destination[output++] = static_cast<char>(byte);
    }
    destination[output] = '\0';
    return true;
}

const char* snapshotStateToken(const GameSnapshot::State state) noexcept
{
    switch (state) {
    case GameSnapshot::State::Resolving: return "resolving";
    case GameSnapshot::State::Unsupported: return "unsupported";
    case GameSnapshot::State::WaitingForGameThread: return "waiting_for_game_thread";
    case GameSnapshot::State::NoPlayer: return "no_player";
    case GameSnapshot::State::Ready: return "ready";
    case GameSnapshot::State::JniError: return "jni_error";
    }
    return "unknown";
}

template<std::size_t Capacity>
class FixedLine final {
public:
    [[nodiscard]] bool append(const std::string_view value) noexcept
    {
        if (value.size() > Capacity - m_size) return false;
        std::memcpy(m_data.data() + m_size, value.data(), value.size());
        m_size += value.size();
        return true;
    }

    [[nodiscard]] bool append(const char value) noexcept
    {
        if (m_size == Capacity) return false;
        m_data[m_size++] = value;
        return true;
    }

    template<typename Integer>
    [[nodiscard]] bool appendInteger(const Integer value) noexcept
    {
        static_assert(std::is_integral_v<Integer>);
        const auto result = std::to_chars(m_data.data() + m_size,
                                          m_data.data() + Capacity, value);
        if (result.ec != std::errc{}) return false;
        m_size = static_cast<std::size_t>(result.ptr - m_data.data());
        return true;
    }

    [[nodiscard]] bool appendDouble(const double value) noexcept
    {
        const auto result = std::to_chars(m_data.data() + m_size,
                                          m_data.data() + Capacity, value,
                                          std::chars_format::general, 9);
        if (result.ec != std::errc{}) return false;
        m_size = static_cast<std::size_t>(result.ptr - m_data.data());
        return true;
    }

    [[nodiscard]] std::string_view view() const noexcept
    {
        return {m_data.data(), m_size};
    }

private:
    std::array<char, Capacity> m_data{};
    std::size_t m_size = 0U;
};

template<std::size_t Capacity>
[[nodiscard]] bool appendPercentEncoded(FixedLine<Capacity>& destination,
                                        const char* const value) noexcept
{
    if (value == nullptr || *value == '\0') return destination.append("-");
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char byte : std::string_view(value)) {
        const bool unreserved = (byte >= 'A' && byte <= 'Z') ||
                                (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') ||
                                byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved) {
            if (!destination.append(static_cast<char>(byte))) return false;
        } else {
            if (!destination.append('%') ||
                !destination.append(kHex[(byte >> 4U) & 0x0FU]) ||
                !destination.append(kHex[byte & 0x0FU])) return false;
        }
    }
    return true;
}

std::uint64_t unixMillisecondsNow() noexcept
{
    FILETIME time{};
    ::GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    constexpr std::uint64_t kWindowsToUnixEpoch100ns = 116'444'736'000'000'000ULL;
    return ticks.QuadPart >= kWindowsToUnixEpoch100ns
        ? (ticks.QuadPart - kWindowsToUnixEpoch100ns) / 10'000ULL
        : 0U;
}

template<std::size_t Capacity, typename Message>
[[nodiscard]] bool formatGameState(
    FixedLine<Capacity>& line,
    const Message& message) noexcept
{
    const auto token = snapshotStateToken(
        static_cast<GameSnapshot::State>(message.state));
    return line.append("GAME_STATE 1 ") && line.appendInteger(message.sequence) &&
           line.append(' ') && line.appendInteger(message.unixMilliseconds) &&
           line.append(' ') && line.appendInteger(message.valid ? 1 : 0) &&
           line.append(' ') && line.appendDouble(message.health) &&
           line.append(' ') && line.appendDouble(message.maxHealth) &&
           line.append(' ') && line.appendInteger(message.entityId) &&
           line.append(' ') && line.appendDouble(message.x) &&
           line.append(' ') && line.appendDouble(message.y) &&
           line.append(' ') && line.appendDouble(message.z) &&
           line.append(' ') && line.appendInteger(message.loadedEntities) &&
           line.append(' ') && line.appendInteger(message.bedCount) &&
           line.append(' ') && appendPercentEncoded(line, message.mapping.data()) &&
           line.append(' ') && appendPercentEncoded(line, token);
}

} // namespace

AgentRuntime::AgentRuntime(JavaVM* const vm,
                           jvmtiEnv* const jvmti,
                           AgentOptions options)
    : m_vm(vm), m_jvmti(jvmti), m_options(std::move(options))
{
    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_readyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_callbacksIdleEvent = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    m_rendererStoppedEvent = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
    m_telemetryEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_ipc = std::make_unique<IpcClient>(m_options.pipeName);
    m_hook = std::make_unique<OpenGlHook>();
    m_renderer = std::make_unique<OverlayRenderer>();
    m_bindings = std::make_unique<GameBindings>(m_vm, m_jvmti);
}

AgentRuntime::~AgentRuntime()
{
    if (m_telemetry != nullptr) ::CloseHandle(m_telemetry);
    if (m_telemetryEvent != nullptr) ::CloseHandle(m_telemetryEvent);
    if (m_resolver != nullptr) ::CloseHandle(m_resolver);
    if (m_bedScanner != nullptr) ::CloseHandle(m_bedScanner);
    if (m_worker != nullptr) ::CloseHandle(m_worker);
    if (m_rendererStoppedEvent != nullptr) ::CloseHandle(m_rendererStoppedEvent);
    if (m_callbacksIdleEvent != nullptr) ::CloseHandle(m_callbacksIdleEvent);
    if (m_readyEvent != nullptr) ::CloseHandle(m_readyEvent);
    if (m_stopEvent != nullptr) ::CloseHandle(m_stopEvent);
}

jint AgentRuntime::start(JavaVM* const vm, const char* const rawOptions) noexcept
{
    try {
        AgentOptions options = parseAgentOptions(rawOptions);
        jvmtiEnv* const jvmti = jvm::resolveJvmti(vm);
        if (vm == nullptr || jvmti == nullptr || !options.valid()) {
            log::error("Invalid JVM, JVMTI environment, or agent options.");
            return JNI_ERR;
        }

        std::lock_guard lock(g_runtimeMutex);
        if (g_runtime != nullptr) {
            // Agent_OnAttach and the Windows loader fallback may both reach the
            // same asynchronous bootstrap on older Forge VMs. Treat an already
            // running, identical authenticated session as success. Restarting
            // it would close the live pipe and surface as AGENT_DISCONNECTED.
            const bool sameSession = g_runtime->m_options.protocol == options.protocol &&
                g_runtime->m_options.pipeName == options.pipeName &&
                g_runtime->m_options.token == options.token;
            if (sameSession && g_runtime->m_startSucceeded.load(std::memory_order_acquire) &&
                g_runtime->m_running.load(std::memory_order_acquire) &&
                !g_runtime->m_shutdownRequested.load(std::memory_order_acquire)) {
                log::info("Duplicate bootstrap for the active session was ignored.");
                return JNI_OK;
            }
            g_runtime->requestStop(false);
            g_runtime->join();
            if (!g_runtime->shutdownGraphics()) {
                log::error("Previous agent runtime retained because OpenGL hooks could not be removed.");
                return JNI_ERR;
            }
            delete g_runtime;
            g_runtime = nullptr;
        }

        auto* runtime = new AgentRuntime(vm, jvmti, std::move(options));
        if (!runtime->launch()) {
            // A launch timeout can occur after hooks were installed and the
            // worker entered its cleanup path. Apply the same fail-closed
            // lifetime rule as stop(): never free a context that a detour may
            // still reference.
            if (runtime->shutdownGraphics()) {
                delete runtime;
            } else {
                g_runtime = runtime;
                log::error("Failed launch runtime retained because hooks remain active.");
            }
            return JNI_ERR;
        }
        g_runtime = runtime;
        return JNI_OK;
    } catch (...) {
        log::error("Unhandled exception while starting the agent.");
        return JNI_ERR;
    }
}

void AgentRuntime::stop(const bool vmUnloading) noexcept
{
    std::lock_guard lock(g_runtimeMutex);
    AgentRuntime* const runtime = g_runtime;
    if (runtime == nullptr) {
        return;
    }
    runtime->requestStop(vmUnloading);
    runtime->join();
    if (!runtime->shutdownGraphics()) {
        // A detour may still contain this runtime as its callback context.
        // Retaining the complete object graph is safer than freeing a target
        // that a future SwapBuffers call can still dereference.
        log::error("Agent runtime retained because OpenGL hooks are still active.");
        return;
    }
    g_runtime = nullptr;
    delete runtime;
}

bool AgentRuntime::launch() noexcept
{
    if (m_stopEvent == nullptr || m_readyEvent == nullptr ||
        m_callbacksIdleEvent == nullptr || m_rendererStoppedEvent == nullptr) {
        return false;
    }
    unsigned threadId = 0U;
    m_worker = reinterpret_cast<HANDLE>(::_beginthreadex(
        nullptr, 0U, &AgentRuntime::workerEntry, this, 0U, &threadId));
    if (m_worker == nullptr) {
        return false;
    }
    if (::WaitForSingleObject(m_readyEvent, 5000U) != WAIT_OBJECT_0) {
        requestStop(false);
        join();
        return false;
    }
    const bool succeeded = m_startSucceeded.load(std::memory_order_acquire);
    if (!succeeded) {
        join();
    }
    return succeeded;
}

void AgentRuntime::requestStop(const bool vmUnloading) noexcept
{
    m_vmUnloading.store(vmUnloading, std::memory_order_release);
    if (m_stopEvent != nullptr) {
        ::SetEvent(m_stopEvent);
    }
    if (m_ipc != nullptr) {
        m_ipc->cancel();
    }
}

void AgentRuntime::join() noexcept
{
    if (m_worker != nullptr && ::GetCurrentThreadId() != ::GetThreadId(m_worker)) {
        ::WaitForSingleObject(m_worker, INFINITE);
    }
}

unsigned __stdcall AgentRuntime::workerEntry(void* const context) noexcept
{
    static_cast<AgentRuntime*>(context)->workerMain();
    return 0U;
}

bool AgentRuntime::launchResolver() noexcept
{
    unsigned threadId = 0U;
    m_resolver = reinterpret_cast<HANDLE>(::_beginthreadex(
        nullptr, 0U, &AgentRuntime::resolverEntry, this, 0U, &threadId));
    if (m_resolver == nullptr) {
        m_bindings->markResolverUnavailable();
        log::error("Could not create the asynchronous Minecraft mapping resolver.");
        return false;
    }
    return true;
}

unsigned __stdcall AgentRuntime::resolverEntry(void* const context) noexcept
{
    static_cast<AgentRuntime*>(context)->resolverMain();
    return 0U;
}

void AgentRuntime::resolverMain() noexcept
{
    // ScopedThreadEnv uses AttachCurrentThreadAsDaemon for this CRT-created
    // native thread. Its JNIEnv is resolver-local and is never shared with the
    // Java-owned LWJGL render thread.
    jvm::ScopedThreadEnv environment(m_vm, true);
    if (!environment) {
        m_bindings->markResolverUnavailable();
        log::error("Could not attach the mapping resolver to the JVM.");
        return;
    }
    m_bindings->runResolver(environment.get(), m_stopEvent);
}

void AgentRuntime::joinResolver() noexcept
{
    if (m_resolver != nullptr && ::GetCurrentThreadId() != ::GetThreadId(m_resolver)) {
        ::WaitForSingleObject(m_resolver, INFINITE);
    }
}

bool AgentRuntime::launchBedScanner() noexcept
{
    unsigned threadId = 0U;
    m_bedScanner = reinterpret_cast<HANDLE>(::_beginthreadex(
        nullptr, 0U, &AgentRuntime::bedScannerEntry, this, 0U, &threadId));
    if (m_bedScanner == nullptr) {
        log::error("Could not create the chunk-diff bed scanner thread.");
        return false;
    }
    return true;
}

unsigned __stdcall AgentRuntime::bedScannerEntry(void* const context) noexcept
{
    static_cast<AgentRuntime*>(context)->bedScannerMain();
    return 0U;
}

void AgentRuntime::bedScannerMain() noexcept
{
    jvm::ScopedThreadEnv environment(m_vm, true);
    if (!environment) {
        log::error("Could not attach the chunk-diff bed scanner to the JVM.");
        return;
    }
    m_bindings->runBedScanner(environment.get(), m_stopEvent);
}

void AgentRuntime::joinBedScanner() noexcept
{
    if (m_bedScanner != nullptr &&
        ::GetCurrentThreadId() != ::GetThreadId(m_bedScanner)) {
        ::WaitForSingleObject(m_bedScanner, INFINITE);
    }
}

bool AgentRuntime::launchTelemetry() noexcept
{
    if (m_telemetryEvent == nullptr) {
        log::error("Could not create the telemetry wake event.");
        return false;
    }
    unsigned threadId = 0U;
    m_telemetry = reinterpret_cast<HANDLE>(::_beginthreadex(
        nullptr, 0U, &AgentRuntime::telemetryEntry, this, 0U, &threadId));
    if (m_telemetry == nullptr) {
        log::error("Could not create the IPC telemetry thread.");
        return false;
    }
    return true;
}

unsigned __stdcall AgentRuntime::telemetryEntry(void* const context) noexcept
{
    static_cast<AgentRuntime*>(context)->telemetryMain();
    return 0U;
}

void AgentRuntime::telemetryMain() noexcept
{
    const HANDLE events[2]{m_stopEvent, m_telemetryEvent};
    std::uint64_t sentGameStateRevision = 0U;
    std::uint32_t sentStateChangedRevision = 0U;
    std::uint32_t sentFeatureChangedRevision = 0U;
    std::uint32_t sentBindChangedRevision = 0U;
    std::uint32_t sentGuiScaleChangedRevision = 0U;
    std::uint64_t sentHypixelQueryRevision = 0U;
    std::uint64_t sentPlayerRosterGeneration = 0U;
    while (m_stopEvent != nullptr && m_telemetryEvent != nullptr) {
        const DWORD wait = ::WaitForMultipleObjects(2U, events, FALSE, 250U);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
            return;
        }
        if (!m_handshakeSent.load(std::memory_order_acquire)) {
            continue;
        }

        // Readiness and hotkey state originate in SwapBuffers but pipe writes
        // never do. Failed writes retain their revision and are retried on the
        // next wake/timeout; multiple producer updates intentionally coalesce.
        if (m_rendererReadyQueued.load(std::memory_order_acquire) &&
            !m_rendererReadySent.load(std::memory_order_acquire) &&
            m_ipc->sendLine("RENDERER_READY OpenGL")) {
            m_rendererReadySent.store(true, std::memory_order_release);
        }

        const std::uint32_t stateRevision =
            m_stateChangedRevision.load(std::memory_order_acquire);
        if (stateRevision != sentStateChangedRevision) {
            const std::uint8_t state = m_stateChangedBits.load(std::memory_order_acquire);
            std::array<char, 18U> stateLine{};
            constexpr std::string_view prefix{"STATE_CHANGED "};
            std::memcpy(stateLine.data(), prefix.data(), prefix.size());
            stateLine[prefix.size()] = (state & 0x01U) != 0U ? '1' : '0';
            stateLine[prefix.size() + 1U] = ' ';
            stateLine[prefix.size() + 2U] = (state & 0x02U) != 0U ? '1' : '0';
            const std::string_view message(stateLine.data(), prefix.size() + 3U);
            if (m_ipc->sendLine(message)) {
                sentStateChangedRevision = stateRevision;
            }
        }

        const std::uint32_t featureRevision =
            m_featureChangedRevision.load(std::memory_order_acquire);
        if (featureRevision != sentFeatureChangedRevision) {
            const FeatureSettings settings = unpackFeatures(
                m_featureChangedBits.load(std::memory_order_acquire),
                m_featureChangedBedRadius.load(std::memory_order_acquire));
            FixedLine<96U> line;
            if (line.append("FEATURE_STATE_CHANGED ") &&
                line.appendInteger(settings.espEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.entityEspEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedEspEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.labelsEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedThreatAlertsEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedDefensePanelEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedDefenseRadius) &&
                m_ipc->sendLine(line.view())) {
                sentFeatureChangedRevision = featureRevision;
            }
        }

        const std::uint32_t bindRevision =
            m_bindChangedRevision.load(std::memory_order_acquire);
        if (bindRevision != sentBindChangedRevision) {
            FixedLine<48U> line;
            if (line.append("BIND_CHANGED ") &&
                line.appendInteger(m_bindChangedKey.load(std::memory_order_acquire)) &&
                m_ipc->sendLine(line.view())) {
                sentBindChangedRevision = bindRevision;
            }
        }

        const std::uint32_t guiScaleRevision =
            m_guiScaleChangedRevision.load(std::memory_order_acquire);
        if (guiScaleRevision != sentGuiScaleChangedRevision) {
            FixedLine<48U> line;
            if (line.append("GUI_SCALE_CHANGED ") &&
                line.appendInteger(m_guiScaleChangedIndex.load(std::memory_order_acquire)) &&
                m_ipc->sendLine(line.view())) {
                sentGuiScaleChangedRevision = guiScaleRevision;
            }
        }

        GameStateMessage gameState{};
        std::uint64_t gameStateRevision = 0U;
        ::AcquireSRWLockShared(&m_telemetryLock);
        gameState = m_telemetryMailbox.gameState;
        gameStateRevision = m_telemetryMailbox.gameStateRevision;
        const auto hypixelQuery = m_telemetryMailbox.hypixelQuery;
        const std::uint64_t hypixelQueryRevision =
            m_telemetryMailbox.hypixelQueryRevision;
        const auto players = m_telemetryMailbox.players;
        const std::uint32_t playerCount = m_telemetryMailbox.playerCount;
        const std::uint64_t playerRosterGeneration =
            m_telemetryMailbox.playerRosterGeneration;
        const auto localPlayerName = m_telemetryMailbox.localPlayerName;
        const bool matchActive = m_telemetryMailbox.matchActive;
        ::ReleaseSRWLockShared(&m_telemetryLock);
        if (gameStateRevision != 0U && gameStateRevision != sentGameStateRevision) {
            FixedLine<768U> line;
            if (formatGameState(line, gameState) && m_ipc->sendLine(line.view())) {
                sentGameStateRevision = gameStateRevision;
            }
        }
        if (hypixelQueryRevision != 0U &&
            hypixelQueryRevision != sentHypixelQueryRevision) {
            FixedLine<80U> line;
            if (line.append("HYPIXEL_QUERY ") &&
                appendPercentEncoded(line, hypixelQuery.data()) &&
                m_ipc->sendLine(line.view())) {
                sentHypixelQueryRevision = hypixelQueryRevision;
            }
        }
        if (playerRosterGeneration != 0U &&
            playerRosterGeneration != sentPlayerRosterGeneration) {
            FixedLine<48U> matchLine;
            bool allSent = matchLine.append("MATCH_STATE ") &&
                matchLine.appendInteger(matchActive ? 1 : 0) &&
                m_ipc->sendLine(matchLine.view());
            if (allSent && localPlayerName[0U] != '\0') {
                FixedLine<64U> playerLine;
                allSent = playerLine.append("PLAYER_STATUS ") &&
                    appendPercentEncoded(playerLine, localPlayerName.data()) &&
                    m_ipc->sendLine(playerLine.view());
            }
            for (std::uint32_t index = 0U; allSent && matchActive &&
                 index < playerCount; ++index) {
                const char teamPrefix[4]{
                    static_cast<char>(0xC2), static_cast<char>(0xA7),
                    players[index].teamColor, '\0'};
                FixedLine<96U> line;
                if (!line.append("PLAYER_FOUND ") ||
                    !appendPercentEncoded(line, players[index].name.data()) ||
                    !line.append(' ') || !appendPercentEncoded(line, teamPrefix) ||
                    !m_ipc->sendLine(line.view())) {
                    allSent = false;
                    break;
                }
            }
            if (!matchActive) {
                ::AcquireSRWLockExclusive(&m_playerStatsLock);
                m_playerStats.clear();
                ::ReleaseSRWLockExclusive(&m_playerStatsLock);
            }
            if (allSent) sentPlayerRosterGeneration = playerRosterGeneration;
        }
    }
}

void AgentRuntime::joinTelemetry() noexcept
{
    if (m_telemetry != nullptr && ::GetCurrentThreadId() != ::GetThreadId(m_telemetry)) {
        ::WaitForSingleObject(m_telemetry, INFINITE);
    }
}

void AgentRuntime::queueTelemetry(const GameSnapshot& snapshot,
                                  const std::uint64_t tickMilliseconds) noexcept
{
    constexpr std::uint64_t kTelemetryIntervalMs = 100U;
    if (m_lastTelemetryTick != 0U &&
        tickMilliseconds - m_lastTelemetryTick < kTelemetryIntervalMs) {
        return;
    }
    GameStateMessage message{};
    message.valid = snapshot.state == GameSnapshot::State::Ready;
    message.state = static_cast<std::uint8_t>(snapshot.state);
    message.unixMilliseconds = unixMillisecondsNow();
    const auto finiteOrZero = [](const double value) noexcept {
        return std::isfinite(value) ? value : 0.0;
    };
    if (message.valid) {
        message.health = finiteOrZero(snapshot.health);
        message.maxHealth = finiteOrZero(snapshot.maxHealth);
        message.entityId = snapshot.entityId;
        message.x = finiteOrZero(snapshot.x);
        message.y = finiteOrZero(snapshot.y);
        message.z = finiteOrZero(snapshot.z);
        message.loadedEntities = snapshot.loadedEntities;
        message.bedCount = snapshot.bedCount;
    }
    const char* const mapping = snapshot.mapping != nullptr ? snapshot.mapping : "";
    std::size_t mappingLength = 0U;
    while (mapping[mappingLength] != '\0' &&
           mappingLength + 1U < message.mapping.size()) {
        message.mapping[mappingLength] = mapping[mappingLength];
        ++mappingLength;
    }
    message.mapping[mappingLength] = '\0';

    if (::TryAcquireSRWLockExclusive(&m_telemetryLock) == FALSE) {
        return;
    }
    message.sequence = ++m_telemetrySequence;
    m_telemetryMailbox.gameState = message;
    ++m_telemetryMailbox.gameStateRevision;
    if (snapshot.playerRosterGeneration != 0U &&
        snapshot.playerRosterGeneration != m_telemetryMailbox.playerRosterGeneration) {
        m_telemetryMailbox.players = snapshot.players;
        m_telemetryMailbox.playerCount = snapshot.playerCount;
        m_telemetryMailbox.localPlayerName = snapshot.localPlayerName;
        m_telemetryMailbox.matchActive = snapshot.matchActive;
        m_telemetryMailbox.playerRosterGeneration = snapshot.playerRosterGeneration;
    }
    ::ReleaseSRWLockExclusive(&m_telemetryLock);
    m_lastTelemetryTick = tickMilliseconds;
    if (m_telemetryEvent != nullptr) {
        ::SetEvent(m_telemetryEvent);
    }
}

void AgentRuntime::queueStateChanged(const bool visible, const bool interactive) noexcept
{
    const std::uint8_t state = static_cast<std::uint8_t>(
        (visible ? 0x01U : 0U) | (interactive ? 0x02U : 0U));
    m_stateChangedBits.store(state, std::memory_order_relaxed);
    m_stateChangedRevision.fetch_add(1U, std::memory_order_release);
    if (m_telemetryEvent != nullptr) ::SetEvent(m_telemetryEvent);
}

void AgentRuntime::queueFeatureChanged(const FeatureSettings& settings) noexcept
{
    const std::uint16_t bits = packFeatures(settings);
    m_featureBits.store(bits, std::memory_order_release);
    m_bedDefenseRadius.store(std::clamp(settings.bedDefenseRadius, 3, 10),
                             std::memory_order_release);
    m_featureChangedBits.store(bits, std::memory_order_relaxed);
    m_featureChangedBedRadius.store(std::clamp(settings.bedDefenseRadius, 3, 10),
                                    std::memory_order_relaxed);
    m_featureChangedRevision.fetch_add(1U, std::memory_order_release);
    if (m_telemetryEvent != nullptr) ::SetEvent(m_telemetryEvent);
}

void AgentRuntime::queueHypixelQuery(const std::array<char, 17U>& playerId) noexcept
{
    // Explicit button clicks must not be dropped if telemetry is copying its
    // tiny mailbox at the same instant. Regular game samples remain try-lock
    // based; this one-shot path may wait only for that bounded copy.
    ::AcquireSRWLockExclusive(&m_telemetryLock);
    m_telemetryMailbox.hypixelQuery = playerId;
    ++m_telemetryMailbox.hypixelQueryRevision;
    ::ReleaseSRWLockExclusive(&m_telemetryLock);
    if (m_telemetryEvent != nullptr) ::SetEvent(m_telemetryEvent);
}

void AgentRuntime::queueMenuHotkeyChanged(const unsigned virtualKey) noexcept
{
    m_menuHotkey.store(virtualKey, std::memory_order_release);
    m_bindChangedKey.store(virtualKey, std::memory_order_relaxed);
    m_bindChangedRevision.fetch_add(1U, std::memory_order_release);
    if (m_telemetryEvent != nullptr) ::SetEvent(m_telemetryEvent);
}

void AgentRuntime::queueGuiScaleChanged(const int index) noexcept
{
    const int bounded = std::clamp(index, 0, 3);
    m_guiScaleIndex.store(bounded, std::memory_order_release);
    m_guiScaleChangedIndex.store(bounded, std::memory_order_relaxed);
    m_guiScaleChangedRevision.fetch_add(1U, std::memory_order_release);
    if (m_telemetryEvent != nullptr) ::SetEvent(m_telemetryEvent);
}

void AgentRuntime::queueRendererReady() noexcept
{
    m_rendererReadyQueued.store(true, std::memory_order_release);
    if (m_telemetryEvent != nullptr) ::SetEvent(m_telemetryEvent);
}

bool AgentRuntime::sendHello() noexcept
{
    std::string hello = "HELLO " + std::to_string(m_options.protocol) + " " +
                        std::to_string(::GetCurrentProcessId()) + " " + m_options.token;
    return m_ipc->sendLine(hello);
}

bool AgentRuntime::sendHandshake() noexcept
{
    if (!sendHello()) return false;
    if (!m_ipc->sendLine("HOOK_READY OpenGL")) {
        return false;
    }
    m_handshakeSent.store(true, std::memory_order_release);
    if (m_telemetryEvent != nullptr) {
        ::SetEvent(m_telemetryEvent);
    }
    return true;
}

bool AgentRuntime::handleControlLine(const std::string_view line) noexcept
{
    std::istringstream stream{std::string(line)};
    std::string command;
    stream >> command;
    if (command == "STATE") {
        std::string visibleToken;
        std::string interactiveToken;
        std::string trailing;
        bool visible = false;
        bool interactive = false;
        if (!(stream >> visibleToken >> interactiveToken) || (stream >> trailing) ||
            !parseFlag(visibleToken, visible) || !parseFlag(interactiveToken, interactive)) {
            (void)m_ipc->sendLine("ERROR BAD_STATE expected-STATE-visible-interactive");
            return true;
        }
        if (interactive) visible = true;
        m_visible.store(visible, std::memory_order_release);
        m_interactive.store(interactive, std::memory_order_release);
        // This is a protocol acknowledgement, not a user-facing progress
        // message. Keeping it out of STATUS prevents the controller's friendly
        // "waiting for renderer" text from being overwritten by state-applied.
        (void)m_ipc->sendLine(std::string("STATE_APPLIED ") +
                              (visible ? "1 " : "0 ") +
                              (interactive ? "1" : "0"));
        return true;
    }
    if (command == "FEATURE_STATE") {
        std::array<std::string, 7U> tokens{};
        std::string trailing;
        FeatureSettings settings{};
        bool values[7]{};
        int radius = 0;
        if (!(stream >> tokens[0] >> tokens[1] >> tokens[2] >> tokens[3] >> tokens[4]
              >> tokens[5] >> tokens[6] >> radius) ||
            (stream >> trailing)) {
            (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE expected-seven-flags-and-radius");
            return true;
        }
        for (std::size_t index = 0U; index < tokens.size(); ++index) {
            if (!parseFlag(tokens[index], values[index])) {
                (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE expected-seven-flags-and-radius");
                return true;
            }
        }
        if (radius < 3 || radius > 10) {
            (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE radius-must-be-3-10");
            return true;
        }
        settings.espEnabled = values[0];
        settings.entityEspEnabled = values[1];
        settings.bedEspEnabled = values[2];
        settings.labelsEnabled = values[3];
        settings.hypixelPanelEnabled = values[4];
        settings.bedThreatAlertsEnabled = values[5];
        settings.bedDefensePanelEnabled = values[6];
        settings.bedDefenseRadius = radius;
        m_featureBits.store(packFeatures(settings), std::memory_order_release);
        m_bedDefenseRadius.store(radius, std::memory_order_release);
        (void)m_ipc->sendLine("FEATURE_STATE_APPLIED");
        return true;
    }
    if (command == "BIND") {
        unsigned virtualKey = 0U;
        std::string trailing;
        if (!(stream >> virtualKey) || (stream >> trailing) ||
            virtualKey < 8U || virtualKey > 254U) {
            (void)m_ipc->sendLine("ERROR BAD_BIND expected-virtual-key-8-254");
            return true;
        }
        m_menuHotkey.store(virtualKey, std::memory_order_release);
        (void)m_ipc->sendLine(std::string("BIND_APPLIED ") +
                              std::to_string(virtualKey));
        return true;
    }
    if (command == "GUI_SCALE") {
        int index = -1;
        std::string trailing;
        if (!(stream >> index) || (stream >> trailing) || index < 0 || index > 3) {
            (void)m_ipc->sendLine("ERROR BAD_GUI_SCALE expected-index-0-3");
            return true;
        }
        m_guiScaleIndex.store(index, std::memory_order_release);
        (void)m_ipc->sendLine(std::string("GUI_SCALE_APPLIED ") + std::to_string(index));
        return true;
    }
    if (command == "BED_RESCAN") {
        std::string trailing;
        if (stream >> trailing) {
            (void)m_ipc->sendLine("ERROR BAD_BED_RESCAN expected-no-arguments");
            return true;
        }
        m_bindings->requestBedRescan();
        (void)m_ipc->sendLine("BED_RESCAN_ACCEPTED");
        return true;
    }
    if (command == "STATS") {
        std::string playerToken;
        std::string teamToken;
        std::string trailing;
        PlayerStatsEntry entry{};
        if (!(stream >> playerToken >> teamToken >> entry.stars >> entry.fkdr >> entry.level) ||
            (stream >> trailing) || !std::isfinite(entry.fkdr) ||
            entry.stars < 0 || entry.stars > 100000 || entry.fkdr < 0.0 ||
            entry.fkdr > 1000000.0 || entry.level < 0 || entry.level > 100000 ||
            !percentDecode(playerToken, entry.name) ||
            !percentDecode(teamToken, entry.teamPrefix)) {
            (void)m_ipc->sendLine("ERROR BAD_STATS invalid-payload");
            return true;
        }
        const std::string_view playerName(entry.name.data());
        const bool validName = !playerName.empty() && playerName.size() <= 16U &&
            std::all_of(playerName.begin(), playerName.end(), [](const char character) noexcept {
                return (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') || character == '_';
            });
        const bool validTeam = static_cast<unsigned char>(entry.teamPrefix[0U]) == 0xC2U &&
            static_cast<unsigned char>(entry.teamPrefix[1U]) == 0xA7U &&
            (((entry.teamPrefix[2U] >= '0' && entry.teamPrefix[2U] <= '9') ||
              (entry.teamPrefix[2U] >= 'a' && entry.teamPrefix[2U] <= 'f'))) &&
            entry.teamPrefix[3U] == '\0';
        if (!validName || !validTeam) {
            (void)m_ipc->sendLine("ERROR BAD_STATS invalid-player-or-team");
            return true;
        }
        ::AcquireSRWLockExclusive(&m_playerStatsLock);
        if (!m_playerStats.contains(std::string(playerName)) && m_playerStats.size() >= 256U) {
            m_playerStats.erase(m_playerStats.begin());
        }
        m_playerStats[std::string(playerName)] = entry;
        ::ReleaseSRWLockExclusive(&m_playerStatsLock);
        return true;
    }
    if (command == "HYPIXEL_RESULT") {
        int state = 0;
        std::string uuidToken;
        std::string nameToken;
        std::string statusToken;
        HypixelOverlaySnapshot snapshot{};
        if (!(stream >> state >> uuidToken >> nameToken >> snapshot.wins >> snapshot.losses
              >> snapshot.finalKills >> snapshot.finalDeaths >> snapshot.bedsBroken
              >> snapshot.bedsLost >> snapshot.winRate >> snapshot.fkdr >> statusToken) ||
            state < 0 || state > 3 || !std::isfinite(snapshot.winRate) ||
            !std::isfinite(snapshot.fkdr) ||
            !percentDecode(uuidToken, snapshot.uuid) ||
            !percentDecode(nameToken, snapshot.displayName) ||
            !percentDecode(statusToken, snapshot.status)) {
            (void)m_ipc->sendLine("ERROR BAD_HYPIXEL_RESULT invalid-payload");
            return true;
        }
        snapshot.state = static_cast<HypixelOverlaySnapshot::State>(state);
        ::AcquireSRWLockExclusive(&m_hypixelLock);
        m_hypixelSnapshot = snapshot;
        ::ReleaseSRWLockExclusive(&m_hypixelLock);
        return true;
    }
    if (command == "DETACH") {
        m_visible.store(false, std::memory_order_release);
        m_interactive.store(false, std::memory_order_release);
        m_detachRequested.store(true, std::memory_order_release);
        (void)m_ipc->sendLine("STATUS detaching");
        return false;
    }
    (void)m_ipc->sendLine("ERROR UNKNOWN_COMMAND unsupported-command");
    return true;
}

void AgentRuntime::workerMain() noexcept
{
    if (!m_hook->install(&AgentRuntime::frameEntry, this)) {
        // The asynchronous Agent_OnAttach entry has already returned JNI_OK,
        // so OutputDebugString alone cannot explain this failure to the Qt
        // controller. Keep a minimal pipe worker alive long enough to deliver
        // an authenticated, structured error. A later Retry can then replace
        // this inert runtime without restarting Minecraft.
        m_running.store(true, std::memory_order_release);
        m_startSucceeded.store(true, std::memory_order_release);
        ::SetEvent(m_readyEvent);
        m_ipc->run(m_stopEvent,
            [this] {
                if (!sendHello() ||
                    !m_ipc->sendLine("ERROR HOOK_INSTALL_FAILED unable-to-hook-gdi32-SwapBuffers")) {
                    m_ipc->cancel();
                }
            },
            [this](const std::string_view line) { return handleControlLine(line); },
            [] {},
            [this] {
                m_visible.store(false, std::memory_order_release);
                m_interactive.store(false, std::memory_order_release);
            });
        m_running.store(false, std::memory_order_release);
        return;
    }
    // Mapping support is optional for rendering. If this thread cannot be
    // created, ImGui still initializes and displays an explicit unavailable
    // data state instead of blocking HOOK_READY/RENDERER_READY.
    (void)launchResolver();
    (void)launchBedScanner();
    (void)launchTelemetry();
    m_running.store(true, std::memory_order_release);
    m_startSucceeded.store(true, std::memory_order_release);
    ::SetEvent(m_readyEvent);

    m_ipc->run(m_stopEvent,
        [this] {
            if (!sendHandshake()) {
                m_ipc->cancel();
            }
        },
        [this](const std::string_view line) { return handleControlLine(line); },
        [this] {
            const bool shutdownComplete = shutdownGraphics();
            if (shutdownComplete && m_detachRequested.load(std::memory_order_acquire)) {
                (void)m_ipc->sendLine("DETACH_COMPLETE");
            } else if (!shutdownComplete) {
                (void)m_ipc->sendLine("ERROR HOOK_DISABLE_FAILED runtime-retained");
            }
        },
        [this] {
            m_visible.store(false, std::memory_order_release);
            m_interactive.store(false, std::memory_order_release);
        });
    m_running.store(false, std::memory_order_release);
}

void AgentRuntime::frameEntry(void* const context, HDC const deviceContext) noexcept
{
    // A hook must never unwind through opengl32/gdi32. Runtime-owned work uses
    // fixed storage and nothrow Win32 primitives; this final boundary also
    // contains any unexpected third-party/backend exception.
    try {
        static_cast<AgentRuntime*>(context)->beforeSwapBuffers(deviceContext);
    } catch (...) {
        static_cast<AgentRuntime*>(context)->m_frameFaulted.store(
            true, std::memory_order_release);
    }
}

void AgentRuntime::beforeSwapBuffers(HDC const deviceContext)
{
    CallbackGuard guard(m_activeCallbacks, m_callbacksIdleEvent);
    if (::TryAcquireSRWLockExclusive(&m_renderLock) == FALSE) {
        return;
    }
    SrwExclusiveGuard renderGuard(m_renderLock);

    // Vanilla LWJGL normally calls SwapBuffers on a Java-owned thread, but
    // Lunar can present from a native helper thread. Attach that long-lived
    // renderer once as a daemon instead of doing an Attach/Detach pair every
    // frame. This is required for Mouse.setGrabbed(false) to actually execute
    // on Lunar while the Click GUI is open.
    JNIEnv* env = nullptr;
    if (m_vm != nullptr) {
        const jint environmentResult = m_vm->GetEnv(
            reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (environmentResult == JNI_EDETACHED) {
            JavaVMAttachArgs arguments{};
            arguments.version = JNI_VERSION_1_6;
            arguments.name = const_cast<char*>("McOverlayRender");
            if (m_vm->AttachCurrentThreadAsDaemon(
                    reinterpret_cast<void**>(&env), &arguments) == JNI_OK) {
                m_renderThreadAttachedByAgent = true;
                m_renderJvmThreadId = ::GetCurrentThreadId();
                log::info("Attached the native OpenGL presentation thread to the JVM.");
            } else {
                env = nullptr;
            }
        } else if (environmentResult != JNI_OK) {
            env = nullptr;
        }
    }

    if (m_shutdownRequested.load(std::memory_order_acquire)) {
        if (m_renderCleanupCompleted.load(std::memory_order_acquire)) {
            ::SetEvent(m_rendererStoppedEvent);
            return;
        }
        if (m_renderer->initialized() && !m_renderer->ownsCurrentContext()) {
            return;
        }
        bool expected = false;
        if (!m_renderCleanupStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        if (m_gameInputReleased && env != nullptr) {
            (void)m_bindings->setInputCaptured(env, false);
            m_gameInputReleased = false;
        }
        // This callback is the only place where the renderer's original HGLRC
        // is guaranteed current. JNI globals are released later by the worker,
        // after hooks and all frame callbacks have drained.
        if (m_renderer->initialized()) {
            m_renderer->shutdownWithCurrentContext();
        }
        if (m_renderThreadAttachedByAgent && m_vm != nullptr &&
            m_renderJvmThreadId == ::GetCurrentThreadId()) {
            (void)m_vm->DetachCurrentThread();
            m_renderThreadAttachedByAgent = false;
            m_renderJvmThreadId = 0U;
            env = nullptr;
        }
        m_renderCleanupCompleted.store(true, std::memory_order_release);
        ::SetEvent(m_rendererStoppedEvent);
        return;
    }

    m_renderer->setMenuHotkey(m_menuHotkey.load(std::memory_order_acquire));
    if (m_renderer->consumeClickGuiToggle()) {
        const bool next = !m_interactive.load(std::memory_order_acquire);
        m_interactive.store(next, std::memory_order_release);
        if (next) m_visible.store(true, std::memory_order_release);
        queueStateChanged(m_visible.load(std::memory_order_acquire), next);
    }

    const std::uint64_t tickMilliseconds =
        static_cast<std::uint64_t>(::GetTickCount64());
    const bool interactiveNow = m_interactive.load(std::memory_order_acquire);
    if (env != nullptr) {
        if (interactiveNow && !m_gameInputReleased) {
            if (m_bindings->setInputCaptured(env, true)) {
                m_gameInputReleased = true;
                m_lastInputFocusReleaseTick = tickMilliseconds;
            }
        } else if (interactiveNow) {
            // Lunar and other transformed clients can re-enable relative input
            // every game tick. Keep the public LWJGL grab released each frame,
            // and reassert Minecraft.inGameHasFocus at a bounded 10 Hz when a
            // verified mapping is available.
            (void)m_bindings->maintainInputReleased(env);
            if (tickMilliseconds - m_lastInputFocusReleaseTick >= 100U) {
                (void)m_bindings->setInputCaptured(env, true);
                m_lastInputFocusReleaseTick = tickMilliseconds;
            }
        } else if (m_gameInputReleased && m_bindings->setInputCaptured(env, false)) {
            m_gameInputReleased = false;
            m_lastInputFocusReleaseTick = 0U;
        }
    }

    // Renderer readiness must not depend on Minecraft class mappings. Lunar
    // and other transformed clients may never match the supported 1.8.9 names,
    // but a valid HWND/HGLRC is still sufficient to initialize and display
    // ImGui. GetLoadedClasses and ID lookup live exclusively on m_resolver;
    // this callback only copies atomic progress and samples an already-published
    // immutable cache.
    if (env != nullptr) {
        (void)m_bindings->sample(env, tickMilliseconds);
        // ActiveRenderInfo changes with every camera transform. Keep this out
        // of the 10 Hz telemetry sampler so rotation, FOV and view bobbing are
        // reflected by the very same frame that is about to be presented.
        m_bindings->sampleCamera(env);
    }
    GameSnapshot snapshot = m_bindings->snapshot(tickMilliseconds);
    if (m_visible.load(std::memory_order_acquire)) {
        m_renderer->setGuiScaleIndex(m_guiScaleIndex.load(std::memory_order_acquire));
        m_renderer->setFeatureSettings(unpackFeatures(
            m_featureBits.load(std::memory_order_acquire),
            m_bedDefenseRadius.load(std::memory_order_acquire)));
        ::AcquireSRWLockShared(&m_hypixelLock);
        const HypixelOverlaySnapshot hypixel = m_hypixelSnapshot;
        ::ReleaseSRWLockShared(&m_hypixelLock);
        m_renderer->setHypixelSnapshot(hypixel);
        PlayerStatsOverlaySnapshot playerStats{};
        ::AcquireSRWLockShared(&m_playerStatsLock);
        for (const auto& [name, stats] : m_playerStats) {
            (void)name;
            if (playerStats.count >= playerStats.entries.size()) break;
            playerStats.entries[playerStats.count++] = stats;
        }
        ::ReleaseSRWLockShared(&m_playerStatsLock);
        m_renderer->setPlayerStatsSnapshot(playerStats);
        (void)m_renderer->render(deviceContext, snapshot,
                                 m_interactive.load(std::memory_order_acquire));
        FeatureSettings changedFeatures{};
        if (m_renderer->consumeFeatureSettings(changedFeatures)) {
            queueFeatureChanged(changedFeatures);
        }
        std::array<char, 17U> query{};
        if (m_renderer->consumeHypixelQuery(query)) {
            queueHypixelQuery(query);
        }
        unsigned changedHotkey = 0U;
        if (m_renderer->consumeMenuHotkeyChange(changedHotkey)) {
            queueMenuHotkeyChanged(changedHotkey);
        }
        int changedScale = 0;
        if (m_renderer->consumeGuiScaleChange(changedScale)) {
            queueGuiScaleChanged(changedScale);
        }
        if (m_renderer->consumeBedRescanRequest()) {
            m_bindings->requestBedRescan();
        }
        if (m_renderer->initialized() && m_handshakeSent.load(std::memory_order_acquire) &&
            !m_rendererReadySent.load(std::memory_order_acquire)) {
            queueRendererReady();
        }
    }

    queueTelemetry(snapshot, tickMilliseconds);
}

bool AgentRuntime::shutdownGraphics() noexcept
{
    if (m_cleanupCompleted.load(std::memory_order_acquire)) {
        return true;
    }

    m_visible.store(false, std::memory_order_release);
    m_interactive.store(false, std::memory_order_release);

    // Signal first so a resolver backoff wait exits immediately, then join
    // before any callback is allowed to release the published global refs.
    // A currently executing JVMTI snapshot is allowed to finish; it can no
    // longer race cache destruction afterwards.
    if (m_stopEvent != nullptr) {
        ::SetEvent(m_stopEvent);
    }
    joinResolver();
    joinBedScanner();
    joinTelemetry();

    // Give the owning SwapBuffers/window thread a bounded opportunity to shut
    // down both ImGui backends while the renderer's HGLRC is current. The
    // callback sees m_shutdownRequested before any hook is disabled, so this
    // cannot depend on a detour after its target prologue has been restored.
    // Reset before publication: resetting after the store could erase a signal
    // from a callback that completed between the flag store and ResetEvent.
    ::ResetEvent(m_rendererStoppedEvent);
    m_shutdownRequested.store(true, std::memory_order_release);
    if (!m_renderCleanupCompleted.load(std::memory_order_acquire)) {
        (void)::WaitForSingleObject(m_rendererStoppedEvent, 750U);
    }

    // If MinHook cannot restore every target, the complete runtime remains
    // alive. m_shutdownRequested is intentionally left set: any later detour
    // can finish render-thread cleanup safely, but it never samples JNI state
    // or renders another frame.
    bool disabled = false;
    constexpr unsigned kDisableAttempts = 5U;
    for (unsigned attempt = 0U; attempt < kDisableAttempts; ++attempt) {
        if (m_hook->disable()) {
            disabled = true;
            break;
        }
        ::SwitchToThread();
        if (attempt + 1U < kDisableAttempts) {
            ::Sleep(1U);
        }
    }
    if (!disabled) {
        log::error("OpenGL hooks remain active after bounded disable retries; retaining runtime.");
        return false;
    }

    // remove() drains all detours before clearing callback/trampoline state.
    // If it reports failure, preserve the full graph even though publication
    // may already be cancelled; patched target code still belongs to this DLL.
    if (!m_hook->remove()) {
        log::error("OpenGL hook removal failed; retaining runtime and renderer state.");
        return false;
    }

    ::WaitForSingleObject(m_callbacksIdleEvent, INFINITE);
    // No frame arrived with the old HGLRC (or the context changed). The hook is
    // now disabled and all callbacks are drained, so detach input and retain
    // the backend generation without calling GL teardown or DestroyContext.
    // This is a bounded leak, preferable to issuing glDelete* in another HGLRC
    // or tripping Dear ImGui's backend-lifetime assertion.
    m_renderer->abandonAfterHookDisabled();
    if (m_vmUnloading.load(std::memory_order_acquire)) {
        m_bindings->abandon();
    } else {
        jvm::ScopedThreadEnv cleanupEnvironment(m_vm, true);
        if (cleanupEnvironment) {
            m_bindings->release(cleanupEnvironment.get());
        } else {
            m_bindings->abandon();
        }
    }
    m_cleanupCompleted.store(true, std::memory_order_release);
    ::SetEvent(m_rendererStoppedEvent);
    return true;
}

} // namespace mcoverlay
