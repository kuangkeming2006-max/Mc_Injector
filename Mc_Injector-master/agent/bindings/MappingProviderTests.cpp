#include "MappingProvider.h"

#include <cstdio>
#include <string>
#include <utility>

namespace {

using namespace mcoverlay::bindings;

[[nodiscard]] MappingDictionary testLunarDictionary()
{
    MappingDictionary mapping;
    mapping.id = "test-lunar-1.8.9";
    mapping.label = "Test Lunar mapping";
    mapping.family = ClientFamily::Lunar;
    mapping.detection = {
        {DetectionMatch::LaunchHintContains, ".lunarclient", 250U}};
    mapping.minecraftName = "example.lunar.Minecraft";
    mapping.minecraftSignature = "Lexample/lunar/Minecraft;";
    mapping.playerName = "example.lunar.Player";
    mapping.playerSignature = "Lexample/lunar/Player;";
    mapping.livingName = "example.lunar.Living";
    mapping.livingSignature = "Lexample/lunar/Living;";
    mapping.entityName = "example.lunar.Entity";
    mapping.entitySignature = "Lexample/lunar/Entity;";
    mapping.aabbName = "example.lunar.Box";
    mapping.aabbSignature = "Lexample/lunar/Box;";
    mapping.worldName = "example.lunar.World";
    mapping.worldSignature = "Lexample/lunar/World;";
    mapping.worldClientName = "example.lunar.WorldClient";
    mapping.worldClientSignature = "Lexample/lunar/WorldClient;";
    mapping.stateName = "example.lunar.State";
    mapping.stateSignature = "Lexample/lunar/State;";
    mapping.blockName = "example.lunar.Block";
    mapping.blockSignature = "Lexample/lunar/Block;";
    mapping.blockPosName = "example.lunar.BlockPos";
    mapping.blockPosSignature = "Lexample/lunar/BlockPos;";
    mapping.bedName = "example.lunar.Bed";
    mapping.bedSignature = "Lexample/lunar/Bed;";
    mapping.chunkProviderName = "example.lunar.ChunkProvider";
    mapping.chunkProviderSignature = "Lexample/lunar/ChunkProvider;";
    mapping.chunkProviderInterfaceSignature = "Lexample/lunar/IChunkProvider;";
    mapping.chunkName = "example.lunar.Chunk";
    mapping.chunkSignature = "Lexample/lunar/Chunk;";
    mapping.storageName = "example.lunar.Storage";
    mapping.storageSignature = "Lexample/lunar/Storage;";
    mapping.activeRenderInfoName = "example.lunar.ActiveRenderInfo";
    mapping.activeRenderInfoSignature = "Lexample/lunar/ActiveRenderInfo;";
    mapping.renderManagerName = "example.lunar.RenderManager";
    mapping.renderManagerSignature = "Lexample/lunar/RenderManager;";
    mapping.timerName = "example.lunar.Timer";
    mapping.timerSignature = "Lexample/lunar/Timer;";
    mapping.chatComponentName = "example.lunar.Chat";
    mapping.chatComponentSignature = "Lexample/lunar/Chat;";
    mapping.getMinecraft = "instance";
    mapping.playerField = "player";
    mapping.getHealth = "health";
    mapping.getMaxHealth = "maxHealth";
    mapping.getEntityId = "entityId";
    mapping.getBounds = "bounds";
    mapping.isMainThread = "isMainThread";
    mapping.isSingleplayer = "isSingleplayer";
    mapping.worldField = "world";
    mapping.positionFields = {"x", "y", "z"};
    mapping.previousPositionFields = {"previousX", "previousY", "previousZ"};
    mapping.getLoadedEntities = "loadedEntities";
    mapping.playerEntitiesField = "players";
    mapping.getName = "name";
    mapping.getDisplayName = "displayName";
    mapping.getFormattedText = "formattedText";
    mapping.getBlockState = "blockState";
    mapping.getBlock = "block";
    mapping.getBlockMetadata = "metadata";
    mapping.setIngameFocus = "focus";
    mapping.setIngameNotInFocus = "unfocus";
    mapping.getChunkProvider = "chunkProvider";
    mapping.chunkListingField = "chunks";
    mapping.chunkCoordinateFields = {"chunkX", "chunkZ"};
    mapping.getStorageArrays = "sections";
    mapping.getStorageData = "data";
    mapping.getRenderManager = "renderManager";
    mapping.renderPositionFields = {"renderX", "renderY", "renderZ"};
    mapping.activeModelViewField = "modelView";
    mapping.activeProjectionField = "projection";
    mapping.activeViewportField = "viewport";
    mapping.timerField = "timer";
    mapping.renderPartialTicksField = "partialTicks";
    mapping.aabbFields = {"minX", "minY", "minZ", "maxX", "maxY", "maxZ"};
    return mapping;
}

[[nodiscard]] bool expect(const bool condition, const char* const message)
{
    if (!condition) std::fprintf(stderr, "FAILED: %s\n", message);
    return condition;
}

} // namespace

