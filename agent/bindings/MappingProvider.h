#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mcoverlay::bindings {

enum class ClientFamily : std::uint8_t {
    Unknown,
    Vanilla,
    Forge,
    Lunar
};

[[nodiscard]] const char* clientFamilyName(ClientFamily family) noexcept;

// Evidence is collected while walking the single JVMTI loaded-class snapshot
// and from harmless JVM launch properties. It intentionally contains no JNI
// references and is discarded after binding resolution.
class ClientEnvironment final {
public:
    void addEvidence(ClientFamily family, std::uint16_t confidence) noexcept;
    [[nodiscard]] ClientFamily family() const noexcept;
    [[nodiscard]] std::uint16_t confidence(ClientFamily family) const noexcept;

private:
    std::array<std::uint16_t, 4U> m_confidence{};
};

enum class DetectionMatch : std::uint8_t {
    ExactClassSignature,
    ClassSignaturePrefix,
    LaunchHintContains
};

struct DetectionPattern final {
    DetectionMatch match = DetectionMatch::ExactClassSignature;
    std::string value;
    std::uint16_t confidence = 100U;
};

// A complete, owned mapping dictionary. Strings are owned rather than viewed
// so a future controller-side parser or signed mapping pack can register a
// dictionary without keeping the source buffer alive. Once the registry is
// frozen, dictionaries are immutable for the lifetime of GameBindings.
struct MappingDictionary final {
    std::string id;
    std::string label;
    ClientFamily family = ClientFamily::Unknown;
    std::vector<DetectionPattern> detection;

    std::string minecraftName;
    std::string minecraftSignature;
    std::string playerName;
    std::string playerSignature;
    std::string livingName;
    std::string livingSignature;
    std::string entityName;
    std::string entitySignature;
    std::string aabbName;
    std::string aabbSignature;
    std::string worldName;
    std::string worldSignature;
    std::string worldClientName;
    std::string worldClientSignature;
    std::string stateName;
    std::string stateSignature;
    std::string blockName;
    std::string blockSignature;
    std::string blockPosName;
    std::string blockPosSignature;
    std::string bedName;
    std::string bedSignature;
    std::string chunkProviderName;
    std::string chunkProviderSignature;
    std::string chunkProviderInterfaceSignature;
    std::string chunkName;
    std::string chunkSignature;
    std::string storageName;
    std::string storageSignature;
    std::string activeRenderInfoName;
    std::string activeRenderInfoSignature;
    std::string renderManagerName;
    std::string renderManagerSignature;
    std::string timerName;
    std::string timerSignature;
    std::string chatComponentName;
    std::string chatComponentSignature;
    std::string chatTextName;
    std::string chatTextSignature;

    std::string scoreboardName;
    std::string scoreboardSignature;
    std::string scoreObjectiveName;
    std::string scoreObjectiveSignature;
    std::string scoreName;
    std::string scoreSignature;
    std::string scorePlayerTeamName;
    std::string scorePlayerTeamSignature;
    std::string itemStackName;
    std::string itemStackSignature;
    std::string itemName;
    std::string itemSignature;
    std::string itemArmorName;
    std::string itemArmorSignature;
    std::string inventoryPlayerName;
    std::string inventoryPlayerSignature;

    std::string getMinecraft;
    // Exactly one singleton accessor is required. Lunar's named runtime is
    // most stable through its static `theMinecraft` field, while Vanilla and
    // Forge use the conventional static getMinecraft() method.
    std::string minecraftInstanceField;
    std::string playerField;
    std::string getHealth;
    std::string getMaxHealth;
    std::string getEntityId;
    std::string getBounds;
    std::string isMainThread;
    std::string isSingleplayer;
    std::string worldField;
    std::array<std::string, 3U> positionFields;
    std::array<std::string, 3U> previousPositionFields;
    std::string getLoadedEntities;
    // Optional field alternative used by Lunar (`loadedEntityList`).
    std::string loadedEntitiesField;
    std::string playerEntitiesField;
    std::string getName;
    std::string getDisplayName;
    std::string getFormattedText;
    std::string addChatMessage;
    std::string getBlockState;
    std::string getBlock;
    std::string getBlockMetadata;
    std::string setIngameFocus;
    std::string setIngameNotInFocus;
    std::string getChunkProvider;
    std::string chunkListingField;
    std::array<std::string, 2U> chunkCoordinateFields;
    std::string getStorageArrays;
    std::string getStorageData;
    std::string getRenderManager;
    std::array<std::string, 3U> renderPositionFields;
    std::string activeModelViewField;
    std::string activeProjectionField;
    std::string activeViewportField;
    std::string timerField;
    std::string renderPartialTicksField;
    std::array<std::string, 6U> aabbFields;

