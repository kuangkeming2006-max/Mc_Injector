#pragma once

#include <jni.h>
#include <jvmti.h>
#include <windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "BedWarsState.h"
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
    float health = 0.0F;
    float maxHealth = 0.0F;
    double distance = 0.0;
    bool player = false;
    bool invisible = false;
    bool hasArmor = false;
    std::uint8_t protectionLevel = 0U;
    std::int16_t heldItemId = -1;
    std::uint8_t heldItemCount = 0U;
    std::uint16_t heldItemDamage = 0U;
    char armorTeam = 'u';
    char teamColor = 'u';
    std::array<char, 17U> playerName{};
    std::array<char, 37U> uuid{};
    // True only after this spawned entity was joined to the persistent TAB
    // roster (UUID first, exact name as a compatibility fallback).  Renderers
    // use this to exclude shop NPCs and other player-shaped entities.
    bool confirmedPlayer = false;
    // OpenGL texture name owned by Minecraft's TextureManager in the same
    // context used by the SwapBuffers hook.  The agent never deletes it.
    std::uint32_t skinTextureId = 0U;
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
    // Derived only from nearby, bulk-copied team-coloured wool evidence. An
    // ambiguous/absent result remains unknown; the threat detector must never
    // guess an own bed from player proximity.
    char teamColor = 'u';
};

struct PlayerIdentity final {
    std::array<char, 17U> name{};
    std::array<char, 37U> uuid{};
    // Minecraft formatting color code without the section-sign prefix.
    // For example 'c' represents the protocol token "\xC2\xA7c".
    char teamColor = 'u';
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
    bool hypixelServer = false;
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
    // True only after two consecutive snapshots agree on a local team. An
    // explicit [R]/[B]/... roster tag is the primary signal; a complete
    // Sidebar snapshot is accepted as corroborating evidence.
    bool matchActive = false;
    char ownTeam = 'u';
    enum class OwnBedSource : std::uint8_t { Unknown, TeamWool, MatchSpawn };
    bool ownBedKnown = false;
    int ownBedX = 0;
    int ownBedY = 0;
    int ownBedZ = 0;
    OwnBedSource ownBedSource = OwnBedSource::Unknown;
    WorldCameraSnapshot camera{};
    std::uint32_t mappingAttempt = 0U;
    std::uint32_t mappingRetryInMs = 0U;
    const char* mapping = "unresolved";
};

struct GameplaySettings final {
    bool safewalk = false;
    bool scaffold = false;
    bool fly = false;
    bool bhop = false;
    bool bhopAutoJump = true;
    bool aimAssist = false;
    bool aimSlowdownMode = true;
    int safewalkReleaseDelayMs = 120;
    int safewalkEdgeSensitivity = 55;
    int safewalkMinimumPitch = -5;
    int flySpeedPercent = 100;
    int aimSlowdownPercent = 45;
    int aimSpeedPercent = 35;
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
    // Main-thread edge assistant. It samples only four support blocks beneath
    // the player's AABB and controls Minecraft's own sneak KeyBinding. The
    // method restores the physical key state whenever the feature disables,
    // the world disappears, or the agent shuts down.
    [[nodiscard]] bool updateGameplay(JNIEnv* env,
                                      const GameplaySettings& settings,
                                      const GameSnapshot& snapshot,
                                      std::uint64_t tickMilliseconds) noexcept;
    // Adds a client-side component directly to EntityPlayerSP's chat log. It
    // does not invoke the network handler and therefore cannot send a message
    // to the server. Calls are de-duplicated by the roster generation.
    void publishDebugChat(JNIEnv* env, bool enabled) noexcept;
    // IPC/query threads may enqueue bounded diagnostics here. The Java chat
    // call itself is always performed later by the attached render thread.
    void enqueueDebugChatLine(std::string_view line) noexcept;
    // Queues a local-only, clickable blacklist warning. Clicking it asks the
    // Minecraft chat screen to prefill `/wdr <name>` via SUGGEST_COMMAND; the
    // command is never sent automatically.
    void enqueueWarningChatLine(std::string_view playerName,
                                std::string_view reason) noexcept;
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
    [[nodiscard]] bool ensureLwjglMouseBindings(JNIEnv* env) noexcept;
    [[nodiscard]] bool queryLwjglMouseGrabbed(JNIEnv* env, bool& grabbed) noexcept;
    [[nodiscard]] bool setLwjglMouseGrabbed(JNIEnv* env, bool grabbed) noexcept;
    [[nodiscard]] bool ensureLwjglKeyboardBindings(JNIEnv* env) noexcept;
    [[nodiscard]] bool queryLwjglKeyDown(JNIEnv* env, int lwjglKey,
                                         bool& down) noexcept;
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
    std::uint64_t m_debugRosterGeneration = 0U;
    std::uint64_t m_bedOwnershipGeneration = 0U;
    std::uint64_t m_debugBedOwnershipGeneration = 0U;
    struct MatchProbeState final {
        bool sidebarAvailable = false;
        bool tabAvailable = false;
        bool sidebarEvidence = false;
        bool rosterEvidence = false;
        bool armorEvidence = false;
        std::uint8_t sidebarLines = 0U;
        std::uint8_t sidebarTeams = 0U;
        std::uint8_t sidebarYouRows = 0U;
        std::uint8_t rosterTaggedPlayers = 0U;
        std::uint8_t rosterPlayers = 0U;
        std::uint8_t rosterTeams = 0U;
        char rosterOwnTeam = 'u';
        char localArmorTeam = 'u';
        std::uint8_t armorTeams = 0U;
        std::uint8_t stableCount = 0U;

