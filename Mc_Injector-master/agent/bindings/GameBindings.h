#pragma once

#include <jni.h>
#include <jvmti.h>
#include <windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "MappingProvider.h"

namespace mcoverlay {

struct AxisAlignedBox final {
    double minX = 0.0;
    double minY = 0.0;
    double minZ = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    double maxZ = 0.0;
};

struct EntityMarker final {
    AxisAlignedBox bounds{};
    double previousX = 0.0;
    double previousY = 0.0;
    double previousZ = 0.0;
    double currentX = 0.0;
    double currentY = 0.0;
    double currentZ = 0.0;
    jint entityId = -1;
    double distance = 0.0;
    bool player = false;
    bool hasArmor = false;
    char armorTeam = 'u';
};

struct BedDefenseBlock final {
    static constexpr std::size_t MaxRadius = 10U;
    std::uint16_t blockId = 0U;
    std::uint8_t metadata = 0U;
    // Index is the exact Chebyshev ring (1..10). The renderer sums rings up
    // to the user-selected radius, so changing 3..10 never triggers JNI work.
    std::array<std::uint16_t, MaxRadius + 1U> ringCounts{};
};

struct BedMarker final {
    static constexpr std::size_t MaxDefenseBlocks = 12U;
    int x = 0;
    int y = 0;
    int z = 0;
    int footX = 0;
    int footZ = 0;
    std::array<BedDefenseBlock, MaxDefenseBlocks> defense{};
    std::uint8_t defenseCount = 0U;
};

struct PlayerIdentity final {
    std::array<char, 17U> name{};
    // Minecraft formatting color code without the section-sign prefix.
    // For example 'c' represents the protocol token "\xC2\xA7c".
    char teamColor = 'f';
};

struct WorldCameraSnapshot final {
    std::array<float, 16U> modelView{};
    std::array<float, 16U> projection{};
    std::array<int, 4U> viewport{};
    double renderX = 0.0;
    double renderY = 0.0;
    double renderZ = 0.0;
    float partialTicks = 0.0F;
    bool valid = false;
};

struct GameSnapshot final {
    static constexpr std::size_t MaxEntityMarkers = 128U;
    static constexpr std::size_t MaxBedMarkers = 128U;
    static constexpr std::size_t MaxDiscoveredPlayers = 64U;
    enum class State : std::uint8_t {
        Resolving,
        Unsupported,
        WaitingForGameThread,
        NoPlayer,
        Ready,
        JniError
    };

    State state = State::Resolving;
    float health = 0.0F;
    float maxHealth = 0.0F;
    jint entityId = -1;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    AxisAlignedBox bounds{};
    jint loadedEntities = 0;
    bool singlePlayer = false;
    std::array<EntityMarker, MaxEntityMarkers> entityMarkers{};
    std::uint32_t entityMarkerCount = 0U;
    // Incremented only when the 20 Hz JNI entity snapshot is refreshed. The
    // renderer combines this with renderPartialTicks to detect a game-tick
    // boundary that happened between two snapshots and briefly extrapolate it.
    std::uint64_t entitySampleGeneration = 0U;
    std::array<BedMarker, MaxBedMarkers> bedMarkers{};
    std::uint32_t bedMarkerCount = 0U;
    std::uint32_t bedCount = 0U;
    float bedScanProgress = 0.0F;
    std::array<PlayerIdentity, MaxDiscoveredPlayers> players{};
    std::uint32_t playerCount = 0U;
    std::uint64_t playerRosterGeneration = 0U;
    std::array<char, 17U> localPlayerName{};
    // True only after at least two distinct BedWars team tags (for example
    // [R] and [B]) are visible in the formatted player roster.
    bool matchActive = false;
    char ownTeam = 'f';
    bool ownBedKnown = false;
    int ownBedX = 0;
    int ownBedY = 0;
    int ownBedZ = 0;
    WorldCameraSnapshot camera{};
    std::uint32_t mappingAttempt = 0U;
    std::uint32_t mappingRetryInMs = 0U;
    const char* mapping = "unresolved";
};

// Minecraft 1.8.9-only JNI binding cache. Only jclass global references and
// method/field IDs survive a frame. Minecraft/player/AABB object references
// are always local to sample() and are discarded before SwapBuffers returns.
class GameBindings final {
public:
    GameBindings(JavaVM* vm, jvmtiEnv* jvmti) noexcept;
    ~GameBindings();

    GameBindings(const GameBindings&) = delete;
    GameBindings& operator=(const GameBindings&) = delete;

    // Runtime mapping packs are converted to this owned, validated form by an
    // external parser and must be registered before runResolver starts. This
    // keeps file/JSON parsing out of the injected render path and provides the
    // transformed-client mapping integration point without guessing names.
    [[nodiscard]] bindings::MappingRegistrationResult registerMappingDictionary(
        bindings::MappingDictionary dictionary,
        std::string* error = nullptr) noexcept;