    std::string getScoreboard;
    std::string getObjectiveInDisplaySlot;
    std::string getPlayersTeam;
    std::string getSortedScores;
    std::string getPlayerName;
    std::string formatPlayerName;
    std::string inventoryField;
    std::string armorInventoryField;
    std::string getItem;
    std::string hasColor;
    std::string getColor;

    // Strict validation prevents a partially filled or malformed external
    // dictionary from ever reaching JNI GetMethodID/GetFieldID calls.
    [[nodiscard]] bool validate(std::string* error = nullptr) const noexcept;
};

// Providers are configuration-time objects. Implementations may contain one
// or more dictionaries (for example, several verified Lunar releases) and
// contribute environment evidence without triggering another class scan.
class MappingProvider {
public:
    virtual ~MappingProvider() = default;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual ClientFamily family() const noexcept = 0;
    [[nodiscard]] virtual int priority() const noexcept = 0;
    [[nodiscard]] virtual std::span<const MappingDictionary> dictionaries() const noexcept = 0;
    virtual void observeClassSignature(std::string_view signature,
                                       ClientEnvironment& environment) const noexcept = 0;
    virtual void observeLaunchHint(std::string_view hint,
                                   ClientEnvironment& environment) const noexcept = 0;
};

enum class MappingRegistrationResult : std::uint8_t {
    Accepted,
    Frozen,
    Invalid,
    Duplicate,
    CapacityExceeded,
    OutOfMemory
};

struct MappingCandidates final {
    static constexpr std::size_t Capacity = 16U;
    std::array<const MappingDictionary*, Capacity> items{};
    std::size_t count = 0U;
    bool truncated = false;

    [[nodiscard]] bool empty() const noexcept { return count == 0U; }
};

// Registry ownership is local to one GameBindings instance. registerDictionary
// is the stable dynamic-loading seam: a later JSON/binary/Open-Vape adapter
// should parse into MappingDictionary, validate it here, and never expose its
// source buffer to the render thread. Registration closes atomically when
// freeze() begins the one-shot resolver pass.
class MappingRegistry final {
public:
    MappingRegistry() noexcept;
    ~MappingRegistry();

    MappingRegistry(const MappingRegistry&) = delete;
    MappingRegistry& operator=(const MappingRegistry&) = delete;

    [[nodiscard]] MappingRegistrationResult registerDictionary(
        MappingDictionary dictionary, std::string* error = nullptr) noexcept;
    [[nodiscard]] MappingRegistrationResult registerProvider(
        std::unique_ptr<MappingProvider> provider,
        std::string* error = nullptr) noexcept;

    // freeze() is idempotent and is the publication barrier between optional
    // configuration and the resolver thread. No provider can be mutated after
    // it returns.
    [[nodiscard]] bool freeze() noexcept;
    [[nodiscard]] bool healthy() const noexcept;

    void observeClassSignature(std::string_view signature,
                               ClientEnvironment& environment) const noexcept;
    void observeLaunchHint(std::string_view hint,
                           ClientEnvironment& environment) const noexcept;
    [[nodiscard]] bool hasMappingsForFamily(ClientFamily family) const noexcept;
    [[nodiscard]] bool isMinecraftAnchor(std::string_view signature) const noexcept;
    [[nodiscard]] MappingCandidates candidatesForAnchor(
        std::string_view signature,
        const ClientEnvironment& environment) const noexcept;

private:
    [[nodiscard]] bool containsDictionaryId(std::string_view id) const noexcept;
    void setError(std::string* error, std::string_view message) const noexcept;

    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<MappingProvider>> m_providers;
    bool m_frozen = false;
    bool m_healthy = true;
};

} // namespace mcoverlay::bindings