        [[nodiscard]] bool operator==(const MatchProbeState&) const noexcept = default;
    };
    MatchProbeState m_matchProbe{};
    std::uint64_t m_matchProbeGeneration = 0U;
    std::uint64_t m_debugMatchProbeGeneration = 0U;
    jweak m_lastWorld = nullptr;
    bedwars::Team m_sidebarCandidateTeam = bedwars::Team::Unknown;
    std::uint8_t m_sidebarStableCount = 0U;
    std::uint8_t m_sidebarMissingCount = 0U;
    bool m_matchAnchorValid = false;
    double m_matchAnchorX = 0.0;
    double m_matchAnchorY = 0.0;
    double m_matchAnchorZ = 0.0;
    bool m_lockedOwnBedKnown = false;
    int m_lockedOwnBedX = 0;
    int m_lockedOwnBedY = 0;
    int m_lockedOwnBedZ = 0;
    GameSnapshot::OwnBedSource m_lockedOwnBedSource =
        GameSnapshot::OwnBedSource::Unknown;
    GameSnapshot m_snapshot{};

    static constexpr std::size_t DebugQueueCapacity = 32U;
    static constexpr std::size_t DebugLineCapacity = 160U;
    mutable SRWLOCK m_debugQueueLock = SRWLOCK_INIT;
    std::array<std::array<char, DebugLineCapacity>, DebugQueueCapacity> m_debugQueue{};
    std::uint32_t m_debugQueueHead = 0U;
    std::uint32_t m_debugQueueCount = 0U;

    struct WarningChatLine final {
        std::array<char, 17U> playerName{};
        std::array<char, 81U> reason{};
    };
    static constexpr std::size_t WarningQueueCapacity = 8U;
    mutable SRWLOCK m_warningQueueLock = SRWLOCK_INIT;
    std::array<WarningChatLine, WarningQueueCapacity> m_warningQueue{};
    std::uint32_t m_warningQueueHead = 0U;
    std::uint32_t m_warningQueueCount = 0U;

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
    jmethodID m_lwjglIsGrabbed = nullptr;
    jclass m_lwjglKeyboardClass = nullptr;
    jmethodID m_lwjglIsKeyDown = nullptr;
    bool m_overlayInputSessionActive = false;
    bool m_inputGrabStateKnown = false;
    bool m_inputWasGrabbed = true;
    bool m_safewalkSneakForced = false;
    int m_safewalkSneakKeyCode = 0;
    std::uint8_t m_safewalkSupportMask = 0U;
    std::uint64_t m_safewalkReleaseAt = 0U;
    std::uint64_t m_lastScaffoldPlacementTick = 0U;
    std::uint64_t m_lastGameplayTick = 0U;
    bool m_aimSensitivityModified = false;
    float m_originalMouseSensitivity = 0.5F;
};

} // namespace mcoverlay
