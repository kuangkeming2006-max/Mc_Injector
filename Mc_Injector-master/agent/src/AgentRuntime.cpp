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

std::uint32_t packFeatures(const FeatureSettings& settings) noexcept
{
    return static_cast<std::uint32_t>((settings.espEnabled ? 0x01U : 0U) |
        (settings.entityEspEnabled ? 0x02U : 0U) |
        (settings.bedEspEnabled ? 0x04U : 0U) |
        (settings.labelsEnabled ? 0x08U : 0U) |
        (settings.hypixelPanelEnabled ? 0x10U : 0U) |
        (settings.bedThreatAlertsEnabled ? 0x20U : 0U) |
        (settings.bedDefensePanelEnabled ? 0x40U : 0U) |
        (settings.entityEspPlayersOnly ? 0x80U : 0U) |
        (settings.bedAutoRefreshEnabled ? 0x100U : 0U) |
        (settings.bedEspFilled ? 0x200U : 0U) |
        (settings.debugChatEnabled ? 0x400U : 0U) |
        (settings.showOwnBedDefenseInfo ? 0x800U : 0U) |
        (settings.showTeammateBoxes ? 0x1000U : 0U) |
        (settings.bedDefenseHoldToShow ? 0x2000U : 0U) |
        (settings.bedDefensePerspectiveScale ? 0x4000U : 0U) |
        (settings.hypixelPanelHoldToShow ? 0x8000U : 0U) |
        (settings.nametagEnabled ? 0x10000U : 0U) |
        (settings.nametagSidePlacement ? 0x20000U : 0U) |
        (settings.enemyItemIndicatorsEnabled ? 0x40000U : 0U) |
        (settings.showTeammateNametags ? 0x80000U : 0U) |
        (settings.nametagNearbyEnemiesOnly ? 0x100000U : 0U) |
        (settings.nametagTeamPulse ? 0x200000U : 0U) |
        (settings.showTeammateArrows ? 0x400000U : 0U) |
        (settings.safewalkEnabled ? 0x800000U : 0U) |
        (settings.scaffoldEnabled ? 0x1000000U : 0U) |
        (settings.flyEnabled ? 0x2000000U : 0U) |
        (settings.bhopEnabled ? 0x4000000U : 0U) |
        (settings.bhopAutoJump ? 0x8000000U : 0U) |
        (settings.aimAssistEnabled ? 0x10000000U : 0U) |
        (settings.aimSlowdownMode ? 0x20000000U : 0U) |
        (settings.textGuiEnabled ? 0x40000000U : 0U) |
        (settings.allowHypixelMovement ? 0x80000000U : 0U));
}

std::uint32_t packExtraFeatures(const FeatureSettings& settings) noexcept
{
    return static_cast<std::uint32_t>(
        (settings.aimNearestPriority ? 0x01U : 0U) |
        (settings.textGuiVerticalLine ? 0x02U : 0U) |
        (settings.knockbackPredictionEnabled ? 0x04U : 0U) |
        (settings.bowPredictionEnabled ? 0x08U : 0U) |
        (settings.localMobAuraEnabled ? 0x10U : 0U) |
        (settings.localVelocityEnabled ? 0x20U : 0U) |
        (settings.scaffoldSameLayerOnly ? 0x40U : 0U) |
        (settings.fullscreenImeFixEnabled ? 0x80U : 0U));
}

std::uint64_t packFeatureHotkeys(const FeatureSettings& settings,
                                 const std::size_t first) noexcept
{
    std::uint64_t packed = 0U;
    for (std::size_t offset = 0U; offset < 8U; ++offset) {
        const std::size_t index = first + offset;
        if (index >= settings.featureHotkeys.size()) break;
        const auto key = static_cast<std::uint64_t>(std::clamp(
            settings.featureHotkeys[index], 0, 254));
        packed |= key << (offset * 8U);
    }
    return packed;
}

