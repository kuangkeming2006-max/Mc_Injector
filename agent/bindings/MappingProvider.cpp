#include "MappingProvider.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <utility>

namespace mcoverlay::bindings {
namespace {

constexpr std::size_t kMaxProviders = 32U;
constexpr std::size_t kMaxDictionariesPerProvider = 16U;
constexpr std::size_t kMaxDetectionPatterns = 24U;
constexpr std::size_t kMaxIdentifierLength = 64U;
constexpr std::size_t kMaxLabelLength = 128U;
constexpr std::size_t kMaxSymbolLength = 512U;

void assignError(std::string* const error, const std::string_view message) noexcept
{
    if (error == nullptr) {
        return;
    }
    try {
        error->assign(message);
    } catch (...) {
        error->clear();
    }
}

[[nodiscard]] bool validIdentifier(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > kMaxIdentifierLength) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char character) noexcept {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '.' ||
               character == '_' || character == '-';
    });
}

[[nodiscard]] bool validBinaryName(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > kMaxSymbolLength ||
        value.front() == '.' || value.back() == '.') {
        return false;
    }
    for (const char character : value) {
        if (character == '/' || character == ';' || character == '[' ||
            character == '(' || character == ')' || character == '\0') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validMemberName(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > kMaxSymbolLength) {
        return false;
    }
    for (const char character : value) {
        if (character == '.' || character == '/' || character == ';' ||
            character == '[' || character == '(' || character == ')' ||
            character == '<' || character == '>' || character == '\0') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool signatureMatchesBinaryName(const std::string_view binaryName,
                                              const std::string_view signature) noexcept
{
    if (!validBinaryName(binaryName) || signature.size() != binaryName.size() + 2U ||
        signature.front() != 'L' || signature.back() != ';') {
        return false;
    }
    for (std::size_t index = 0U; index < binaryName.size(); ++index) {
        const char expected = binaryName[index] == '.' ? '/' : binaryName[index];
        if (signature[index + 1U] != expected) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool containsCaseInsensitive(const std::string_view haystack,
                                           const std::string_view needle) noexcept
{
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const auto equal = [](const char left, const char right) noexcept {
        return std::tolower(static_cast<unsigned char>(left)) ==
               std::tolower(static_cast<unsigned char>(right));
    };
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end(), equal) != haystack.end();
}

[[nodiscard]] bool matchesPattern(const DetectionPattern& pattern,
                                  const std::string_view input,
                                  const bool launchHint) noexcept
{
    switch (pattern.match) {
    case DetectionMatch::ExactClassSignature:
        return !launchHint && input == pattern.value;
    case DetectionMatch::ClassSignaturePrefix:
        return !launchHint && input.starts_with(pattern.value);
    case DetectionMatch::LaunchHintContains:
        return launchHint && containsCaseInsensitive(input, pattern.value);
    }
    return false;
}

class OwnedMappingProvider final : public MappingProvider {
public:
    OwnedMappingProvider(std::string id,
                         const ClientFamily family,
                         const int priority,
                         std::vector<DetectionPattern> detection,
                         std::vector<MappingDictionary> dictionaries)
        : m_id(std::move(id)),
          m_family(family),
          m_priority(priority),
          m_detection(std::move(detection)),
          m_dictionaries(std::move(dictionaries))
    {
    }

    [[nodiscard]] std::string_view id() const noexcept override { return m_id; }
    [[nodiscard]] ClientFamily family() const noexcept override { return m_family; }
    [[nodiscard]] int priority() const noexcept override { return m_priority; }
    [[nodiscard]] std::span<const MappingDictionary> dictionaries() const noexcept override
    {
        return m_dictionaries;
    }

    void observeClassSignature(const std::string_view signature,
                               ClientEnvironment& environment) const noexcept override
    {
        observe(signature, false, environment);
    }

    void observeLaunchHint(const std::string_view hint,
                           ClientEnvironment& environment) const noexcept override
    {
        observe(hint, true, environment);
    }

private:
    void observe(const std::string_view value,
                 const bool launchHint,
                 ClientEnvironment& environment) const noexcept
    {
        for (const DetectionPattern& pattern : m_detection) {
            if (matchesPattern(pattern, value, launchHint)) {
                environment.addEvidence(m_family, pattern.confidence);
            }
        }
        for (const MappingDictionary& dictionary : m_dictionaries) {
            for (const DetectionPattern& pattern : dictionary.detection) {
                if (matchesPattern(pattern, value, launchHint)) {
                    environment.addEvidence(m_family, pattern.confidence);
                }
            }
        }
    }

    std::string m_id;
    ClientFamily m_family = ClientFamily::Unknown;
    int m_priority = 0;
    std::vector<DetectionPattern> m_detection;
    std::vector<MappingDictionary> m_dictionaries;
};

[[nodiscard]] MappingDictionary forgeSrgDictionary()
{
    MappingDictionary mapping;
    mapping.id = "minecraft-1.8.9-forge-srg";
    mapping.label = "Forge SRG";
    mapping.family = ClientFamily::Forge;
    mapping.detection = {
        {DetectionMatch::ClassSignaturePrefix, "Lnet/minecraftforge/", 190U},
        {DetectionMatch::ClassSignaturePrefix, "Lcpw/mods/fml/", 180U},
        {DetectionMatch::LaunchHintContains, "forge", 190U},
        {DetectionMatch::LaunchHintContains, "launchwrapper", 90U}};
    mapping.minecraftName = "net.minecraft.client.Minecraft";
    mapping.minecraftSignature = "Lnet/minecraft/client/Minecraft;";
    mapping.playerName = "net.minecraft.client.entity.EntityPlayerSP";
    mapping.playerSignature = "Lnet/minecraft/client/entity/EntityPlayerSP;";
    mapping.livingName = "net.minecraft.entity.EntityLivingBase";
    mapping.livingSignature = "Lnet/minecraft/entity/EntityLivingBase;";
    mapping.entityName = "net.minecraft.entity.Entity";
    mapping.entitySignature = "Lnet/minecraft/entity/Entity;";
    mapping.aabbName = "net.minecraft.util.AxisAlignedBB";
    mapping.aabbSignature = "Lnet/minecraft/util/AxisAlignedBB;";
    mapping.worldName = "net.minecraft.world.World";
    mapping.worldSignature = "Lnet/minecraft/world/World;";
    mapping.worldClientName = "net.minecraft.client.multiplayer.WorldClient";
    mapping.worldClientSignature = "Lnet/minecraft/client/multiplayer/WorldClient;";
    mapping.stateName = "net.minecraft.block.state.IBlockState";
    mapping.stateSignature = "Lnet/minecraft/block/state/IBlockState;";
    mapping.blockName = "net.minecraft.block.Block";
    mapping.blockSignature = "Lnet/minecraft/block/Block;";
    mapping.blockPosName = "net.minecraft.util.BlockPos";
    mapping.blockPosSignature = "Lnet/minecraft/util/BlockPos;";
    mapping.bedName = "net.minecraft.block.BlockBed";
    mapping.bedSignature = "Lnet/minecraft/block/BlockBed;";
    mapping.chunkProviderName = "net.minecraft.client.multiplayer.ChunkProviderClient";
    mapping.chunkProviderSignature = "Lnet/minecraft/client/multiplayer/ChunkProviderClient;";
    mapping.chunkProviderInterfaceSignature = "Lnet/minecraft/world/chunk/IChunkProvider;";
    mapping.chunkName = "net.minecraft.world.chunk.Chunk";
    mapping.chunkSignature = "Lnet/minecraft/world/chunk/Chunk;";
    mapping.storageName = "net.minecraft.world.chunk.storage.ExtendedBlockStorage";
    mapping.storageSignature = "Lnet/minecraft/world/chunk/storage/ExtendedBlockStorage;";
    mapping.activeRenderInfoName = "net.minecraft.client.renderer.ActiveRenderInfo";
    mapping.activeRenderInfoSignature = "Lnet/minecraft/client/renderer/ActiveRenderInfo;";
    mapping.renderManagerName = "net.minecraft.client.renderer.entity.RenderManager";
    mapping.renderManagerSignature = "Lnet/minecraft/client/renderer/entity/RenderManager;";
    mapping.timerName = "net.minecraft.util.Timer";
    mapping.timerSignature = "Lnet/minecraft/util/Timer;";
    mapping.chatComponentName = "net.minecraft.util.IChatComponent";
    mapping.chatComponentSignature = "Lnet/minecraft/util/IChatComponent;";
    mapping.chatTextName = "net.minecraft.util.ChatComponentText";
    mapping.chatTextSignature = "Lnet/minecraft/util/ChatComponentText;";
    mapping.getMinecraft = "func_71410_x";
    mapping.playerField = "field_71439_g";
    mapping.getHealth = "func_110143_aJ";
    mapping.getMaxHealth = "func_110138_aP";
    mapping.getEntityId = "func_145782_y";
    mapping.getBounds = "func_174813_aQ";
    mapping.isMainThread = "func_152345_ab";
    mapping.isSingleplayer = "func_71356_B";
    mapping.worldField = "field_71441_e";
    mapping.positionFields = {"field_70165_t", "field_70163_u", "field_70161_v"};
    mapping.previousPositionFields = {"field_70142_S", "field_70137_T", "field_70136_U"};
    mapping.getLoadedEntities = "func_72910_y";
    mapping.playerEntitiesField = "field_73010_i";
    mapping.getName = "func_70005_c_";
    mapping.getDisplayName = "func_145748_c_";
    mapping.getFormattedText = "func_150254_d";
    mapping.addChatMessage = "func_145747_a";
    mapping.getBlockState = "func_180495_p";
    mapping.getBlock = "func_177230_c";
    mapping.getBlockMetadata = "func_176201_c";
    mapping.setIngameFocus = "func_71381_h";
    mapping.setIngameNotInFocus = "func_71364_i";
    mapping.getChunkProvider = "func_72863_F";
    mapping.chunkListingField = "field_73237_c";
    mapping.chunkCoordinateFields = {"field_76635_g", "field_76647_h"};
    mapping.getStorageArrays = "func_76587_i";
    mapping.getStorageData = "func_177487_g";
    mapping.getRenderManager = "func_175598_ae";
    mapping.renderPositionFields = {"field_78725_b", "field_78726_c", "field_78723_d"};
    mapping.activeModelViewField = "field_178812_b";
    mapping.activeProjectionField = "field_178813_c";
    mapping.activeViewportField = "field_178814_a";
    mapping.timerField = "field_71428_T";
    mapping.renderPartialTicksField = "field_74281_c";
    mapping.aabbFields = {"field_72340_a", "field_72338_b", "field_72339_c",
                          "field_72336_d", "field_72337_e", "field_72334_f"};

    mapping.scoreboardName = "net.minecraft.scoreboard.Scoreboard";
    mapping.scoreboardSignature = "Lnet/minecraft/scoreboard/Scoreboard;";
    mapping.scoreObjectiveName = "net.minecraft.scoreboard.ScoreObjective";
    mapping.scoreObjectiveSignature = "Lnet/minecraft/scoreboard/ScoreObjective;";
    mapping.scoreName = "net.minecraft.scoreboard.Score";
    mapping.scoreSignature = "Lnet/minecraft/scoreboard/Score;";
    mapping.scorePlayerTeamName = "net.minecraft.scoreboard.ScorePlayerTeam";
    mapping.scorePlayerTeamSignature = "Lnet/minecraft/scoreboard/ScorePlayerTeam;";
    mapping.itemStackName = "net.minecraft.item.ItemStack";
    mapping.itemStackSignature = "Lnet/minecraft/item/ItemStack;";
    mapping.itemName = "net.minecraft.item.Item";
    mapping.itemSignature = "Lnet/minecraft/item/Item;";
    mapping.itemArmorName = "net.minecraft.item.ItemArmor";
    mapping.itemArmorSignature = "Lnet/minecraft/item/ItemArmor;";
    mapping.inventoryPlayerName = "net.minecraft.entity.player.InventoryPlayer";
    mapping.inventoryPlayerSignature = "Lnet/minecraft/entity/player/InventoryPlayer;";

    mapping.getScoreboard = "func_96441_U";
    mapping.getObjectiveInDisplaySlot = "func_96539_a";
    mapping.getPlayersTeam = "func_96509_i";
    mapping.getSortedScores = "func_96534_i";
    mapping.getPlayerName = "func_96653_e";
    mapping.formatPlayerName = "func_96667_a";
    mapping.inventoryField = "field_71071_by";
    mapping.armorInventoryField = "field_70460_b";
    mapping.getItem = "func_77973_b";
    mapping.hasColor = "func_82816_b_";
    mapping.getColor = "func_82814_b";

    return mapping;
}

[[nodiscard]] MappingDictionary vanillaNotchDictionary()
{
    MappingDictionary mapping;
    mapping.id = "minecraft-1.8.9-vanilla-notch";
    mapping.label = "Vanilla obfuscated";
    mapping.family = ClientFamily::Vanilla;
    mapping.minecraftName = "ave";
    mapping.minecraftSignature = "Lave;";
    mapping.playerName = "bew";
    mapping.playerSignature = "Lbew;";
    mapping.livingName = "pr";
    mapping.livingSignature = "Lpr;";
    mapping.entityName = "pk";
    mapping.entitySignature = "Lpk;";
    mapping.aabbName = "aug";
    mapping.aabbSignature = "Laug;";
    mapping.worldName = "adm";
    mapping.worldSignature = "Ladm;";
    mapping.worldClientName = "bdb";
    mapping.worldClientSignature = "Lbdb;";
    mapping.stateName = "alz";
    mapping.stateSignature = "Lalz;";
    mapping.blockName = "afh";
    mapping.blockSignature = "Lafh;";
    mapping.blockPosName = "cj";
    mapping.blockPosSignature = "Lcj;";
    mapping.bedName = "afg";
    mapping.bedSignature = "Lafg;";
    mapping.chunkProviderName = "bcz";
    mapping.chunkProviderSignature = "Lbcz;";
    mapping.chunkProviderInterfaceSignature = "Lamv;";
    mapping.chunkName = "amy";
    mapping.chunkSignature = "Lamy;";
    mapping.storageName = "amz";
    mapping.storageSignature = "Lamz;";
    mapping.activeRenderInfoName = "auz";
    mapping.activeRenderInfoSignature = "Lauz;";
    mapping.renderManagerName = "biu";
    mapping.renderManagerSignature = "Lbiu;";
    mapping.timerName = "avl";
    mapping.timerSignature = "Lavl;";
    mapping.chatComponentName = "eu";
    mapping.chatComponentSignature = "Leu;";
    mapping.chatTextName = "fa";
    mapping.chatTextSignature = "Lfa;";
    mapping.getMinecraft = "A";
    mapping.playerField = "h";
    mapping.getHealth = "bn";
    mapping.getMaxHealth = "bu";
    mapping.getEntityId = "F";
    mapping.getBounds = "aR";
    mapping.isMainThread = "aJ";
    mapping.isSingleplayer = "E";
    mapping.worldField = "f";
    mapping.positionFields = {"s", "t", "u"};
    mapping.previousPositionFields = {"P", "Q", "R"};
    mapping.getLoadedEntities = "E";
    mapping.playerEntitiesField = "j";
    mapping.getName = "e_";
    mapping.getDisplayName = "f_";
    mapping.getFormattedText = "c";
    mapping.addChatMessage = "a";
    mapping.getBlockState = "p";
    mapping.getBlock = "c";
    mapping.getBlockMetadata = "c";
    mapping.setIngameFocus = "n";
    mapping.setIngameNotInFocus = "o";
    mapping.getChunkProvider = "N";
    mapping.chunkListingField = "d";
    mapping.chunkCoordinateFields = {"a", "b"};
    mapping.getStorageArrays = "h";
    mapping.getStorageData = "g";
    mapping.getRenderManager = "af";
    mapping.renderPositionFields = {"o", "p", "q"};
    mapping.activeModelViewField = "b";
    mapping.activeProjectionField = "c";
    mapping.activeViewportField = "a";
    mapping.timerField = "Y";
    mapping.renderPartialTicksField = "c";
    mapping.aabbFields = {"a", "b", "c", "d", "e", "f"};

    mapping.scoreboardName = "auo";
    mapping.scoreboardSignature = "Lauo;";
    mapping.scoreObjectiveName = "auk";
    mapping.scoreObjectiveSignature = "Lauk;";
    mapping.scoreName = "aum";
    mapping.scoreSignature = "Laum;";
    mapping.scorePlayerTeamName = "bfh";
    mapping.scorePlayerTeamSignature = "Lbfh;";
    mapping.itemStackName = "zx";
    mapping.itemStackSignature = "Lzx;";
    mapping.itemName = "zw";
    mapping.itemSignature = "Lzw;";
    mapping.itemArmorName = "yq";
    mapping.itemArmorSignature = "Lyq;";
    mapping.inventoryPlayerName = "yx";
    mapping.inventoryPlayerSignature = "Lyx;";

    return mapping;
}

// Mirrors the profile-sharing strategy used by mature multi-client mapping
// systems: a client marker and a symbol namespace are separate concerns. Many
// Lunar 1.8.9 releases keep the canonical Notch namespace, so they can safely
// reuse the verified vanilla dictionary while transformed releases continue to
// fail closed until their exact dictionary is registered through the provider
// interface.
[[nodiscard]] MappingDictionary lunarLegacyNotchDictionary()
{
    MappingDictionary mapping = vanillaNotchDictionary();
    mapping.id = "minecraft-1.8.9-lunar-notch";
    mapping.label = "Lunar 1.8.9 (vanilla namespace)";
    mapping.family = ClientFamily::Lunar;
    mapping.detection = {
        {DetectionMatch::ClassSignaturePrefix, "Lcom/lunarclient/", 250U},
        {DetectionMatch::ClassSignaturePrefix, "Lcom/moonsworth/", 250U},
        {DetectionMatch::LaunchHintContains, ".lunarclient", 260U},
        {DetectionMatch::LaunchHintContains, "lunar", 240U}};
    return mapping;
}

// Lunar's current 1.8.9 line does not use the Forge SRG member namespace.
// Its transformed Minecraft classes keep the readable MCP class and member
// names (for example net.minecraft.client.Minecraft::thePlayer).  This is a
// separate dictionary from Forge even though both expose the same readable
// class names: choosing Forge's func_/field_ symbols was the reason a Lunar
// class anchor was detected but every binding lookup subsequently failed.
//
// The names below are the 1.8.9 MCP names also present in the public Lunar
// Mapping Project's v1_8_9 mixin mappings.  resolveProfile still validates
// every class, field, method and signature before publishing the cache, so a
// future Lunar transformer update fails closed rather than calling a guessed
// member.
[[nodiscard]] MappingDictionary lunarMcpDictionary()
{
    MappingDictionary mapping = forgeSrgDictionary();
    mapping.id = "minecraft-1.8.9-lunar-mcp";
    mapping.label = "Lunar 1.8.9 (MCP named)";
    mapping.family = ClientFamily::Lunar;
    mapping.detection = {
        {DetectionMatch::ClassSignaturePrefix, "Lcom/lunarclient/", 250U},
        {DetectionMatch::ClassSignaturePrefix, "Lcom/moonsworth/", 250U},
        {DetectionMatch::LaunchHintContains, ".lunarclient", 260U},
        {DetectionMatch::LaunchHintContains, "lunar", 240U}};
    mapping.getMinecraft.clear();
    mapping.minecraftInstanceField = "theMinecraft";
    mapping.playerField = "thePlayer";
    mapping.getHealth = "getHealth";
    mapping.getMaxHealth = "getMaxHealth";
    mapping.getEntityId = "getEntityId";
    mapping.getBounds = "getEntityBoundingBox";
    mapping.isMainThread = "isCallingFromMinecraftThread";
    mapping.isSingleplayer = "isSingleplayer";
    mapping.worldField = "theWorld";
    mapping.positionFields = {"posX", "posY", "posZ"};
    mapping.previousPositionFields = {"lastTickPosX", "lastTickPosY", "lastTickPosZ"};
    mapping.getLoadedEntities.clear();
    mapping.loadedEntitiesField = "loadedEntityList";
    mapping.playerEntitiesField = "playerEntities";
    mapping.getName = "getName";
    mapping.getDisplayName = "getDisplayName";
    mapping.getFormattedText = "getFormattedText";
    mapping.addChatMessage = "addChatComponentMessage";
    mapping.getBlockState = "getBlockState";
    mapping.getBlock = "getBlock";
    mapping.getBlockMetadata = "getMetaFromState";
    mapping.setIngameFocus = "setIngameFocus";
    mapping.setIngameNotInFocus = "setIngameNotInFocus";
    mapping.getChunkProvider = "getChunkProvider";
    mapping.chunkListingField = "chunkListing";
    mapping.chunkCoordinateFields = {"xPosition", "zPosition"};
    mapping.getStorageArrays = "getBlockStorageArray";
    mapping.getStorageData = "getData";
    mapping.getRenderManager = "getRenderManager";
    mapping.renderPositionFields = {"renderPosX", "renderPosY", "renderPosZ"};
    mapping.activeModelViewField = "MODELVIEW";
    mapping.activeProjectionField = "PROJECTION";
    mapping.activeViewportField = "VIEWPORT";
    mapping.timerField = "timer";
    mapping.renderPartialTicksField = "renderPartialTicks";
    mapping.aabbFields = {"minX", "minY", "minZ", "maxX", "maxY", "maxZ"};

    mapping.getScoreboard = "getScoreboard";
    mapping.getObjectiveInDisplaySlot = "getObjectiveInDisplaySlot";
    mapping.getPlayersTeam = "getPlayersTeam";
    mapping.getSortedScores = "getSortedScores";
    mapping.getPlayerName = "getPlayerName";
    mapping.formatPlayerName = "formatPlayerName";
    mapping.inventoryField = "inventory";
    mapping.armorInventoryField = "armorInventory";
    mapping.getItem = "getItem";
    mapping.hasColor = "hasColor";
    mapping.getColor = "getColor";

    return mapping;
}

[[nodiscard]] std::unique_ptr<MappingProvider> makeProvider(
    std::string id,
    const ClientFamily family,
    const int priority,
    std::vector<DetectionPattern> detection,
    std::vector<MappingDictionary> dictionaries)
{
    return std::make_unique<OwnedMappingProvider>(
        std::move(id), family, priority,
        std::move(detection), std::move(dictionaries));
}

} // namespace

const char* clientFamilyName(const ClientFamily family) noexcept
{
    switch (family) {
    case ClientFamily::Vanilla: return "Vanilla";
    case ClientFamily::Forge: return "Forge";
    case ClientFamily::Lunar: return "Lunar";
    case ClientFamily::Unknown: return "Unknown";
    }
    return "Unknown";
}

void ClientEnvironment::addEvidence(const ClientFamily family,
                                    const std::uint16_t confidenceValue) noexcept
{
    const std::size_t index = static_cast<std::size_t>(family);
    if (family == ClientFamily::Unknown || index >= m_confidence.size()) {
        return;
    }
    m_confidence[index] = std::max(m_confidence[index], confidenceValue);
}

ClientFamily ClientEnvironment::family() const noexcept
{
    ClientFamily result = ClientFamily::Unknown;
    std::uint16_t strongest = 0U;
    // Deliberate tie order: a positively detected transformed client must not
    // silently fall back to Forge merely because it shares a named anchor.
    for (const ClientFamily candidate :
         {ClientFamily::Vanilla, ClientFamily::Forge, ClientFamily::Lunar}) {
        const std::uint16_t value = confidence(candidate);
        if (value >= strongest && value != 0U) {
            strongest = value;
            result = candidate;
        }
    }
    return result;
}

std::uint16_t ClientEnvironment::confidence(const ClientFamily family) const noexcept
{
    const std::size_t index = static_cast<std::size_t>(family);
    return index < m_confidence.size() ? m_confidence[index] : 0U;
}

bool MappingDictionary::validate(std::string* const error) const noexcept
{
    auto reject = [&](const std::string_view message) noexcept {
        assignError(error, message);
        return false;
    };
    if (!validIdentifier(id)) return reject("mapping id must be 1-64 ASCII identifier characters");
    if (label.empty() || label.size() > kMaxLabelLength) return reject("mapping label is empty or too long");
    if (family == ClientFamily::Unknown) return reject("mapping client family is unknown");
    if (detection.size() > kMaxDetectionPatterns) return reject("too many mapping detection patterns");
    for (const DetectionPattern& pattern : detection) {
        if (pattern.value.empty() || pattern.value.size() > kMaxSymbolLength ||
            pattern.confidence == 0U) {
            return reject("invalid mapping detection pattern");
        }
        if (pattern.match != DetectionMatch::LaunchHintContains && pattern.value.front() != 'L') {
            return reject("class detection patterns must use JVM object signatures");
        }
    }

    // Core classes define whether the client profile itself is usable.  BedWars
    // sidebar/armor support is an optional capability and must never turn an
    // otherwise valid Minecraft profile into "unsupported client mappings".
    const std::array<std::pair<std::string_view, std::string_view>, 18U> coreClasses{{
        {minecraftName, minecraftSignature}, {playerName, playerSignature},
        {livingName, livingSignature}, {entityName, entitySignature},
        {aabbName, aabbSignature}, {worldName, worldSignature},
        {worldClientName, worldClientSignature}, {stateName, stateSignature},
        {blockName, blockSignature}, {blockPosName, blockPosSignature},
        {bedName, bedSignature}, {chunkProviderName, chunkProviderSignature},
        {chunkName, chunkSignature}, {storageName, storageSignature},
        {activeRenderInfoName, activeRenderInfoSignature},
        {renderManagerName, renderManagerSignature},
        {timerName, timerSignature}, {chatComponentName, chatComponentSignature}}};
    for (const auto& [binaryName, signature] : coreClasses) {
        if (!signatureMatchesBinaryName(binaryName, signature)) {
            return reject("core class binary name and JNI signature do not match");
        }
    }

    const std::array<std::pair<std::string_view, std::string_view>, 9U> featureClasses{{
        {scoreboardName, scoreboardSignature}, {scoreObjectiveName, scoreObjectiveSignature},
        {scoreName, scoreSignature}, {scorePlayerTeamName, scorePlayerTeamSignature},
        {itemStackName, itemStackSignature}, {itemName, itemSignature},
        {itemArmorName, itemArmorSignature}, {inventoryPlayerName, inventoryPlayerSignature},
        {chatTextName, chatTextSignature}}};
    for (const auto& [binaryName, signature] : featureClasses) {
        // A feature class may be omitted by an external/core-only mapping pack,
        // but a partially specified pair is still malformed.
        if (binaryName.empty() && signature.empty()) continue;
        if (binaryName.empty() || signature.empty() ||
            !signatureMatchesBinaryName(binaryName, signature)) {
            return reject("optional feature class binary name and JNI signature do not match");
        }
    }
    if (chunkProviderInterfaceSignature.size() < 3U ||
        chunkProviderInterfaceSignature.front() != 'L' ||
        chunkProviderInterfaceSignature.back() != ';') {
        return reject("invalid chunk-provider interface JNI signature");
    }

    if ((getMinecraft.empty() && minecraftInstanceField.empty()) ||
        (!getMinecraft.empty() && !minecraftInstanceField.empty())) {
        return reject("mapping must define exactly one Minecraft singleton accessor");
    }
    if ((getLoadedEntities.empty() && loadedEntitiesField.empty()) ||
        (!getLoadedEntities.empty() && !loadedEntitiesField.empty())) {
        return reject("mapping must define exactly one loaded-entity accessor");
    }
    const std::array<std::string_view, 42U> members{{
        playerField, getHealth, getMaxHealth, getEntityId,
        getBounds, isMainThread, isSingleplayer, worldField, getLoadedEntities,
        playerEntitiesField, getName, getDisplayName, getFormattedText, addChatMessage,
        getBlockState, getBlock, getBlockMetadata, setIngameFocus,
        setIngameNotInFocus, getChunkProvider, chunkListingField,
        getStorageArrays, getStorageData, getRenderManager,
        activeModelViewField, activeProjectionField, activeViewportField,
        timerField, renderPartialTicksField, minecraftInstanceField,
        loadedEntitiesField, getScoreboard, getObjectiveInDisplaySlot,
        getPlayersTeam, getSortedScores, getPlayerName, formatPlayerName,
        inventoryField, armorInventoryField,
        getItem, hasColor, getColor}};
    for (const std::string_view member : members) {
        if (!member.empty() && !validMemberName(member))
            return reject("invalid method or field name");
    }
    for (const std::string& member : positionFields) {
        if (!validMemberName(member)) return reject("invalid position field name");
    }
    for (const std::string& member : previousPositionFields) {
        if (!validMemberName(member)) return reject("invalid previous-position field name");
    }
    for (const std::string& member : chunkCoordinateFields) {
        if (!validMemberName(member)) return reject("invalid chunk coordinate field name");
    }
    for (const std::string& member : renderPositionFields) {
        if (!validMemberName(member)) return reject("invalid render-position field name");
    }
    for (const std::string& member : aabbFields) {
        if (!validMemberName(member)) return reject("invalid AABB field name");
    }
    assignError(error, {});
    return true;
}

MappingRegistry::MappingRegistry() noexcept
{
    try {
        std::vector<MappingDictionary> forgeMappings;
        forgeMappings.push_back(forgeSrgDictionary());
        std::vector<MappingDictionary> vanillaMappings;
        vanillaMappings.push_back(vanillaNotchDictionary());
        std::vector<MappingDictionary> lunarMappings;
        lunarMappings.push_back(lunarMcpDictionary());
        lunarMappings.push_back(lunarLegacyNotchDictionary());

        std::string ignored;
        if (registerProvider(makeProvider("builtin-forge", ClientFamily::Forge, 100,
                                          {}, std::move(forgeMappings)), &ignored) !=
                MappingRegistrationResult::Accepted ||
            registerProvider(makeProvider("builtin-vanilla", ClientFamily::Vanilla, 100,
                                          {}, std::move(vanillaMappings)), &ignored) !=
                MappingRegistrationResult::Accepted ||
            registerProvider(makeProvider(
                "builtin-lunar-legacy", ClientFamily::Lunar, 1000,
                {{DetectionMatch::ClassSignaturePrefix, "Lcom/lunarclient/", 250U},
                 {DetectionMatch::ClassSignaturePrefix, "Lcom/moonsworth/", 250U},
                 {DetectionMatch::LaunchHintContains, ".lunarclient", 260U},
                 {DetectionMatch::LaunchHintContains, "lunar", 240U}},
                std::move(lunarMappings)), &ignored) != MappingRegistrationResult::Accepted) {
            m_healthy = false;
        }
    } catch (...) {
        m_healthy = false;
        m_providers.clear();
    }
}

MappingRegistry::~MappingRegistry() = default;

void MappingRegistry::setError(std::string* const error,
                               const std::string_view message) const noexcept
{
    assignError(error, message);
}

bool MappingRegistry::containsDictionaryId(const std::string_view id) const noexcept
{
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        if (provider != nullptr) {
            if (provider->id() == id) return true;
            for (const MappingDictionary& dictionary : provider->dictionaries()) {
                if (dictionary.id == id) return true;
            }
        }
    }
    return false;
}

MappingRegistrationResult MappingRegistry::registerDictionary(
    MappingDictionary dictionary, std::string* const error) noexcept
{
    std::string validationError;
    if (!dictionary.validate(&validationError)) {
        setError(error, validationError);
        return MappingRegistrationResult::Invalid;
    }
    try {
        const std::string providerId = std::string("external-") + dictionary.id;
        const ClientFamily family = dictionary.family;
        std::vector<MappingDictionary> dictionaries;
        dictionaries.push_back(std::move(dictionary));
        return registerProvider(makeProvider(providerId, family, 500,
                                             {}, std::move(dictionaries)), error);
    } catch (...) {
        setError(error, "out of memory while registering mapping dictionary");
        return MappingRegistrationResult::OutOfMemory;
    }
}

MappingRegistrationResult MappingRegistry::registerProvider(
    std::unique_ptr<MappingProvider> provider, std::string* const error) noexcept
{
    if (provider == nullptr || !validIdentifier(provider->id()) ||
        provider->family() == ClientFamily::Unknown) {
        setError(error, "invalid mapping provider metadata");
        return MappingRegistrationResult::Invalid;
    }
    const std::span<const MappingDictionary> dictionaries = provider->dictionaries();
    if (dictionaries.size() > kMaxDictionariesPerProvider) {
        setError(error, "mapping provider dictionary capacity exceeded");
        return MappingRegistrationResult::CapacityExceeded;
    }
    for (const MappingDictionary& dictionary : dictionaries) {
        std::string validationError;
        if (dictionary.family != provider->family() || !dictionary.validate(&validationError)) {
            setError(error, validationError.empty()
                ? "mapping dictionary family does not match provider" : validationError);
            return MappingRegistrationResult::Invalid;
        }
    }

    std::lock_guard lock(m_mutex);
    if (m_frozen) {
        setError(error, "mapping registry is already frozen");
        return MappingRegistrationResult::Frozen;
    }
    if (m_providers.size() >= kMaxProviders) {
        setError(error, "mapping provider capacity exceeded");
        return MappingRegistrationResult::CapacityExceeded;
    }
    if (containsDictionaryId(provider->id())) {
        setError(error, "duplicate mapping provider id");
        return MappingRegistrationResult::Duplicate;
    }
    for (const MappingDictionary& dictionary : dictionaries) {
        if (containsDictionaryId(dictionary.id)) {
            setError(error, "duplicate mapping dictionary id");
            return MappingRegistrationResult::Duplicate;
        }
    }
    try {
        m_providers.push_back(std::move(provider));
    } catch (...) {
        setError(error, "out of memory while storing mapping provider");
        return MappingRegistrationResult::OutOfMemory;
    }
    setError(error, {});
    return MappingRegistrationResult::Accepted;
}

bool MappingRegistry::freeze() noexcept
{
    std::lock_guard lock(m_mutex);
    if (!m_healthy) {
        return false;
    }
    if (!m_frozen) {
        std::sort(m_providers.begin(), m_providers.end(),
                  [](const std::unique_ptr<MappingProvider>& left,
                     const std::unique_ptr<MappingProvider>& right) noexcept {
            return left->priority() > right->priority();
        });
        m_frozen = true;
    }
    return true;
}

bool MappingRegistry::healthy() const noexcept
{
    std::lock_guard lock(m_mutex);
    return m_healthy;
}

void MappingRegistry::observeClassSignature(const std::string_view signature,
                                            ClientEnvironment& environment) const noexcept
{
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        provider->observeClassSignature(signature, environment);
    }

    // An unambiguous anchor is weak evidence. If multiple client families use
    // the same anchor, launch/class markers decide; with no markers the
    // resolver safely tries all exact-anchor candidates once.
    ClientFamily anchorFamily = ClientFamily::Unknown;
    bool ambiguous = false;
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        for (const MappingDictionary& dictionary : provider->dictionaries()) {
            if (dictionary.minecraftSignature == signature) {
                if (anchorFamily == ClientFamily::Unknown) {
                    anchorFamily = provider->family();
                } else if (anchorFamily != provider->family()) {
                    ambiguous = true;
                }
            }
        }
    }
    if (!ambiguous && anchorFamily != ClientFamily::Unknown) {
        environment.addEvidence(anchorFamily, 50U);
    }
}