int main()
{
    using namespace mcoverlay::bindings;
    bool passed = true;

    MappingRegistry builtins;
    passed &= expect(builtins.healthy(), "builtin registry is healthy");
    passed &= expect(builtins.freeze(), "builtin registry freezes");
    ClientEnvironment forge;
    builtins.observeClassSignature("Lnet/minecraftforge/common/MinecraftForge;", forge);
    builtins.observeClassSignature("Lnet/minecraft/client/Minecraft;", forge);
    const MappingCandidates forgeCandidates = builtins.candidatesForAnchor(
        "Lnet/minecraft/client/Minecraft;", forge);
    passed &= expect(forge.family() == ClientFamily::Forge, "Forge environment detection");
    passed &= expect(forgeCandidates.count == 1U, "Forge provider selection");

    MappingRegistry lunarBuiltins;
    ClientEnvironment lunar;
    lunarBuiltins.observeLaunchHint(
        "C:/Users/test/.lunarclient/jre/bin/javaw.exe", lunar);
    lunarBuiltins.observeClassSignature("Lave;", lunar);
    const MappingCandidates lunarCandidates = lunarBuiltins.candidatesForAnchor(
        "Lave;", lunar);
    passed &= expect(lunar.family() == ClientFamily::Lunar, "Lunar launch-hint detection");
    passed &= expect(lunarBuiltins.hasMappingsForFamily(ClientFamily::Lunar),
                     "Lunar legacy namespace is registered");
    passed &= expect(lunarCandidates.count == 1U,
                     "Lunar legacy provider reuses the verified Notch namespace");

    MappingRegistry lunarNamedBuiltins;
    ClientEnvironment lunarNamed;
    lunarNamedBuiltins.observeLaunchHint(
        "C:/Users/test/.lunarclient/jre/bin/javaw.exe", lunarNamed);
    lunarNamedBuiltins.observeClassSignature(
        "Lnet/minecraft/client/Minecraft;", lunarNamed);
    const MappingCandidates lunarNamedCandidates =
        lunarNamedBuiltins.candidatesForAnchor(
            "Lnet/minecraft/client/Minecraft;", lunarNamed);
    passed &= expect(lunarNamed.family() == ClientFamily::Lunar,
                     "Lunar named environment detection");
    passed &= expect(lunarNamedCandidates.count == 1U,
                     "Lunar MCP-named provider selection");
    passed &= expect(lunarNamedCandidates.items[0] != nullptr &&
                         lunarNamedCandidates.items[0]->id ==
                             "minecraft-1.8.9-lunar-mcp",
                     "Lunar named anchor selects the MCP dictionary");
    passed &= expect(lunarNamedCandidates.items[0] != nullptr &&
                         lunarNamedCandidates.items[0]->minecraftInstanceField ==
                             "theMinecraft" &&
                         lunarNamedCandidates.items[0]->loadedEntitiesField ==
                             "loadedEntityList",
                     "Lunar named dictionary uses verified static/list fields");

    MappingRegistry external;
    std::string error;
    MappingDictionary lunarMapping = testLunarDictionary();
    passed &= expect(lunarMapping.validate(&error), "valid external dictionary");
    passed &= expect(external.registerDictionary(std::move(lunarMapping), &error) ==
                         MappingRegistrationResult::Accepted,
                     "external dictionary registration");
    passed &= expect(external.freeze(), "external registry freezes");
    ClientEnvironment externalLunar;
    external.observeLaunchHint(".lunarclient", externalLunar);
    external.observeClassSignature("Lexample/lunar/Minecraft;", externalLunar);
    const MappingCandidates externalCandidates = external.candidatesForAnchor(
        "Lexample/lunar/Minecraft;", externalLunar);
    passed &= expect(externalCandidates.count == 1U, "external Lunar provider selection");

    MappingDictionary lateMapping = testLunarDictionary();
    lateMapping.id = "test-lunar-late";
    passed &= expect(external.registerDictionary(std::move(lateMapping), &error) ==
                         MappingRegistrationResult::Frozen,
                     "registration is rejected after freeze");

    MappingRegistry invalidRegistry;
    MappingDictionary invalid = testLunarDictionary();
    invalid.minecraftSignature = "Lwrong/Signature;";
    passed &= expect(invalidRegistry.registerDictionary(std::move(invalid), &error) ==
                         MappingRegistrationResult::Invalid,
                     "invalid JNI signature is rejected");

    if (passed) std::puts("Mapping provider tests passed.");
    return passed ? 0 : 1;
}