FeatureSettings unpackFeatures(const std::uint32_t bits,
                               const int defenseRadius = 6,
                               const int threatRadius = 8,
                               const int bedHotkey = VK_LMENU,
                                const int panelOpacity = 78,
                                const int hypixelHotkey = VK_TAB,
                                const int hypixelOpacity = 76,
                                const int hypixelScale = 100,
                                const int hypixelX = -1,
                                const int hypixelY = -1,
                                const bool clickGuiLightTheme = false,
                                const std::uint32_t playerColor = 0xFF3B30U,
                                const std::uint32_t bedColor = 0xFF5C68U,
                                const std::uint32_t panelColor = 0x191621U,
                                const std::uint32_t hypixelColor = 0x000000U,
                                const int hypixelHeight = 100,
                                const int nametagOpacity = 82,
                                const std::uint32_t nametagColor = 0x101218U,
                                const std::uint32_t accentColor = 0x825DE8U,
                                const int hypixelFontIndex = 1,
                                const int nametagRange = 32,
                                const int nametagSizeIndex = 1,
                                const std::uint32_t hypixelRailColor = 0x825DE8U,
                                const int hypixelRailOpacity = 100,
                                const int safewalkReleaseDelayMs = 120,
                                const int safewalkEdgeSensitivity = 55,
                                const int safewalkMinimumPitch = -5,
                                const int safewalkHotkey = VK_F8,
                                const int flySpeedPercent = 100,
                                const int aimSlowdownPercent = 45,
                                const int aimSpeedPercent = 35,
                                const std::uint32_t textGuiColor = 0x7EE7FFU,
                                const int textGuiX = -1,
                                const int textGuiY = -1,
                                const int bhopAirSpeedPercent = 100,
                                const std::uint64_t hotkeysPackedA = 0U,
                                const std::uint64_t hotkeysPackedB = 0U,
                                const bool fireballEspEnabled = false,
                                const bool fireballEspFilled = true,
                                const bool longJumpEnabled = false,
                                const int longJumpSpeedPercent = 100,
                                const std::uint32_t fireballEspColor = 0xFF9D3DU,
                                const int aimMinimumDistance = 0,
                                const int aimMaximumDistance = 16,
                                const int aimFovDegrees = 90,
                                const int clickGuiWidthPercent = 100,
                                const int clickGuiHeightPercent = 100,
                                const int clickGuiOpacity = 96,
                                const std::uint32_t extraBits = 0x43U,
                                const int textGuiAlignment = 2,
                                const int localMobReach = 4,
                                const int localAttackDelayMs = 500,
                                const int localVelocityPercent = 100) noexcept
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
    s.bedEspFilled = (bits & 0x200U) != 0U;
    s.debugChatEnabled = (bits & 0x400U) != 0U;
    s.showOwnBedDefenseInfo = (bits & 0x800U) != 0U;
    s.showTeammateBoxes = (bits & 0x1000U) != 0U;
    s.bedDefenseHoldToShow = (bits & 0x2000U) != 0U;
    s.bedDefensePerspectiveScale = (bits & 0x4000U) != 0U;
    s.hypixelPanelHoldToShow = (bits & 0x8000U) != 0U;
    s.nametagEnabled = (bits & 0x10000U) != 0U;
    s.nametagSidePlacement = (bits & 0x20000U) != 0U;
    s.enemyItemIndicatorsEnabled = (bits & 0x40000U) != 0U;
    s.showTeammateNametags = (bits & 0x80000U) != 0U;
    s.nametagNearbyEnemiesOnly = (bits & 0x100000U) != 0U;
    s.nametagTeamPulse = (bits & 0x200000U) != 0U;
    s.showTeammateArrows = (bits & 0x400000U) != 0U;
    s.safewalkEnabled = (bits & 0x800000U) != 0U;
    s.scaffoldEnabled = (bits & 0x1000000U) != 0U;
    s.flyEnabled = (bits & 0x2000000U) != 0U;
    s.bhopEnabled = (bits & 0x4000000U) != 0U;
    s.bhopAutoJump = (bits & 0x8000000U) != 0U;
    s.aimAssistEnabled = (bits & 0x10000000U) != 0U;
    s.aimSlowdownMode = (bits & 0x20000000U) != 0U;
    s.textGuiEnabled = (bits & 0x40000000U) != 0U;
    s.allowHypixelMovement = (bits & 0x80000000U) != 0U;
    s.bedDefenseRadius = std::clamp(defenseRadius, 3, 10);
    s.bedThreatRadius = std::clamp(threatRadius, 3, 32);
    s.bedDefenseHotkey = std::clamp(bedHotkey, 8, 254);
    s.bedDefensePanelOpacity = std::clamp(panelOpacity, 0, 100);
    s.hypixelPanelHotkey = std::clamp(hypixelHotkey, 8, 254);
    s.hypixelPanelOpacity = std::clamp(hypixelOpacity, 0, 100);
    s.hypixelPanelScale = std::clamp(hypixelScale, 70, 160);
    s.hypixelPanelHeight = std::clamp(hypixelHeight, 60, 400);
    s.hypixelPanelX = std::clamp(hypixelX, -1, 1000);
    s.hypixelPanelY = std::clamp(hypixelY, -1, 1000);
    s.clickGuiLightTheme = clickGuiLightTheme;
    s.playerEspColor = playerColor & 0xFFFFFFU;
    s.bedEspColor = bedColor & 0xFFFFFFU;
    s.bedDefensePanelColor = panelColor & 0xFFFFFFU;
    s.hypixelPanelColor = hypixelColor & 0xFFFFFFU;
    s.nametagPanelOpacity = std::clamp(nametagOpacity, 10, 100);
    s.nametagPanelColor = nametagColor & 0xFFFFFFU;
    s.clickGuiAccentColor = accentColor & 0xFFFFFFU;
    s.hypixelPanelFontIndex = std::clamp(hypixelFontIndex, 0, 3);
    s.nametagRange = std::clamp(nametagRange, 4, 128);
    s.nametagSizeIndex = std::clamp(nametagSizeIndex, 0, 3);
    s.hypixelRailColor = hypixelRailColor & 0xFFFFFFU;
    s.hypixelRailOpacity = std::clamp(hypixelRailOpacity, 0, 100);
    s.safewalkReleaseDelayMs = std::clamp(safewalkReleaseDelayMs, 0, 750);
    s.safewalkEdgeSensitivity = std::clamp(safewalkEdgeSensitivity, 0, 100);
    s.safewalkMinimumPitch = std::clamp(safewalkMinimumPitch, -90, 90);
    s.safewalkHotkey = std::clamp(safewalkHotkey, 8, 254);
    s.flySpeedPercent = std::clamp(flySpeedPercent, 10, 500);
    s.aimSlowdownPercent = std::clamp(aimSlowdownPercent, 5, 95);
    s.aimSpeedPercent = std::clamp(aimSpeedPercent, 1, 100);
    s.textGuiColor = textGuiColor & 0xFFFFFFU;
    s.textGuiX = std::clamp(textGuiX, -1, 1000);
    s.textGuiY = std::clamp(textGuiY, -1, 1000);
    s.bhopAirSpeedPercent = std::clamp(bhopAirSpeedPercent, 10, 300);
    for (std::size_t index = 0U; index < s.featureHotkeys.size(); ++index) {
        const std::uint64_t packed = index < 8U ? hotkeysPackedA : hotkeysPackedB;
        const std::size_t offset = index < 8U ? index : index - 8U;
        s.featureHotkeys[index] = static_cast<int>((packed >> (offset * 8U)) & 0xFFU);
    }
    // Preserve the legacy Safewalk binding as a migration source. New builds
    // keep both fields synchronized, while old settings remain usable.
    if (s.featureHotkeys[4U] == 0) s.featureHotkeys[4U] = s.safewalkHotkey;
    else s.safewalkHotkey = s.featureHotkeys[4U];
    s.fireballEspEnabled = fireballEspEnabled;
    s.fireballEspFilled = fireballEspFilled;
    s.longJumpEnabled = longJumpEnabled;
    s.longJumpSpeedPercent = std::clamp(longJumpSpeedPercent, 25, 250);
    s.fireballEspColor = fireballEspColor & 0xFFFFFFU;
    s.aimMinimumDistance = std::clamp(aimMinimumDistance, 0, 64);
    s.aimMaximumDistance = std::clamp(aimMaximumDistance,
                                      std::max(1, s.aimMinimumDistance), 128);
    s.aimFovDegrees = std::clamp(aimFovDegrees, 1, 360);
    s.clickGuiWidthPercent = std::clamp(clickGuiWidthPercent, 80, 150);
    s.clickGuiHeightPercent = std::clamp(clickGuiHeightPercent, 80, 150);
    s.clickGuiOpacity = std::clamp(clickGuiOpacity, 35, 100);
    s.aimNearestPriority = (extraBits & 0x01U) != 0U;
    s.textGuiVerticalLine = (extraBits & 0x02U) != 0U;
    s.knockbackPredictionEnabled = (extraBits & 0x04U) != 0U;
    s.bowPredictionEnabled = (extraBits & 0x08U) != 0U;
    s.localMobAuraEnabled = (extraBits & 0x10U) != 0U;
    s.localVelocityEnabled = (extraBits & 0x20U) != 0U;
    s.scaffoldSameLayerOnly = (extraBits & 0x40U) != 0U;
    s.fullscreenImeFixEnabled = (extraBits & 0x80U) != 0U;
    s.textGuiAlignment = std::clamp(textGuiAlignment, 0, 2);
    s.localMobReach = std::clamp(localMobReach, 3, 10);
    s.localAttackDelayMs = std::clamp(localAttackDelayMs, 100, 1500);
    s.localVelocityPercent = std::clamp(localVelocityPercent, 0, 100);
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
    std::uint32_t sentBlacklistActionRevision = 0U;
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
                m_featureChangedBedRadius.load(std::memory_order_acquire),
                m_featureChangedThreatRadius.load(std::memory_order_acquire),
                m_featureChangedBedHotkey.load(std::memory_order_acquire),
                m_featureChangedPanelOpacity.load(std::memory_order_acquire),
                m_featureChangedHypixelHotkey.load(std::memory_order_acquire),
                m_featureChangedHypixelOpacity.load(std::memory_order_acquire),
                m_featureChangedHypixelScale.load(std::memory_order_acquire),
                m_featureChangedHypixelX.load(std::memory_order_acquire),
                m_featureChangedHypixelY.load(std::memory_order_acquire),
                m_featureChangedClickGuiLightTheme.load(std::memory_order_acquire),
                m_featureChangedPlayerColor.load(std::memory_order_acquire),
                m_featureChangedBedColor.load(std::memory_order_acquire),
                m_featureChangedPanelColor.load(std::memory_order_acquire),
                m_featureChangedHypixelColor.load(std::memory_order_acquire),
                m_featureChangedHypixelHeight.load(std::memory_order_acquire),
                m_featureChangedNametagOpacity.load(std::memory_order_acquire),
                m_featureChangedNametagColor.load(std::memory_order_acquire),
                m_featureChangedAccentColor.load(std::memory_order_acquire),
                m_featureChangedHypixelFontIndex.load(std::memory_order_acquire),
                m_featureChangedNametagRange.load(std::memory_order_acquire),
                m_featureChangedNametagSizeIndex.load(std::memory_order_acquire),
                m_featureChangedHypixelRailColor.load(std::memory_order_acquire),
                m_featureChangedHypixelRailOpacity.load(std::memory_order_acquire),
                m_featureChangedSafewalkReleaseDelayMs.load(
                    std::memory_order_acquire),
                m_featureChangedSafewalkEdgeSensitivity.load(std::memory_order_acquire),
                m_featureChangedSafewalkMinimumPitch.load(std::memory_order_acquire),
                m_featureChangedSafewalkHotkey.load(std::memory_order_acquire),
                m_featureChangedFlySpeedPercent.load(std::memory_order_acquire),
                m_featureChangedAimSlowdownPercent.load(std::memory_order_acquire),
                m_featureChangedAimSpeedPercent.load(std::memory_order_acquire),
                m_featureChangedTextGuiColor.load(std::memory_order_acquire),
                m_featureChangedTextGuiX.load(std::memory_order_acquire),
                m_featureChangedTextGuiY.load(std::memory_order_acquire),
                m_featureChangedBhopAirSpeedPercent.load(std::memory_order_acquire),
                m_featureChangedHotkeysPackedA.load(std::memory_order_acquire),
                m_featureChangedHotkeysPackedB.load(std::memory_order_acquire),
                m_featureChangedFireballEspEnabled.load(std::memory_order_acquire),
                m_featureChangedFireballEspFilled.load(std::memory_order_acquire),
                m_featureChangedLongJumpEnabled.load(std::memory_order_acquire),
                m_featureChangedLongJumpSpeedPercent.load(std::memory_order_acquire),
                m_featureChangedFireballEspColor.load(std::memory_order_acquire),
                m_featureChangedAimMinimumDistance.load(std::memory_order_acquire),
                m_featureChangedAimMaximumDistance.load(std::memory_order_acquire),
                m_featureChangedAimFovDegrees.load(std::memory_order_acquire),
                m_featureChangedClickGuiWidthPercent.load(std::memory_order_acquire),
                m_featureChangedClickGuiHeightPercent.load(std::memory_order_acquire),
                m_featureChangedClickGuiOpacity.load(std::memory_order_acquire),
                m_featureChangedExtraBits.load(std::memory_order_acquire),
                m_featureChangedTextGuiAlignment.load(std::memory_order_acquire),
                m_featureChangedLocalMobReach.load(std::memory_order_acquire),
                m_featureChangedLocalAttackDelayMs.load(std::memory_order_acquire),
                m_featureChangedLocalVelocityPercent.load(std::memory_order_acquire));
            FixedLine<960U> line;
            if (line.append("FEATURE_STATE_CHANGED ") &&
                line.appendInteger(settings.espEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.entityEspEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedEspEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.labelsEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedThreatAlertsEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedDefensePanelEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.entityEspPlayersOnly ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedAutoRefreshEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedEspFilled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.debugChatEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.showOwnBedDefenseInfo ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.showTeammateBoxes ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedDefenseHoldToShow ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedDefensePerspectiveScale ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelHoldToShow ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.nametagEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.nametagSidePlacement ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.enemyItemIndicatorsEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.showTeammateNametags ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.nametagNearbyEnemiesOnly ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.nametagTeamPulse ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.showTeammateArrows ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.safewalkEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.scaffoldEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.flyEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bhopEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bhopAutoJump ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.aimAssistEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.aimSlowdownMode ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.textGuiEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.allowHypixelMovement ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.bedDefenseRadius) && line.append(' ') &&
                line.appendInteger(settings.bedThreatRadius) && line.append(' ') &&
                line.appendInteger(settings.bedDefenseHotkey) && line.append(' ') &&
                line.appendInteger(settings.bedDefensePanelOpacity) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelHotkey) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelOpacity) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelScale) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelX) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelY) && line.append(' ') &&
                line.appendInteger(settings.clickGuiLightTheme ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.playerEspColor) && line.append(' ') &&
                line.appendInteger(settings.bedEspColor) && line.append(' ') &&
                line.appendInteger(settings.bedDefensePanelColor) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelColor) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelHeight) && line.append(' ') &&
                line.appendInteger(settings.nametagPanelOpacity) && line.append(' ') &&
                line.appendInteger(settings.nametagPanelColor) && line.append(' ') &&
                line.appendInteger(settings.clickGuiAccentColor) && line.append(' ') &&
                line.appendInteger(settings.hypixelPanelFontIndex) && line.append(' ') &&
                line.appendInteger(settings.nametagRange) && line.append(' ') &&
                line.appendInteger(settings.nametagSizeIndex) && line.append(' ') &&
                line.appendInteger(settings.hypixelRailColor) && line.append(' ') &&
                line.appendInteger(settings.hypixelRailOpacity) && line.append(' ') &&
                line.appendInteger(settings.safewalkReleaseDelayMs) && line.append(' ') &&
                line.appendInteger(settings.safewalkEdgeSensitivity) && line.append(' ') &&
                line.appendInteger(settings.safewalkMinimumPitch) && line.append(' ') &&
                line.appendInteger(settings.safewalkHotkey) && line.append(' ') &&
                line.appendInteger(settings.flySpeedPercent) && line.append(' ') &&
                line.appendInteger(settings.aimSlowdownPercent) && line.append(' ') &&
                line.appendInteger(settings.aimSpeedPercent) && line.append(' ') &&
                line.appendInteger(settings.textGuiColor) && line.append(' ') &&
                line.appendInteger(settings.textGuiX) && line.append(' ') &&
                line.appendInteger(settings.textGuiY) && line.append(' ') &&
                line.appendInteger(settings.bhopAirSpeedPercent) && line.append(' ') &&
                line.appendInteger(packFeatureHotkeys(settings, 0U)) && line.append(' ') &&
                line.appendInteger(packFeatureHotkeys(settings, 8U)) && line.append(' ') &&
                line.appendInteger(settings.fireballEspEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.fireballEspFilled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.longJumpEnabled ? 1 : 0) && line.append(' ') &&
                line.appendInteger(settings.longJumpSpeedPercent) && line.append(' ') &&
                line.appendInteger(settings.fireballEspColor) && line.append(' ') &&
                line.appendInteger(settings.aimMinimumDistance) && line.append(' ') &&
                line.appendInteger(settings.aimMaximumDistance) && line.append(' ') &&
                line.appendInteger(settings.aimFovDegrees) && line.append(' ') &&
                line.appendInteger(settings.clickGuiWidthPercent) && line.append(' ') &&
                line.appendInteger(settings.clickGuiHeightPercent) && line.append(' ') &&
                line.appendInteger(settings.clickGuiOpacity) && line.append(' ') &&
                line.appendInteger(packExtraFeatures(settings)) && line.append(' ') &&
                line.appendInteger(settings.textGuiAlignment) && line.append(' ') &&
                line.appendInteger(settings.localMobReach) && line.append(' ') &&
                line.appendInteger(settings.localAttackDelayMs) && line.append(' ') &&
                line.appendInteger(settings.localVelocityPercent) &&
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

        const std::uint32_t blacklistActionRevision =
            m_blacklistActionRevision.load(std::memory_order_acquire);
        if (blacklistActionRevision != sentBlacklistActionRevision) {
            BlacklistAction action{};
            ::AcquireSRWLockShared(&m_blacklistActionLock);
            action = m_blacklistAction;
            ::ReleaseSRWLockShared(&m_blacklistActionLock);
            FixedLine<1024U> actionLine;
            bool formatted = false;
            switch (action.type) {
            case BlacklistAction::Type::Add:
                formatted = actionLine.append("BLACKLIST_ADD ") &&
                    appendPercentEncoded(actionLine, action.name.data()) && actionLine.append(' ') &&
                    appendPercentEncoded(actionLine, action.uuid.data()) && actionLine.append(' ') &&
                    appendPercentEncoded(actionLine, action.reason.data()) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.idOnlyNick ? 1 : 0) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.warnOnEncounter ? 1 : 0);
                break;
            case BlacklistAction::Type::Remove:
                formatted = actionLine.append("BLACKLIST_REMOVE ") &&
                    appendPercentEncoded(actionLine, action.key.data());
                break;
            case BlacklistAction::Type::Warning:
                formatted = actionLine.append("BLACKLIST_WARNING ") &&
                    appendPercentEncoded(actionLine, action.key.data()) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.warnOnEncounter ? 1 : 0);
                break;
            case BlacklistAction::Type::Layout:
                formatted = actionLine.append("BLACKLIST_LAYOUT ") &&
                    actionLine.appendInteger(action.x) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.y) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.width) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.height);
                break;
            case BlacklistAction::Type::Settings:
                formatted = actionLine.append("BLACKLIST_SETTINGS_CHANGED ") &&
                    actionLine.appendInteger(action.panelEnabled ? 1 : 0) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.matchAlertsEnabled ? 1 : 0) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.allowIdOnlyNicks ? 1 : 0) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.showWithClickGui ? 1 : 0) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.collapsed ? 1 : 0) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.panelOpacity) && actionLine.append(' ') &&
                    actionLine.appendInteger(action.panelColor);
                break;
            case BlacklistAction::Type::None: break;
            }
            if (formatted && m_ipc->sendLine(actionLine.view()))
                sentBlacklistActionRevision = blacklistActionRevision;
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
                const char teamColor = players[index].teamColor;
                if (!((teamColor >= '0' && teamColor <= '9') ||
                      (teamColor >= 'a' && teamColor <= 'f'))) {
                    continue;
                }
                const char teamPrefix[4]{
                    static_cast<char>(0xC2), static_cast<char>(0xA7),
                    teamColor, '\0'};
                FixedLine<160U> line;
                if (!line.append("PLAYER_FOUND ") ||
                    !appendPercentEncoded(line, players[index].name.data()) ||
                    !line.append(' ') || !appendPercentEncoded(line, teamPrefix) ||
                    !line.append(' ') ||
                    !appendPercentEncoded(line, players[index].uuid.data()) ||
                    !m_ipc->sendLine(line.view())) {
                    allSent = false;
                    break;
                }
            }
            if (allSent && matchActive && playerCount > 0U) {
                m_bindings->enqueueDebugChatLine(
                    "stats_query=dispatched players=" + std::to_string(playerCount));
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
    const std::uint32_t bits = packFeatures(settings);
    m_featureBits.store(bits, std::memory_order_release);
    m_bedDefenseRadius.store(std::clamp(settings.bedDefenseRadius, 3, 10),
                             std::memory_order_release);
    m_bedThreatRadius.store(std::clamp(settings.bedThreatRadius, 3, 32),
                            std::memory_order_release);
    m_bedDefenseHotkey.store(std::clamp(settings.bedDefenseHotkey, 8, 254),
                             std::memory_order_release);
    m_bedDefensePanelOpacity.store(std::clamp(settings.bedDefensePanelOpacity, 0, 100),
                                   std::memory_order_release);
    m_hypixelPanelHotkey.store(std::clamp(settings.hypixelPanelHotkey, 8, 254),
                               std::memory_order_release);
    m_hypixelPanelOpacity.store(std::clamp(settings.hypixelPanelOpacity, 0, 100),
                                std::memory_order_release);
    m_hypixelPanelScale.store(std::clamp(settings.hypixelPanelScale, 70, 160),
                              std::memory_order_release);
    m_hypixelPanelHeight.store(std::clamp(settings.hypixelPanelHeight, 60, 400),
                               std::memory_order_release);
    m_hypixelPanelX.store(std::clamp(settings.hypixelPanelX, -1, 1000),
                          std::memory_order_release);
    m_hypixelPanelY.store(std::clamp(settings.hypixelPanelY, -1, 1000),
                          std::memory_order_release);
    m_clickGuiLightTheme.store(settings.clickGuiLightTheme,
                               std::memory_order_release);
    m_playerEspColor.store(settings.playerEspColor & 0xFFFFFFU,
                           std::memory_order_release);
    m_bedEspColor.store(settings.bedEspColor & 0xFFFFFFU,
                        std::memory_order_release);
    m_bedDefensePanelColor.store(settings.bedDefensePanelColor & 0xFFFFFFU,
                                 std::memory_order_release);
    m_hypixelPanelColor.store(settings.hypixelPanelColor & 0xFFFFFFU,
                              std::memory_order_release);
    m_nametagPanelOpacity.store(std::clamp(settings.nametagPanelOpacity, 10, 100),
                                std::memory_order_release);
    m_nametagPanelColor.store(settings.nametagPanelColor & 0xFFFFFFU,
                              std::memory_order_release);
    m_clickGuiAccentColor.store(settings.clickGuiAccentColor & 0xFFFFFFU,
                                std::memory_order_release);
    m_hypixelRailColor.store(settings.hypixelRailColor & 0xFFFFFFU,
                             std::memory_order_release);
    m_hypixelRailOpacity.store(std::clamp(settings.hypixelRailOpacity, 0, 100),
                               std::memory_order_release);
    m_hypixelPanelFontIndex.store(std::clamp(settings.hypixelPanelFontIndex, 0, 3),
                                  std::memory_order_release);
    m_nametagRange.store(std::clamp(settings.nametagRange, 4, 128),
                         std::memory_order_release);
    m_nametagSizeIndex.store(std::clamp(settings.nametagSizeIndex, 0, 3),
                             std::memory_order_release);
    m_safewalkReleaseDelayMs.store(
        std::clamp(settings.safewalkReleaseDelayMs, 0, 750),
        std::memory_order_release);
    m_safewalkEdgeSensitivity.store(
        std::clamp(settings.safewalkEdgeSensitivity, 0, 100),
        std::memory_order_release);
    m_safewalkMinimumPitch.store(
        std::clamp(settings.safewalkMinimumPitch, -90, 90),
        std::memory_order_release);
    m_safewalkHotkey.store(std::clamp(settings.safewalkHotkey, 8, 254),
                            std::memory_order_release);
    m_flySpeedPercent.store(std::clamp(settings.flySpeedPercent, 10, 500),
                            std::memory_order_release);
    m_bhopAirSpeedPercent.store(
        std::clamp(settings.bhopAirSpeedPercent, 10, 300),
        std::memory_order_release);
    m_featureHotkeysPackedA.store(packFeatureHotkeys(settings, 0U),
                                  std::memory_order_release);
    m_featureHotkeysPackedB.store(packFeatureHotkeys(settings, 8U),
                                  std::memory_order_release);
    m_fireballEspEnabled.store(settings.fireballEspEnabled, std::memory_order_release);
    m_fireballEspFilled.store(settings.fireballEspFilled, std::memory_order_release);
    m_longJumpEnabled.store(settings.longJumpEnabled, std::memory_order_release);
    m_longJumpSpeedPercent.store(std::clamp(settings.longJumpSpeedPercent, 25, 250),
                                 std::memory_order_release);
    m_fireballEspColor.store(settings.fireballEspColor & 0xFFFFFFU,
                             std::memory_order_release);
    m_aimSlowdownPercent.store(std::clamp(settings.aimSlowdownPercent, 5, 95),
                               std::memory_order_release);
    m_aimSpeedPercent.store(std::clamp(settings.aimSpeedPercent, 1, 100),
                            std::memory_order_release);
    m_aimMinimumDistance.store(std::clamp(settings.aimMinimumDistance, 0, 64),
                               std::memory_order_release);
    m_aimMaximumDistance.store(std::clamp(settings.aimMaximumDistance,
        std::max(1, settings.aimMinimumDistance), 128), std::memory_order_release);
    m_aimFovDegrees.store(std::clamp(settings.aimFovDegrees, 1, 360),
                          std::memory_order_release);
    m_clickGuiWidthPercent.store(std::clamp(settings.clickGuiWidthPercent, 80, 150),
                                 std::memory_order_release);
    m_clickGuiHeightPercent.store(std::clamp(settings.clickGuiHeightPercent, 80, 150),
                                  std::memory_order_release);
    m_clickGuiOpacity.store(std::clamp(settings.clickGuiOpacity, 35, 100),
                            std::memory_order_release);
    m_featureExtraBits.store(packExtraFeatures(settings), std::memory_order_release);
    m_textGuiAlignment.store(std::clamp(settings.textGuiAlignment, 0, 2),
                             std::memory_order_release);
    m_localMobReach.store(std::clamp(settings.localMobReach, 3, 10),
                          std::memory_order_release);
    m_localAttackDelayMs.store(std::clamp(settings.localAttackDelayMs, 100, 1500),
                               std::memory_order_release);
    m_localVelocityPercent.store(std::clamp(settings.localVelocityPercent, 0, 100),
                                 std::memory_order_release);
    m_textGuiColor.store(settings.textGuiColor & 0xFFFFFFU,
                         std::memory_order_release);
    m_textGuiX.store(std::clamp(settings.textGuiX, -1, 1000),
                     std::memory_order_release);
    m_textGuiY.store(std::clamp(settings.textGuiY, -1, 1000),
                     std::memory_order_release);
    m_featureChangedBits.store(bits, std::memory_order_relaxed);
    m_featureChangedBedRadius.store(std::clamp(settings.bedDefenseRadius, 3, 10),
                                    std::memory_order_relaxed);
    m_featureChangedThreatRadius.store(std::clamp(settings.bedThreatRadius, 3, 32),
                                       std::memory_order_relaxed);
    m_featureChangedBedHotkey.store(std::clamp(settings.bedDefenseHotkey, 8, 254),
                                    std::memory_order_relaxed);
    m_featureChangedPanelOpacity.store(
        std::clamp(settings.bedDefensePanelOpacity, 0, 100),
        std::memory_order_relaxed);
    m_featureChangedHypixelHotkey.store(
        std::clamp(settings.hypixelPanelHotkey, 8, 254), std::memory_order_relaxed);
    m_featureChangedHypixelOpacity.store(
        std::clamp(settings.hypixelPanelOpacity, 0, 100), std::memory_order_relaxed);
    m_featureChangedHypixelScale.store(
        std::clamp(settings.hypixelPanelScale, 70, 160), std::memory_order_relaxed);
    m_featureChangedHypixelHeight.store(
        std::clamp(settings.hypixelPanelHeight, 60, 400), std::memory_order_relaxed);
    m_featureChangedHypixelX.store(
        std::clamp(settings.hypixelPanelX, -1, 1000), std::memory_order_relaxed);
    m_featureChangedHypixelY.store(
        std::clamp(settings.hypixelPanelY, -1, 1000), std::memory_order_relaxed);
    m_featureChangedClickGuiLightTheme.store(settings.clickGuiLightTheme,
                                              std::memory_order_relaxed);
    m_featureChangedPlayerColor.store(settings.playerEspColor & 0xFFFFFFU,
                                      std::memory_order_relaxed);
    m_featureChangedBedColor.store(settings.bedEspColor & 0xFFFFFFU,
                                   std::memory_order_relaxed);
    m_featureChangedPanelColor.store(settings.bedDefensePanelColor & 0xFFFFFFU,
                                     std::memory_order_relaxed);
    m_featureChangedHypixelColor.store(settings.hypixelPanelColor & 0xFFFFFFU,
                                       std::memory_order_relaxed);
    m_featureChangedNametagOpacity.store(
        std::clamp(settings.nametagPanelOpacity, 10, 100), std::memory_order_relaxed);
    m_featureChangedNametagColor.store(settings.nametagPanelColor & 0xFFFFFFU,
                                       std::memory_order_relaxed);
    m_featureChangedAccentColor.store(settings.clickGuiAccentColor & 0xFFFFFFU,
                                      std::memory_order_relaxed);
    m_featureChangedHypixelRailColor.store(settings.hypixelRailColor & 0xFFFFFFU,
                                           std::memory_order_relaxed);
    m_featureChangedHypixelRailOpacity.store(
        std::clamp(settings.hypixelRailOpacity, 0, 100),
        std::memory_order_relaxed);
    m_featureChangedHypixelFontIndex.store(
        std::clamp(settings.hypixelPanelFontIndex, 0, 3), std::memory_order_relaxed);
    m_featureChangedNametagRange.store(
        std::clamp(settings.nametagRange, 4, 128), std::memory_order_relaxed);
    m_featureChangedNametagSizeIndex.store(
        std::clamp(settings.nametagSizeIndex, 0, 3), std::memory_order_relaxed);
    m_featureChangedSafewalkReleaseDelayMs.store(
        std::clamp(settings.safewalkReleaseDelayMs, 0, 750),
        std::memory_order_relaxed);
    m_featureChangedSafewalkEdgeSensitivity.store(
        std::clamp(settings.safewalkEdgeSensitivity, 0, 100),
        std::memory_order_relaxed);
    m_featureChangedSafewalkMinimumPitch.store(
        std::clamp(settings.safewalkMinimumPitch, -90, 90),
        std::memory_order_relaxed);
    m_featureChangedSafewalkHotkey.store(
        std::clamp(settings.safewalkHotkey, 8, 254),
        std::memory_order_relaxed);
    m_featureChangedFlySpeedPercent.store(
        std::clamp(settings.flySpeedPercent, 10, 500),
        std::memory_order_relaxed);
    m_featureChangedBhopAirSpeedPercent.store(
        std::clamp(settings.bhopAirSpeedPercent, 10, 300),
        std::memory_order_relaxed);
    m_featureChangedHotkeysPackedA.store(packFeatureHotkeys(settings, 0U),
                                         std::memory_order_relaxed);
    m_featureChangedHotkeysPackedB.store(packFeatureHotkeys(settings, 8U),
                                         std::memory_order_relaxed);
    m_featureChangedFireballEspEnabled.store(settings.fireballEspEnabled,
                                              std::memory_order_relaxed);
    m_featureChangedFireballEspFilled.store(settings.fireballEspFilled,
                                             std::memory_order_relaxed);
    m_featureChangedLongJumpEnabled.store(settings.longJumpEnabled,
                                           std::memory_order_relaxed);
    m_featureChangedLongJumpSpeedPercent.store(
        std::clamp(settings.longJumpSpeedPercent, 25, 250),
        std::memory_order_relaxed);
    m_featureChangedFireballEspColor.store(settings.fireballEspColor & 0xFFFFFFU,
                                            std::memory_order_relaxed);
    m_featureChangedAimSlowdownPercent.store(
        std::clamp(settings.aimSlowdownPercent, 5, 95),
        std::memory_order_relaxed);
    m_featureChangedAimSpeedPercent.store(
        std::clamp(settings.aimSpeedPercent, 1, 100),
        std::memory_order_relaxed);
    m_featureChangedAimMinimumDistance.store(
        std::clamp(settings.aimMinimumDistance, 0, 64), std::memory_order_relaxed);
    m_featureChangedAimMaximumDistance.store(std::clamp(
        settings.aimMaximumDistance, std::max(1, settings.aimMinimumDistance), 128),
        std::memory_order_relaxed);
    m_featureChangedAimFovDegrees.store(
        std::clamp(settings.aimFovDegrees, 1, 360), std::memory_order_relaxed);
    m_featureChangedClickGuiWidthPercent.store(
        std::clamp(settings.clickGuiWidthPercent, 80, 150), std::memory_order_relaxed);
    m_featureChangedClickGuiHeightPercent.store(
        std::clamp(settings.clickGuiHeightPercent, 80, 150), std::memory_order_relaxed);
    m_featureChangedClickGuiOpacity.store(
        std::clamp(settings.clickGuiOpacity, 35, 100), std::memory_order_relaxed);
    m_featureChangedExtraBits.store(packExtraFeatures(settings),
                                    std::memory_order_relaxed);
    m_featureChangedTextGuiAlignment.store(
        std::clamp(settings.textGuiAlignment, 0, 2), std::memory_order_relaxed);
    m_featureChangedLocalMobReach.store(
        std::clamp(settings.localMobReach, 3, 10), std::memory_order_relaxed);
    m_featureChangedLocalAttackDelayMs.store(
        std::clamp(settings.localAttackDelayMs, 100, 1500),
        std::memory_order_relaxed);
    m_featureChangedLocalVelocityPercent.store(
        std::clamp(settings.localVelocityPercent, 0, 100),
        std::memory_order_relaxed);
    m_featureChangedTextGuiColor.store(settings.textGuiColor & 0xFFFFFFU,
                                       std::memory_order_relaxed);
    m_featureChangedTextGuiX.store(std::clamp(settings.textGuiX, -1, 1000),
                                   std::memory_order_relaxed);
    m_featureChangedTextGuiY.store(std::clamp(settings.textGuiY, -1, 1000),
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

void AgentRuntime::queueBlacklistAction(const BlacklistAction& action) noexcept
{
    ::AcquireSRWLockExclusive(&m_blacklistActionLock);
    m_blacklistAction = action;
    ::ReleaseSRWLockExclusive(&m_blacklistActionLock);
    m_blacklistActionRevision.fetch_add(1U, std::memory_order_release);
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
        std::array<std::string, 32U> tokens{};
        std::string trailing;
        FeatureSettings settings{};
        std::array<bool, 32U> values{};
        int defenseRadius = 0;
        int threatRadius = 0;
        int bedHotkey = 0;
        int panelOpacity = 0;
        int hypixelHotkey = 0;
        int hypixelOpacity = 0;
        int hypixelScale = 0;
        int hypixelX = 0;
        int hypixelY = 0;
        int clickGuiTheme = 0;
        std::uint32_t playerColor = 0U;
        std::uint32_t bedColor = 0U;
        std::uint32_t panelColor = 0U;
        std::uint32_t hypixelColor = 0U;
        int hypixelHeight = 0;
        int nametagOpacity = 0;
        std::uint32_t nametagColor = 0U;
        std::uint32_t accentColor = 0U;
        int hypixelFontIndex = 0;
        int nametagRange = 0;
        int nametagSizeIndex = 0;
        std::uint32_t hypixelRailColor = 0U;
        int hypixelRailOpacity = 0;
        int safewalkReleaseDelayMs = 0;
        int safewalkEdgeSensitivity = 0;
        int safewalkMinimumPitch = 0;
        int safewalkHotkey = 0;
        int flySpeedPercent = 0;
        int aimSlowdownPercent = 0;
        int aimSpeedPercent = 0;
        std::uint32_t textGuiColor = 0U;
        int textGuiX = 0;
        int textGuiY = 0;
        int bhopAirSpeedPercent = 0;
        std::uint64_t hotkeysPackedA = 0U;
        std::uint64_t hotkeysPackedB = 0U;
        std::string fireballEnabledToken;
        std::string fireballFilledToken;
        std::string longJumpEnabledToken;
        int longJumpSpeedPercent = 0;
        std::uint32_t fireballEspColor = 0U;
        int aimMinimumDistance = 0;
        int aimMaximumDistance = 0;
        int aimFovDegrees = 0;
        int clickGuiWidthPercent = 0;
        int clickGuiHeightPercent = 0;
        int clickGuiOpacity = 0;
        std::uint32_t extraBits = 0U;
        int textGuiAlignment = 0;
        int localMobReach = 0;
        int localAttackDelayMs = 0;
        int localVelocityPercent = 0;
        bool featureTokensRead = true;
        for (std::string& token : tokens) {
            if (!(stream >> token)) {
                featureTokensRead = false;
                break;
            }
        }
        if (!featureTokensRead || !(stream
               >> defenseRadius >> threatRadius >> bedHotkey >> panelOpacity
               >> hypixelHotkey >> hypixelOpacity
               >> hypixelScale >> hypixelX >> hypixelY >> clickGuiTheme
               >> playerColor >> bedColor >> panelColor >> hypixelColor
               >> hypixelHeight >> nametagOpacity >> nametagColor >> accentColor
               >> hypixelFontIndex >> nametagRange >> nametagSizeIndex
               >> hypixelRailColor >> hypixelRailOpacity
               >> safewalkReleaseDelayMs >> safewalkEdgeSensitivity
               >> safewalkMinimumPitch >> safewalkHotkey >> flySpeedPercent
               >> aimSlowdownPercent >> aimSpeedPercent >> textGuiColor
               >> textGuiX >> textGuiY >> bhopAirSpeedPercent
               >> hotkeysPackedA >> hotkeysPackedB
               >> fireballEnabledToken >> fireballFilledToken
               >> longJumpEnabledToken >> longJumpSpeedPercent
               >> fireballEspColor >> aimMinimumDistance
               >> aimMaximumDistance >> aimFovDegrees
               >> clickGuiWidthPercent >> clickGuiHeightPercent
               >> clickGuiOpacity >> extraBits >> textGuiAlignment
               >> localMobReach >> localAttackDelayMs
               >> localVelocityPercent) ||
            (stream >> trailing)) {
            (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE expected-thirty-two-flags-and-layout");
            return true;
        }

        for (std::size_t index = 0U; index < tokens.size(); ++index) {
            if (!parseFlag(tokens[index], values[index])) {
                (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE expected-thirty-two-flags-and-layout");
                return true;
            }
        }
        if (defenseRadius < 3 || defenseRadius > 10 ||
            threatRadius < 3 || threatRadius > 32 ||
            bedHotkey < 8 || bedHotkey > 254 ||
            panelOpacity < 0 || panelOpacity > 100 ||
            hypixelHotkey < 8 || hypixelHotkey > 254 ||
            hypixelOpacity < 0 || hypixelOpacity > 100 ||
            hypixelScale < 70 || hypixelScale > 160 ||
            hypixelHeight < 60 || hypixelHeight > 400 ||
            hypixelX < -1 || hypixelX > 1000 ||
            hypixelY < -1 || hypixelY > 1000 ||
            clickGuiTheme < 0 || clickGuiTheme > 1 ||
            playerColor > 0xFFFFFFU || bedColor > 0xFFFFFFU ||
            panelColor > 0xFFFFFFU || hypixelColor > 0xFFFFFFU ||
            nametagOpacity < 10 || nametagOpacity > 100 ||
            nametagColor > 0xFFFFFFU || accentColor > 0xFFFFFFU ||
            hypixelRailColor > 0xFFFFFFU ||
            hypixelRailOpacity < 0 || hypixelRailOpacity > 100 ||
            safewalkReleaseDelayMs < 0 || safewalkReleaseDelayMs > 750 ||
            safewalkEdgeSensitivity < 0 || safewalkEdgeSensitivity > 100 ||
            safewalkMinimumPitch < -90 || safewalkMinimumPitch > 90 ||
            safewalkHotkey < 8 || safewalkHotkey > 254 ||
            flySpeedPercent < 10 || flySpeedPercent > 500 ||
            aimSlowdownPercent < 5 || aimSlowdownPercent > 95 ||
            aimSpeedPercent < 1 || aimSpeedPercent > 100 ||
            aimMinimumDistance < 0 || aimMinimumDistance > 64 ||
            aimMaximumDistance < std::max(1, aimMinimumDistance) ||
            aimMaximumDistance > 128 ||
            aimFovDegrees < 1 || aimFovDegrees > 360 ||
            clickGuiWidthPercent < 80 || clickGuiWidthPercent > 150 ||
            clickGuiHeightPercent < 80 || clickGuiHeightPercent > 150 ||
            clickGuiOpacity < 35 || clickGuiOpacity > 100 ||
            textGuiColor > 0xFFFFFFU || textGuiX < -1 || textGuiX > 1000 ||
            textGuiY < -1 || textGuiY > 1000 ||
            bhopAirSpeedPercent < 10 || bhopAirSpeedPercent > 300 ||
            hypixelFontIndex < 0 || hypixelFontIndex > 3 ||
            nametagRange < 4 || nametagRange > 128 ||
            nametagSizeIndex < 0 || nametagSizeIndex > 3 ||
            extraBits > 0xFFU || textGuiAlignment < 0 || textGuiAlignment > 2 ||
            localMobReach < 3 || localMobReach > 10 ||
            localAttackDelayMs < 100 || localAttackDelayMs > 1500 ||
            localVelocityPercent < 0 || localVelocityPercent > 100) {
            (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE invalid-radius-bind-opacity-or-color");
            return true;
        }
        bool fireballEnabled = false;
        bool fireballFilled = false;
        bool longJumpEnabled = false;
        if (!parseFlag(fireballEnabledToken, fireballEnabled) ||
            !parseFlag(fireballFilledToken, fireballFilled) ||
            !parseFlag(longJumpEnabledToken, longJumpEnabled) ||
            longJumpSpeedPercent < 25 || longJumpSpeedPercent > 250 ||
            fireballEspColor > 0xFFFFFFU) {
            (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE invalid-local-feature");
            return true;
        }
        settings.espEnabled = values[0];
        settings.entityEspEnabled = values[1];
        settings.bedEspEnabled = values[2];
        settings.labelsEnabled = values[3];
        settings.hypixelPanelEnabled = values[4];
        settings.bedThreatAlertsEnabled = values[5];
        settings.bedDefensePanelEnabled = values[6];
        settings.entityEspPlayersOnly = values[7];
        settings.bedAutoRefreshEnabled = values[8];
        settings.bedEspFilled = values[9];
        settings.debugChatEnabled = values[10];
        settings.showOwnBedDefenseInfo = values[11];
        settings.showTeammateBoxes = values[12];
        settings.bedDefenseHoldToShow = values[13];
        settings.bedDefensePerspectiveScale = values[14];
        settings.hypixelPanelHoldToShow = values[15];
        settings.nametagEnabled = values[16];
        settings.nametagSidePlacement = values[17];
        settings.enemyItemIndicatorsEnabled = values[18];
        settings.showTeammateNametags = values[19];
        settings.nametagNearbyEnemiesOnly = values[20];
        settings.nametagTeamPulse = values[21];
        settings.showTeammateArrows = values[22];
        settings.safewalkEnabled = values[23];
        settings.scaffoldEnabled = values[24];
        settings.flyEnabled = values[25];
        settings.bhopEnabled = values[26];
        settings.bhopAutoJump = values[27];
        settings.aimAssistEnabled = values[28];
        settings.aimSlowdownMode = values[29];
        settings.textGuiEnabled = values[30];
        settings.allowHypixelMovement = values[31];
        settings.bedDefenseRadius = defenseRadius;
        settings.bedThreatRadius = threatRadius;
        settings.bedDefenseHotkey = bedHotkey;
        settings.bedDefensePanelOpacity = panelOpacity;
        settings.hypixelPanelHotkey = hypixelHotkey;
        settings.hypixelPanelOpacity = hypixelOpacity;
        settings.hypixelPanelScale = hypixelScale;
        settings.hypixelPanelHeight = hypixelHeight;
        settings.hypixelPanelX = hypixelX;
        settings.hypixelPanelY = hypixelY;
        settings.clickGuiLightTheme = clickGuiTheme != 0;
        settings.playerEspColor = playerColor;
        settings.bedEspColor = bedColor;
        settings.bedDefensePanelColor = panelColor;
        settings.hypixelPanelColor = hypixelColor;
        settings.nametagPanelOpacity = nametagOpacity;
        settings.nametagPanelColor = nametagColor;
        settings.clickGuiAccentColor = accentColor;
        settings.hypixelPanelFontIndex = hypixelFontIndex;
        settings.nametagRange = nametagRange;
        settings.nametagSizeIndex = nametagSizeIndex;
        settings.hypixelRailColor = hypixelRailColor;
        settings.hypixelRailOpacity = hypixelRailOpacity;
        settings.safewalkReleaseDelayMs = safewalkReleaseDelayMs;
        settings.safewalkEdgeSensitivity = safewalkEdgeSensitivity;
        settings.safewalkMinimumPitch = safewalkMinimumPitch;
        settings.safewalkHotkey = safewalkHotkey;
        settings.flySpeedPercent = flySpeedPercent;
        settings.aimSlowdownPercent = aimSlowdownPercent;
        settings.aimSpeedPercent = aimSpeedPercent;
        settings.textGuiColor = textGuiColor;
        settings.textGuiX = textGuiX;
        settings.textGuiY = textGuiY;
        settings.bhopAirSpeedPercent = bhopAirSpeedPercent;
        for (std::size_t index = 0U; index < settings.featureHotkeys.size(); ++index) {
            const std::uint64_t packed = index < 8U ? hotkeysPackedA : hotkeysPackedB;
            const std::size_t offset = index < 8U ? index : index - 8U;
            const int key = static_cast<int>((packed >> (offset * 8U)) & 0xFFU);
            if ((key > 0 && key < 8) || key > 254) {
                (void)m_ipc->sendLine("ERROR BAD_FEATURE_STATE invalid-feature-hotkey");
                return true;
            }
            settings.featureHotkeys[index] = key;
        }
        if (settings.featureHotkeys[4U] == 0)
            settings.featureHotkeys[4U] = safewalkHotkey;
        settings.safewalkHotkey = settings.featureHotkeys[4U];
        settings.fireballEspEnabled = fireballEnabled;
        settings.fireballEspFilled = fireballFilled;
        settings.longJumpEnabled = longJumpEnabled;
        settings.longJumpSpeedPercent = longJumpSpeedPercent;
        settings.fireballEspColor = fireballEspColor;
        settings.aimMinimumDistance = aimMinimumDistance;
        settings.aimMaximumDistance = aimMaximumDistance;
        settings.aimFovDegrees = aimFovDegrees;
        settings.clickGuiWidthPercent = clickGuiWidthPercent;
        settings.clickGuiHeightPercent = clickGuiHeightPercent;
        settings.clickGuiOpacity = clickGuiOpacity;
        settings.aimNearestPriority = (extraBits & 0x01U) != 0U;
        settings.textGuiVerticalLine = (extraBits & 0x02U) != 0U;
        settings.knockbackPredictionEnabled = (extraBits & 0x04U) != 0U;
        settings.bowPredictionEnabled = (extraBits & 0x08U) != 0U;
        settings.localMobAuraEnabled = (extraBits & 0x10U) != 0U;
        settings.localVelocityEnabled = (extraBits & 0x20U) != 0U;
        settings.scaffoldSameLayerOnly = (extraBits & 0x40U) != 0U;
        settings.fullscreenImeFixEnabled = (extraBits & 0x80U) != 0U;
        settings.textGuiAlignment = textGuiAlignment;
        settings.localMobReach = localMobReach;
        settings.localAttackDelayMs = localAttackDelayMs;
        settings.localVelocityPercent = localVelocityPercent;
        m_featureBits.store(packFeatures(settings), std::memory_order_release);
        m_bedDefenseRadius.store(defenseRadius, std::memory_order_release);
        m_bedThreatRadius.store(threatRadius, std::memory_order_release);
        m_bedDefenseHotkey.store(bedHotkey, std::memory_order_release);
        m_bedDefensePanelOpacity.store(panelOpacity, std::memory_order_release);
        m_hypixelPanelHotkey.store(hypixelHotkey, std::memory_order_release);
        m_hypixelPanelOpacity.store(hypixelOpacity, std::memory_order_release);
        m_hypixelPanelScale.store(hypixelScale, std::memory_order_release);
        m_hypixelPanelHeight.store(hypixelHeight, std::memory_order_release);
        m_hypixelPanelX.store(hypixelX, std::memory_order_release);
        m_hypixelPanelY.store(hypixelY, std::memory_order_release);
        m_clickGuiLightTheme.store(clickGuiTheme != 0, std::memory_order_release);
        m_playerEspColor.store(playerColor, std::memory_order_release);
        m_bedEspColor.store(bedColor, std::memory_order_release);
        m_bedDefensePanelColor.store(panelColor, std::memory_order_release);
        m_hypixelPanelColor.store(hypixelColor, std::memory_order_release);
        m_nametagPanelOpacity.store(nametagOpacity, std::memory_order_release);
        m_nametagPanelColor.store(nametagColor, std::memory_order_release);
        m_clickGuiAccentColor.store(accentColor, std::memory_order_release);
        m_hypixelRailColor.store(hypixelRailColor, std::memory_order_release);
        m_hypixelRailOpacity.store(hypixelRailOpacity, std::memory_order_release);
        m_hypixelPanelFontIndex.store(hypixelFontIndex, std::memory_order_release);
        m_nametagRange.store(nametagRange, std::memory_order_release);
        m_nametagSizeIndex.store(nametagSizeIndex, std::memory_order_release);
        m_safewalkReleaseDelayMs.store(safewalkReleaseDelayMs,
                                       std::memory_order_release);
        m_safewalkEdgeSensitivity.store(safewalkEdgeSensitivity,
                                         std::memory_order_release);
        m_safewalkMinimumPitch.store(safewalkMinimumPitch,
                                     std::memory_order_release);
        m_safewalkHotkey.store(safewalkHotkey, std::memory_order_release);
        m_flySpeedPercent.store(flySpeedPercent, std::memory_order_release);
        m_aimSlowdownPercent.store(aimSlowdownPercent,
                                   std::memory_order_release);
        m_aimSpeedPercent.store(aimSpeedPercent, std::memory_order_release);
        m_textGuiColor.store(textGuiColor, std::memory_order_release);
        m_textGuiX.store(textGuiX, std::memory_order_release);
        m_textGuiY.store(textGuiY, std::memory_order_release);
        m_bhopAirSpeedPercent.store(bhopAirSpeedPercent,
                                    std::memory_order_release);
        m_featureHotkeysPackedA.store(packFeatureHotkeys(settings, 0U),
                                      std::memory_order_release);
        m_featureHotkeysPackedB.store(packFeatureHotkeys(settings, 8U),
                                      std::memory_order_release);
        m_fireballEspEnabled.store(fireballEnabled, std::memory_order_release);
        m_fireballEspFilled.store(fireballFilled, std::memory_order_release);
        m_longJumpEnabled.store(longJumpEnabled, std::memory_order_release);
        m_longJumpSpeedPercent.store(longJumpSpeedPercent,
                                     std::memory_order_release);
        m_fireballEspColor.store(fireballEspColor, std::memory_order_release);
        m_aimMinimumDistance.store(aimMinimumDistance, std::memory_order_release);
        m_aimMaximumDistance.store(aimMaximumDistance, std::memory_order_release);
        m_aimFovDegrees.store(aimFovDegrees, std::memory_order_release);
        m_clickGuiWidthPercent.store(clickGuiWidthPercent, std::memory_order_release);
        m_clickGuiHeightPercent.store(clickGuiHeightPercent, std::memory_order_release);
        m_clickGuiOpacity.store(clickGuiOpacity, std::memory_order_release);
        m_featureExtraBits.store(extraBits, std::memory_order_release);
        m_textGuiAlignment.store(textGuiAlignment, std::memory_order_release);
        m_localMobReach.store(localMobReach, std::memory_order_release);
        m_localAttackDelayMs.store(localAttackDelayMs, std::memory_order_release);
        m_localVelocityPercent.store(localVelocityPercent, std::memory_order_release);
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
    if (command == "BLACKLIST_RESET") {
        std::string trailing;
        if (stream >> trailing) return true;
        ::AcquireSRWLockExclusive(&m_blacklistLock);
        m_blacklistSnapshot = {};
        ::ReleaseSRWLockExclusive(&m_blacklistLock);
        m_blacklistSyncInProgress = true;
        return true;
    }
    if (command == "BLACKLIST_SETTINGS") {
        std::string enabledToken, alertsToken, idOnlyToken, showToken,
                    collapsedToken, trailing;
        int opacity = 0, x = -1, y = -1, width = 100, height = 100;
        std::uint32_t color = 0U;
        bool enabled = false, alerts = false, allowIdOnly = false;
        bool showWithClickGui = false, collapsed = false;
        if (!(stream >> enabledToken >> alertsToken >> idOnlyToken >> showToken
                     >> collapsedToken >> opacity >> color
                     >> x >> y >> width >> height) || (stream >> trailing) ||
            !parseFlag(enabledToken, enabled) || !parseFlag(alertsToken, alerts) ||
            !parseFlag(idOnlyToken, allowIdOnly) ||
            !parseFlag(showToken, showWithClickGui) ||
            !parseFlag(collapsedToken, collapsed) || opacity < 0 || opacity > 100 ||
            color > 0xFFFFFFU || x < -1 || x > 1000 || y < -1 || y > 1000 ||
            width < 60 || width > 180 || height < 60 || height > 300) return true;
        ::AcquireSRWLockExclusive(&m_blacklistLock);
        m_blacklistSnapshot.panelEnabled = enabled;
        m_blacklistSnapshot.matchAlertsEnabled = alerts;
        m_blacklistSnapshot.allowIdOnlyNicks = allowIdOnly;
        m_blacklistSnapshot.showWithClickGui = showWithClickGui;
        m_blacklistSnapshot.collapsed = collapsed;
        m_blacklistSnapshot.panelOpacity = opacity;
        m_blacklistSnapshot.panelColor = color;
        m_blacklistSnapshot.panelX = x;
        m_blacklistSnapshot.panelY = y;
        m_blacklistSnapshot.panelWidth = width;
        m_blacklistSnapshot.panelHeight = height;
        ::ReleaseSRWLockExclusive(&m_blacklistLock);
        if (!m_blacklistSyncInProgress)
            m_blacklistRevision.fetch_add(1U, std::memory_order_release);
        return true;
    }
    if (command == "BLACKLIST_PRESET") {
        std::string valueToken, trailing;
        std::array<char, 81U> value{};
        if (!(stream >> valueToken) || (stream >> trailing) ||
            !percentDecode(valueToken, value) || value[0U] == '\0') return true;
        ::AcquireSRWLockExclusive(&m_blacklistLock);
        if (m_blacklistSnapshot.presetCount < m_blacklistSnapshot.presets.size())
            m_blacklistSnapshot.presets[m_blacklistSnapshot.presetCount++] = value;
        ::ReleaseSRWLockExclusive(&m_blacklistLock);
        if (!m_blacklistSyncInProgress)
            m_blacklistRevision.fetch_add(1U, std::memory_order_release);
        return true;
    }
    if (command == "BLACKLIST_ENTRY") {
        std::string keyToken, uuidToken, nameToken, reasonToken, nickToken,
                    idOnlyToken, warningToken, faceToken, trailing;
        std::int64_t addedAt = 0;
        BlacklistEntry entry{};
        bool nick = false, idOnly = false, warning = false;
        if (!(stream >> keyToken >> uuidToken >> nameToken >> reasonToken >> addedAt
                     >> nickToken >> idOnlyToken >> warningToken >> faceToken) ||
            (stream >> trailing) || !parseFlag(nickToken, nick) ||
            !parseFlag(idOnlyToken, idOnly) || !parseFlag(warningToken, warning) ||
            !percentDecode(keyToken, entry.key) ||
            !percentDecode(uuidToken, entry.uuid) ||
            !percentDecode(nameToken, entry.name) ||
            !percentDecode(reasonToken, entry.reason) ||
            !percentDecode(faceToken, entry.facePath) || entry.key[0U] == '\0' ||
            entry.name[0U] == '\0') return true;
        entry.addedAt = addedAt;
        entry.nick = nick;
        entry.idOnly = idOnly;
        entry.warnOnEncounter = warning;
        ::AcquireSRWLockExclusive(&m_blacklistLock);
        std::uint32_t index = m_blacklistSnapshot.count;
        for (std::uint32_t candidate = 0U;
             candidate < m_blacklistSnapshot.count; ++candidate) {
            if (::_stricmp(m_blacklistSnapshot.entries[candidate].key.data(),
                           entry.key.data()) == 0) {
                index = candidate;
                break;
            }
        }
        if (index < m_blacklistSnapshot.entries.size()) {
            m_blacklistSnapshot.entries[index] = entry;
            if (index == m_blacklistSnapshot.count) ++m_blacklistSnapshot.count;
        }
        ::ReleaseSRWLockExclusive(&m_blacklistLock);
        if (!m_blacklistSyncInProgress)
            m_blacklistRevision.fetch_add(1U, std::memory_order_release);
        return true;
    }
    if (command == "BLACKLIST_REMOVE" || command == "BLACKLIST_WARNING") {
        std::string keyToken, valueToken, trailing;
        std::array<char, 50U> key{};
        const bool warningCommand = command == "BLACKLIST_WARNING";
        bool warning = false;
        if (!(stream >> keyToken) || !percentDecode(keyToken, key) ||
            (warningCommand && (!(stream >> valueToken) ||
                                !parseFlag(valueToken, warning))) ||
            (stream >> trailing)) return true;
        ::AcquireSRWLockExclusive(&m_blacklistLock);
        for (std::uint32_t index = 0U; index < m_blacklistSnapshot.count; ++index) {
            if (::_stricmp(m_blacklistSnapshot.entries[index].key.data(), key.data()) != 0)
                continue;
            if (warningCommand) {
                m_blacklistSnapshot.entries[index].warnOnEncounter = warning;
            } else {
                for (std::uint32_t move = index + 1U;
                     move < m_blacklistSnapshot.count; ++move) {
                    m_blacklistSnapshot.entries[move - 1U] =
                        m_blacklistSnapshot.entries[move];
                }
                --m_blacklistSnapshot.count;
                m_blacklistSnapshot.entries[m_blacklistSnapshot.count] = {};
            }
            break;
        }
        ::ReleaseSRWLockExclusive(&m_blacklistLock);
        m_blacklistRevision.fetch_add(1U, std::memory_order_release);
        return true;
    }
    if (command == "BLACKLIST_SYNC_END") {
        std::string trailing;
        if (!(stream >> trailing)) {
            m_blacklistSyncInProgress = false;
            m_blacklistRevision.fetch_add(1U, std::memory_order_release);
        }
        return true;
    }
    if (command == "STATS") {
        std::string playerToken;
        std::string teamToken;
        std::string trailing;
        PlayerStatsEntry entry{};
        if (!(stream >> playerToken >> teamToken >> entry.stars >> entry.fkdr >>
              entry.wlr >> entry.bblr >> entry.wins >> entry.finalKills >>
              entry.bedsBroken >> entry.winStreak >> entry.level) ||
            (stream >> trailing) || !std::isfinite(entry.fkdr) ||
            !std::isfinite(entry.wlr) || !std::isfinite(entry.bblr) ||
            entry.stars < 0 || entry.stars > 100000 || entry.fkdr < 0.0 ||
            entry.fkdr > 1000000.0 || entry.wlr < 0.0 || entry.wlr > 1000000.0 ||
            entry.bblr < 0.0 || entry.bblr > 1000000.0 || entry.wins < 0 ||
            entry.finalKills < 0 || entry.bedsBroken < 0 ||
            entry.winStreak < 0 || entry.winStreak > 1000000 ||
            entry.level < 0 || entry.level > 100000 ||
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
        m_bindings->enqueueDebugChatLine(
            "stats_ready player=" + std::string(playerName) +
            " stars=" + std::to_string(entry.stars) +
            " fkdr=" + std::to_string(entry.fkdr) +
            " wlr=" + std::to_string(entry.wlr) +
            " beds=" + std::to_string(entry.bedsBroken) +
            " level=" + std::to_string(entry.level));
        return true;
    }
    if (command == "STATS_ERROR") {
        std::string playerToken;
        std::string reasonToken;
        std::string trailing;
        PlayerStatsEntry entry{};
        if (!(stream >> playerToken >> reasonToken) || (stream >> trailing) ||
            !percentDecode(playerToken, entry.name) ||
            !percentDecode(reasonToken, entry.status)) {
            (void)m_ipc->sendLine("ERROR BAD_STATS_ERROR invalid-payload");
            return true;
        }
        const std::string_view playerName(entry.name.data());
        const bool validName = !playerName.empty() && playerName.size() <= 16U &&
            std::all_of(playerName.begin(), playerName.end(), [](const char character) noexcept {
                return (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') || character == '_';
            });
        if (!validName || entry.status[0U] == '\0') {
            (void)m_ipc->sendLine("ERROR BAD_STATS_ERROR invalid-player-or-reason");
            return true;
        }
        entry.failed = true;
        ::AcquireSRWLockExclusive(&m_playerStatsLock);
        if (!m_playerStats.contains(std::string(playerName)) &&
            m_playerStats.size() >= 256U) {
            m_playerStats.erase(m_playerStats.begin());
        }
        m_playerStats[std::string(playerName)] = entry;
        ::ReleaseSRWLockExclusive(&m_playerStatsLock);
        m_bindings->enqueueDebugChatLine(
            "stats_error player=" + std::string(playerName) +
            " reason=" + std::string(entry.status.data()));
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
    FeatureSettings activeFeatures = unpackFeatures(
        m_featureBits.load(std::memory_order_acquire),
        m_bedDefenseRadius.load(std::memory_order_acquire),
        m_bedThreatRadius.load(std::memory_order_acquire),
        m_bedDefenseHotkey.load(std::memory_order_acquire),
        m_bedDefensePanelOpacity.load(std::memory_order_acquire),
        m_hypixelPanelHotkey.load(std::memory_order_acquire),
        m_hypixelPanelOpacity.load(std::memory_order_acquire),
        m_hypixelPanelScale.load(std::memory_order_acquire),
        m_hypixelPanelX.load(std::memory_order_acquire),
        m_hypixelPanelY.load(std::memory_order_acquire),
        m_clickGuiLightTheme.load(std::memory_order_acquire),
        m_playerEspColor.load(std::memory_order_acquire),
        m_bedEspColor.load(std::memory_order_acquire),
        m_bedDefensePanelColor.load(std::memory_order_acquire),
        m_hypixelPanelColor.load(std::memory_order_acquire),
        m_hypixelPanelHeight.load(std::memory_order_acquire),
        m_nametagPanelOpacity.load(std::memory_order_acquire),
        m_nametagPanelColor.load(std::memory_order_acquire),
        m_clickGuiAccentColor.load(std::memory_order_acquire),
        m_hypixelPanelFontIndex.load(std::memory_order_acquire),
        m_nametagRange.load(std::memory_order_acquire),
        m_nametagSizeIndex.load(std::memory_order_acquire),
        m_hypixelRailColor.load(std::memory_order_acquire),
        m_hypixelRailOpacity.load(std::memory_order_acquire),
        m_safewalkReleaseDelayMs.load(std::memory_order_acquire),
        m_safewalkEdgeSensitivity.load(std::memory_order_acquire),
        m_safewalkMinimumPitch.load(std::memory_order_acquire),
        m_safewalkHotkey.load(std::memory_order_acquire),
        m_flySpeedPercent.load(std::memory_order_acquire),
        m_aimSlowdownPercent.load(std::memory_order_acquire),
        m_aimSpeedPercent.load(std::memory_order_acquire),
        m_textGuiColor.load(std::memory_order_acquire),
        m_textGuiX.load(std::memory_order_acquire),
        m_textGuiY.load(std::memory_order_acquire),
        m_bhopAirSpeedPercent.load(std::memory_order_acquire),
        m_featureHotkeysPackedA.load(std::memory_order_acquire),
        m_featureHotkeysPackedB.load(std::memory_order_acquire),
        m_fireballEspEnabled.load(std::memory_order_acquire),
        m_fireballEspFilled.load(std::memory_order_acquire),
        m_longJumpEnabled.load(std::memory_order_acquire),
        m_longJumpSpeedPercent.load(std::memory_order_acquire),
        m_fireballEspColor.load(std::memory_order_acquire),
        m_aimMinimumDistance.load(std::memory_order_acquire),
        m_aimMaximumDistance.load(std::memory_order_acquire),
        m_aimFovDegrees.load(std::memory_order_acquire),
        m_clickGuiWidthPercent.load(std::memory_order_acquire),
        m_clickGuiHeightPercent.load(std::memory_order_acquire),
        m_clickGuiOpacity.load(std::memory_order_acquire),
        m_featureExtraBits.load(std::memory_order_acquire),
        m_textGuiAlignment.load(std::memory_order_acquire),
        m_localMobReach.load(std::memory_order_acquire),
        m_localAttackDelayMs.load(std::memory_order_acquire),
        m_localVelocityPercent.load(std::memory_order_acquire));
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

    // Server guard is evaluated from currentServerData.serverIP and therefore
    // remains active in lobbies and during respawn. The explicit override is
    // persisted separately and never inferred from a feature hotkey.
    if (snapshot.hypixelServer && !activeFeatures.allowHypixelMovement &&
        (activeFeatures.scaffoldEnabled || activeFeatures.flyEnabled ||
         activeFeatures.bhopEnabled)) {
        activeFeatures.scaffoldEnabled = false;
        activeFeatures.flyEnabled = false;
        activeFeatures.bhopEnabled = false;
        queueFeatureChanged(activeFeatures);
        m_bindings->enqueueDebugChatLine(
            "[Movement Guard] Fly/BHop/Scaffold were disabled on Hypixel.");
    }

    // New local diagnostics are fail-closed. A warning cannot be used as an
    // override: only an integrated single-player server may execute them.
    if (!snapshot.integratedSinglePlayer &&
        (activeFeatures.longJumpEnabled || activeFeatures.fireballEspEnabled ||
         activeFeatures.localMobAuraEnabled ||
         activeFeatures.localVelocityEnabled)) {
        activeFeatures.longJumpEnabled = false;
        activeFeatures.fireballEspEnabled = false;
        activeFeatures.localMobAuraEnabled = false;
        activeFeatures.localVelocityEnabled = false;
        queueFeatureChanged(activeFeatures);
        m_bindings->enqueueDebugChatLine(
            "[Local Guard] Local diagnostics require an integrated single-player world.");
    }

    if (env != nullptr) {
        GameplaySettings gameplay{};
        gameplay.safewalk = activeFeatures.safewalkEnabled && !interactiveNow;
        gameplay.scaffold = activeFeatures.scaffoldEnabled && !interactiveNow;
        gameplay.scaffoldSameLayerOnly = activeFeatures.scaffoldSameLayerOnly;
        gameplay.fly = activeFeatures.flyEnabled && !interactiveNow;
        gameplay.bhop = activeFeatures.bhopEnabled && !interactiveNow;
        gameplay.bhopAutoJump = activeFeatures.bhopAutoJump;
        gameplay.aimAssist = activeFeatures.aimAssistEnabled && !interactiveNow;
        gameplay.longJump = activeFeatures.longJumpEnabled && !interactiveNow &&
                            snapshot.integratedSinglePlayer;
        gameplay.aimSlowdownMode = activeFeatures.aimSlowdownMode;
        gameplay.aimNearestPriority = activeFeatures.aimNearestPriority;
        gameplay.localMobAura = activeFeatures.localMobAuraEnabled &&
            !interactiveNow && snapshot.integratedSinglePlayer;
        gameplay.localVelocity = activeFeatures.localVelocityEnabled &&
            !interactiveNow && snapshot.integratedSinglePlayer;
        gameplay.safewalkReleaseDelayMs = activeFeatures.safewalkReleaseDelayMs;
        gameplay.safewalkEdgeSensitivity = activeFeatures.safewalkEdgeSensitivity;
        gameplay.safewalkMinimumPitch = activeFeatures.safewalkMinimumPitch;
        gameplay.flySpeedPercent = activeFeatures.flySpeedPercent;
        gameplay.bhopAirSpeedPercent = activeFeatures.bhopAirSpeedPercent;
        gameplay.aimSlowdownPercent = activeFeatures.aimSlowdownPercent;
        gameplay.aimSpeedPercent = activeFeatures.aimSpeedPercent;
        gameplay.aimMinimumDistance = activeFeatures.aimMinimumDistance;
        gameplay.aimMaximumDistance = activeFeatures.aimMaximumDistance;
        gameplay.aimFovDegrees = activeFeatures.aimFovDegrees;
        gameplay.longJumpSpeedPercent = activeFeatures.longJumpSpeedPercent;
        gameplay.localMobReach = activeFeatures.localMobReach;
        gameplay.localAttackDelayMs = activeFeatures.localAttackDelayMs;
        gameplay.localVelocityPercent = activeFeatures.localVelocityPercent;
        (void)m_bindings->updateGameplay(env, gameplay, snapshot,
                                         tickMilliseconds);
    }
    const std::uint32_t currentBlacklistRevision =
        m_blacklistRevision.load(std::memory_order_acquire);
    if (currentBlacklistRevision != m_runtimeBlacklistRevision) {
        ::AcquireSRWLockShared(&m_blacklistLock);
        m_runtimeBlacklistSnapshot = m_blacklistSnapshot;
        ::ReleaseSRWLockShared(&m_blacklistLock);
        m_runtimeBlacklistRevision = currentBlacklistRevision;
    }
    if (!snapshot.matchActive) {
        m_blacklistChatWarnedCount = 0U;
    } else if (m_runtimeBlacklistSnapshot.matchAlertsEnabled) {
        for (std::uint32_t entryIndex = 0U;
             entryIndex < m_runtimeBlacklistSnapshot.count; ++entryIndex) {
            const BlacklistEntry& entry =
                m_runtimeBlacklistSnapshot.entries[entryIndex];
            if (!entry.warnOnEncounter) continue;
            bool encountered = false;
            for (std::uint32_t playerIndex = 0U;
                 playerIndex < snapshot.playerCount; ++playerIndex) {
                const PlayerIdentity& player = snapshot.players[playerIndex];
                encountered = entry.idOnly
                    ? ::_stricmp(entry.name.data(), player.name.data()) == 0
                    : entry.uuid[0U] != '\0' && player.uuid[0U] != '\0' &&
                      ::_stricmp(entry.uuid.data(), player.uuid.data()) == 0;
                if (encountered) break;
            }
            if (!encountered) continue;
            bool alreadyWarned = false;
            for (std::uint32_t index = 0U;
                 index < m_blacklistChatWarnedCount; ++index) {
                if (::_stricmp(m_blacklistChatWarnedKeys[index].data(),
                               entry.key.data()) == 0) {
                    alreadyWarned = true;
                    break;
                }
            }
            if (alreadyWarned) continue;
            if (m_blacklistChatWarnedCount <
                m_blacklistChatWarnedKeys.size()) {
                auto& key = m_blacklistChatWarnedKeys[
                    m_blacklistChatWarnedCount++];
                std::snprintf(key.data(), key.size(), "%s", entry.key.data());
            }
            m_bindings->enqueueWarningChatLine(entry.name.data(),
                                                entry.reason.data());
        }
    }
    if (env != nullptr)
        m_bindings->publishDebugChat(env, activeFeatures.debugChatEnabled);
    if (m_visible.load(std::memory_order_acquire)) {
        m_renderer->setGuiScaleIndex(m_guiScaleIndex.load(std::memory_order_acquire));
        m_renderer->setFeatureSettings(activeFeatures);
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
        const std::uint32_t blacklistRevision =
            m_blacklistRevision.load(std::memory_order_acquire);
        if (blacklistRevision != m_renderBlacklistRevision) {
            BlacklistOverlaySnapshot blacklist{};
            ::AcquireSRWLockShared(&m_blacklistLock);
            blacklist = m_blacklistSnapshot;
            ::ReleaseSRWLockShared(&m_blacklistLock);
            m_renderer->setBlacklistSnapshot(blacklist);
            m_renderBlacklistRevision = blacklistRevision;
        }
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
        BlacklistAction blacklistAction{};
        if (m_renderer->consumeBlacklistAction(blacklistAction))
            queueBlacklistAction(blacklistAction);
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