void MappingRegistry::observeLaunchHint(const std::string_view hint,
                                        ClientEnvironment& environment) const noexcept
{
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        provider->observeLaunchHint(hint, environment);
    }
}

bool MappingRegistry::hasMappingsForFamily(const ClientFamily family) const noexcept
{
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        if (provider->family() == family && !provider->dictionaries().empty()) {
            return true;
        }
    }
    return false;
}

bool MappingRegistry::isMinecraftAnchor(const std::string_view signature) const noexcept
{
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        for (const MappingDictionary& dictionary : provider->dictionaries()) {
            if (dictionary.minecraftSignature == signature) return true;
        }
    }
    return false;
}

MappingCandidates MappingRegistry::candidatesForAnchor(
    const std::string_view signature,
    const ClientEnvironment& environment) const noexcept
{
    MappingCandidates result;
    const ClientFamily selectedFamily = environment.family();
    for (const std::unique_ptr<MappingProvider>& provider : m_providers) {
        if (selectedFamily != ClientFamily::Unknown && provider->family() != selectedFamily) {
            continue;
        }
        for (const MappingDictionary& dictionary : provider->dictionaries()) {
            if (dictionary.minecraftSignature != signature) continue;
            if (result.count < result.items.size()) {
                result.items[result.count++] = &dictionary;
            } else {
                result.truncated = true;
            }
        }
    }
    return result;
}

} // namespace mcoverlay::bindings