    // The resolver is called exactly once from AgentRuntime's dedicated native
    // daemon thread. It owns every JVMTI class-table walk and all ID lookups;
    // neither resolve() nor GetLoadedClasses can therefore reach SwapBuffers.
    // One snapshot is used because repeated GetLoadedClasses calls can force
    // global JVM safepoints even from a background thread.
    void runResolver(JNIEnv* env, HANDLE stopEvent) noexcept;
    // Dedicated JNI daemon. Every 500 ms it snapshots the loaded chunk list,
    // bulk-copies only newly seen section char[] arrays, and publishes a
    // fixed-size bed cache. It never executes from SwapBuffers.
    void runBedScanner(JNIEnv* env, HANDLE stopEvent) noexcept;
    // Invalidates the processed-chunk set. The scanner then bulk-copies every
    // currently loaded chunk on its next cycle, discovering beds placed after
    // initial chunk load without moving any block reads onto the render thread.
    void requestBedRescan() noexcept;
    void markResolverUnavailable() noexcept;

    // Rendering owns m_snapshot. Resolver progress is kept in atomics and is
    // folded into a by-value copy, so the resolver never writes render-owned
    // memory. The immutable cache is published with release/acquire ordering.
    [[nodiscard]] GameSnapshot snapshot(std::uint64_t tickMilliseconds) const noexcept;
    [[nodiscard]] const GameSnapshot& sample(JNIEnv* env, std::uint64_t tickMilliseconds) noexcept;
    // ActiveRenderInfo is refreshed by Minecraft during every 3D world pass.
    // This lightweight copy intentionally runs once per SwapBuffers frame so
    // yaw/pitch, FOV and view-bobbing never inherit the 10 Hz telemetry limit.
    void sampleCamera(JNIEnv* env) noexcept;
    // Releases/reacquires LWJGL's mouse grab through Minecraft's own focus
    // methods. Must be called from the Java-owned render thread.
    [[nodiscard]] bool setInputCaptured(JNIEnv* env, bool guiOpen) noexcept;
    // Transformed clients can re-grab LWJGL Mouse after an external WndProc
    // handled the hotkey. While Click GUI is open, enforce only the stable
    // LWJGL release path each frame without repeatedly invoking Minecraft's
    // mapped focus methods.
    [[nodiscard]] bool maintainInputReleased(JNIEnv* env) noexcept;
    void release(JNIEnv* env) noexcept;
    void abandon() noexcept;

private:
    using MappingProfile = bindings::MappingDictionary;
    struct BindingCache;

    [[nodiscard]] bool resolve(JNIEnv* env) noexcept;
    [[nodiscard]] bool resolveProfile(JNIEnv* env,
                                      const MappingProfile& profile,
                                      jclass minecraft,
                                      BindingCache& candidate);
    [[nodiscard]] jclass loadWithClassLoader(JNIEnv* env,
                                             jobject loader,
                                             jmethodID loadClass,
                                             const char* binaryName) noexcept;
    // A resolve attempt takes exactly one JVMTI class-table snapshot. Reusing
    // that pass for all registered profiles is critical: GetLoadedClasses plus
    // thousands of GetClassSignature calls is far too expensive for every
    // SwapBuffers frame (or even every 20 Hz data sample).
    [[nodiscard]] jclass findMinecraftClass(
        JNIEnv* env,
        bindings::MappingCandidates& candidates,
        bindings::ClientEnvironment& environment) noexcept;
    void probeEnvironmentHints(JNIEnv* env,
                               bindings::ClientEnvironment& environment) noexcept;
    void clearException(JNIEnv* env) const noexcept;
    [[nodiscard]] bool setLwjglMouseGrabbed(JNIEnv* env, bool grabbed) noexcept;
    static void deleteGlobalRefs(JNIEnv* env, BindingCache& cache) noexcept;

    JavaVM* m_vm = nullptr;
    jvmtiEnv* m_jvmti = nullptr;
    bindings::MappingRegistry m_mappingRegistry;
    std::unique_ptr<BindingCache> m_cache;

    enum class ResolutionPhase : std::uint8_t {
        Resolving,
        Resolved,
        Unsupported,
        Unavailable,
        Stopped
    };
    std::atomic<ResolutionPhase> m_resolutionPhase{ResolutionPhase::Resolving};
    std::atomic<std::uint32_t> m_mappingAttempt{0U};
    std::atomic<std::uint64_t> m_retryAtMilliseconds{0U};
    std::uint64_t m_lastSample = 0U;
    std::uint64_t m_entitySampleGeneration = 0U;
    std::uint64_t m_lastPlayerScan = 0U;
    std::uint64_t m_playerRosterGeneration = 0U;
    jweak m_lastWorld = nullptr;
    GameSnapshot m_snapshot{};

    struct PublishedBedCache final {
        std::array<BedMarker, GameSnapshot::MaxBedMarkers> markers{};
        std::uint32_t markerCount = 0U;
        std::uint32_t loadedChunkCount = 0U;
        std::uint64_t generation = 0U;
    };
    mutable SRWLOCK m_bedCacheLock = SRWLOCK_INIT;
    PublishedBedCache m_publishedBedCache{};
    std::atomic<bool> m_bedRescanRequested{false};
    HANDLE m_bedRescanEvent = nullptr;

    jclass m_lwjglMouseClass = nullptr;
    jmethodID m_lwjglSetGrabbed = nullptr;
};

} // namespace mcoverlay
