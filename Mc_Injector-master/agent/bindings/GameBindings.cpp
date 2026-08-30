#include "GameBindings.h"

#include "src/AgentLog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcoverlay {

// Resolver-owned until publication. Every field is immutable after
// m_resolutionPhase is release-stored as Resolved. The render thread performs
// an acquire load before dereferencing m_cache, so it can never observe a
// partially initialized JNI cache.
struct GameBindings::BindingCache final {
    jclass minecraftClass = nullptr;
    jclass playerClass = nullptr;
    jclass livingClass = nullptr;
    jclass entityClass = nullptr;
    jclass fireballClass = nullptr;
    jclass aabbClass = nullptr;
    jclass worldClass = nullptr;
    jclass worldClientClass = nullptr;
    jclass stateClass = nullptr;
    jclass blockClass = nullptr;
    jclass blockPosClass = nullptr;
    jclass bedClass = nullptr;
    jclass chunkProviderClass = nullptr;
    jclass chunkClass = nullptr;
    jclass storageClass = nullptr;
    jclass activeRenderInfoClass = nullptr;
    jclass renderManagerClass = nullptr;
    jclass timerClass = nullptr;
    jclass gameSettingsClass = nullptr;
    jclass keyBindingClass = nullptr;
    jclass playerControllerClass = nullptr;
    jclass serverDataClass = nullptr;
    jclass itemBlockClass = nullptr;
    jclass enumFacingClass = nullptr;
    jclass vec3Class = nullptr;
    jclass chatComponentClass = nullptr;
    jclass chatTextClass = nullptr;
    jclass chatSerializerClass = nullptr;
    jobject renderManagerObject = nullptr;
    jobject timerObject = nullptr;
    jobject modelViewBuffer = nullptr;
    jobject projectionBuffer = nullptr;
    jobject viewportBuffer = nullptr;

    jmethodID getMinecraft = nullptr;
    jfieldID minecraftInstanceField = nullptr;
    jfieldID playerField = nullptr;
    jmethodID getHealth = nullptr;
    jmethodID getMaxHealth = nullptr;
    jmethodID getEntityId = nullptr;
    jmethodID getBounds = nullptr;
    jmethodID isMainThread = nullptr;
    jmethodID isSingleplayer = nullptr;
    jfieldID worldField = nullptr;
    jfieldID positionX = nullptr;
    jfieldID positionY = nullptr;
    jfieldID positionZ = nullptr;
    std::array<jfieldID, 3U> previousPosition{};
    jmethodID getLoadedEntities = nullptr;
    jfieldID loadedEntitiesField = nullptr;
    jfieldID playerEntities = nullptr;
    jmethodID getName = nullptr;
    jmethodID isInvisible = nullptr;
    jmethodID getDisplayName = nullptr;
    jmethodID getFormattedText = nullptr;
    jmethodID chatTextConstructor = nullptr;
    jmethodID addChatMessage = nullptr;
    jmethodID parseChatJson = nullptr;
    jmethodID listSize = nullptr;
    jmethodID listGet = nullptr;
    jmethodID blockPosConstructor = nullptr;
    jmethodID getBlockState = nullptr;
    jmethodID getBlock = nullptr;
    jmethodID getBlockMetadata = nullptr;
    jmethodID setIngameFocus = nullptr;
    jmethodID setIngameNotInFocus = nullptr;
    jmethodID getChunkProvider = nullptr;
    jfieldID chunkListingField = nullptr;
    jfieldID chunkX = nullptr;
    jfieldID chunkZ = nullptr;
    jmethodID getStorageArrays = nullptr;
    jmethodID getStorageData = nullptr;
    jmethodID listToArray = nullptr;
    jmethodID collectionToArray = nullptr;
    jmethodID getRenderManager = nullptr;
    std::array<jfieldID, 3U> renderPosition{};
    jfieldID activeModelView = nullptr;
    jfieldID activeProjection = nullptr;
    jfieldID activeViewport = nullptr;
    jfieldID timerField = nullptr;
    jfieldID renderPartialTicks = nullptr;
    jfieldID gameSettingsField = nullptr;
    jfieldID keyBindSneakField = nullptr;
    std::array<jfieldID, 5U> movementKeyFields{};
    jmethodID getKeyCode = nullptr;
    jmethodID setKeyBindState = nullptr;
    jfieldID mouseSensitivity = nullptr;
    jfieldID rotationYaw = nullptr;
    jfieldID rotationPitch = nullptr;
    std::array<jfieldID, 3U> motionFields{};
    jfieldID onGround = nullptr;
    jmethodID jump = nullptr;
    jmethodID isAirBlock = nullptr;
    jmethodID getCurrentServerData = nullptr;
    jfieldID serverIp = nullptr;
    jfieldID playerControllerField = nullptr;
    jfieldID currentItem = nullptr;
    jfieldID mainInventory = nullptr;
    jmethodID getBlockFromItem = nullptr;
    jmethodID getIdFromBlock = nullptr;
    jmethodID getFacingByIndex = nullptr;
    jmethodID onPlayerRightClick = nullptr;
    jmethodID vec3Constructor = nullptr;
    jfieldID minX = nullptr;
    jfieldID minY = nullptr;
    jfieldID minZ = nullptr;
    jfieldID maxX = nullptr;
    jfieldID maxY = nullptr;
    jfieldID maxZ = nullptr;

    jclass scoreboardClass = nullptr;
    jclass scoreObjectiveClass = nullptr;
    jclass scoreClass = nullptr;
    jclass scorePlayerTeamClass = nullptr;
    jclass netHandlerClass = nullptr;
    jclass networkPlayerInfoClass = nullptr;
    jclass gameProfileClass = nullptr;
    jclass itemStackClass = nullptr;
    jclass itemClass = nullptr;
    jclass itemArmorClass = nullptr;
    jclass inventoryPlayerClass = nullptr;
    jclass enchantmentHelperClass = nullptr;
    jclass abstractClientPlayerClass = nullptr;
    jclass resourceLocationClass = nullptr;
    jclass textureManagerClass = nullptr;
    jclass textureObjectClass = nullptr;
    jclass uuidClass = nullptr;

    jmethodID getScoreboard = nullptr;
    jmethodID getObjectiveInDisplaySlot = nullptr;
    jmethodID getPlayersTeam = nullptr;
    jmethodID getSortedScores = nullptr;
    jmethodID getPlayerName = nullptr;
    jmethodID formatPlayerName = nullptr;
    jmethodID getNetHandler = nullptr;
    jmethodID getPlayerInfoMap = nullptr;
    jmethodID getGameProfile = nullptr;
    jmethodID gameProfileGetName = nullptr;
    jmethodID gameProfileGetId = nullptr;
    jmethodID uuidToString = nullptr;
    jfieldID inventoryField = nullptr;
    jfieldID armorInventoryField = nullptr;
    jmethodID getItem = nullptr;
    jmethodID hasColor = nullptr;
    jmethodID getColor = nullptr;
    jmethodID getEnchantmentLevel = nullptr;
    jmethodID getEquipmentInSlot = nullptr;
    jmethodID getIdFromItem = nullptr;
    jfieldID stackSize = nullptr;
    jmethodID getItemDamage = nullptr;
    jmethodID getUniqueId = nullptr;
    jmethodID getLocationSkin = nullptr;
    jmethodID getTextureManager = nullptr;
    jmethodID getTexture = nullptr;
    jmethodID getGlTextureId = nullptr;

    const MappingProfile* profile = nullptr;
};

namespace {

// GetLoadedClasses may require a HotSpot global safepoint even when it is
// invoked from a native helper thread. By the time the controller offers a
// visible Minecraft window the 1.8.9 client class is already loaded, so one
// snapshot is sufficient. Retrying unsupported/transformed clients would only
// introduce visible pauses at regular intervals without changing the result.

template<typename Identifier, typename Lookup>
[[nodiscard]] bool lookupRequired(JNIEnv* const env,
                                  Identifier& identifier,
                                  Lookup&& lookup) noexcept
{
    identifier = lookup();
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        identifier = nullptr;
        return false;
    }
    return identifier != nullptr;
}

class LocalReferenceSet final {
public:
    explicit LocalReferenceSet(JNIEnv* const env) noexcept : m_env(env) {}

    ~LocalReferenceSet()
    {
        if (m_env == nullptr) {
            return;
        }
        while (m_count != 0U) {
            --m_count;
            m_env->DeleteLocalRef(m_references[m_count]);
        }
    }

    void add(jobject const reference) noexcept
    {
        if (reference != nullptr && m_count < m_references.size()) {
            m_references[m_count] = reference;
            ++m_count;
        }
    }

private:
    JNIEnv* m_env = nullptr;
    std::array<jobject, 64U> m_references{};
    std::size_t m_count = 0U;
};

} // namespace

GameBindings::GameBindings(JavaVM* const vm, jvmtiEnv* const jvmti) noexcept
    : m_vm(vm), m_jvmti(jvmti)
{
    // Auto-reset wakeup: normal chunk diffing remains asleep for 500 ms, but a
    // user refresh interrupts that wait immediately without introducing a
    // high-frequency polling loop.
    m_bedRescanEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

GameBindings::~GameBindings()
{
    if (m_bedRescanEvent != nullptr) {
        ::CloseHandle(m_bedRescanEvent);
        m_bedRescanEvent = nullptr;
    }
}

bindings::MappingRegistrationResult GameBindings::registerMappingDictionary(
    bindings::MappingDictionary dictionary, std::string* const error) noexcept
{
    return m_mappingRegistry.registerDictionary(std::move(dictionary), error);
}

void GameBindings::clearException(JNIEnv* const env) const noexcept
{
    if (env != nullptr && env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
    }
}

void GameBindings::runResolver(JNIEnv* const env, HANDLE const stopEvent) noexcept
{
    if (env == nullptr || m_jvmti == nullptr || stopEvent == nullptr) {
        markResolverUnavailable();
        return;
    }

    const DWORD stopped = ::WaitForSingleObject(stopEvent, 0U);
    if (stopped == WAIT_OBJECT_0) {
        m_retryAtMilliseconds.store(0U, std::memory_order_release);
        m_resolutionPhase.store(ResolutionPhase::Stopped, std::memory_order_release);
        return;
    }
    if (stopped == WAIT_FAILED) {
        markResolverUnavailable();
        return;
    }

    m_mappingAttempt.store(1U, std::memory_order_release);
    m_retryAtMilliseconds.store(0U, std::memory_order_release);
    if (resolve(env)) {
        return;
    }

    // A failed lookup must never leak a pending exception into
    // DetachCurrentThread. Rendering remains independent of this result.
    clearException(env);
    m_resolutionPhase.store(ResolutionPhase::Unsupported,
                            std::memory_order_release);
    log::info("Minecraft mappings are unsupported; resolver stopped after one class snapshot.");
}

void GameBindings::markResolverUnavailable() noexcept
{
    m_retryAtMilliseconds.store(0U, std::memory_order_release);
    m_resolutionPhase.store(ResolutionPhase::Unavailable, std::memory_order_release);
}

GameSnapshot GameBindings::snapshot(const std::uint64_t tickMilliseconds) const noexcept
{
    // m_snapshot has one owner: the SwapBuffers render thread. Returning a copy
    // also keeps OverlayRenderer from retaining memory that release() resets.
    GameSnapshot result = m_snapshot;
    const ResolutionPhase phase = m_resolutionPhase.load(std::memory_order_acquire);
    result.mappingAttempt = m_mappingAttempt.load(std::memory_order_acquire);

    const std::uint64_t retryAt = m_retryAtMilliseconds.load(std::memory_order_acquire);
    const std::uint64_t retryRemaining = retryAt > tickMilliseconds
        ? retryAt - tickMilliseconds : 0U;
    result.mappingRetryInMs = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(retryRemaining, UINT32_MAX));

    switch (phase) {
    case ResolutionPhase::Resolved:
        // m_cache and every referenced ID were written before the resolver's
        // release-store of Resolved, so this acquire read makes them visible.
        if (m_cache != nullptr && m_cache->profile != nullptr) {
            result.mapping = m_cache->profile->label.c_str();
        }
        if (result.state == GameSnapshot::State::Resolving ||
            result.state == GameSnapshot::State::Unsupported) {
            result.state = GameSnapshot::State::WaitingForGameThread;
        }
        break;
    case ResolutionPhase::Unsupported:
        result.state = GameSnapshot::State::Unsupported;
        result.mapping = "unsupported client mappings";
        result.mappingRetryInMs = 0U;
        break;
    case ResolutionPhase::Unavailable:
        result.state = GameSnapshot::State::JniError;
        result.mapping = "mapping resolver unavailable";
        result.mappingRetryInMs = 0U;
        break;
    case ResolutionPhase::Stopped:
        result.state = GameSnapshot::State::Unsupported;
        result.mapping = "mapping resolver stopped";
        result.mappingRetryInMs = 0U;
        break;
    case ResolutionPhase::Resolving:
        result.state = GameSnapshot::State::Resolving;
        result.mapping = result.mappingAttempt == 0U
            ? "mapping resolver queued"
            : "probing mapping providers";
        break;
    }
    return result;
}

void GameBindings::probeEnvironmentHints(
    JNIEnv* const env, bindings::ClientEnvironment& environment) noexcept
{
    if (env == nullptr || env->PushLocalFrame(12) < 0) {
        clearException(env);
        return;
    }

    jclass systemClass = env->FindClass("java/lang/System");
    if (env->ExceptionCheck() == JNI_TRUE || systemClass == nullptr) {
        clearException(env);
        env->PopLocalFrame(nullptr);
        return;
    }
    jmethodID getProperty = env->GetStaticMethodID(
        systemClass, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
    if (env->ExceptionCheck() == JNI_TRUE || getProperty == nullptr) {
        clearException(env);
        env->PopLocalFrame(nullptr);
        return;
    }

    // Values are inspected in memory only and are never logged or sent over
    // IPC. java.home commonly identifies Lunar before an expensive class walk.
    constexpr std::array<const char*, 3U> kProperties{
        "java.home", "java.class.path", "sun.java.command"};
    for (const char* const property : kProperties) {
        jstring key = env->NewStringUTF(property);
        if (env->ExceptionCheck() == JNI_TRUE || key == nullptr) {
            clearException(env);
            continue;
        }
        jstring value = static_cast<jstring>(
            env->CallStaticObjectMethod(systemClass, getProperty, key));
        if (env->ExceptionCheck() == JNI_TRUE) {
            clearException(env);
            continue;
        }
        if (value == nullptr) continue;

        const char* utf8 = env->GetStringUTFChars(value, nullptr);
        if (env->ExceptionCheck() == JNI_TRUE || utf8 == nullptr) {
            clearException(env);
            continue;
        }
        m_mappingRegistry.observeLaunchHint(utf8, environment);
        env->ReleaseStringUTFChars(value, utf8);
        if (env->ExceptionCheck() == JNI_TRUE) clearException(env);
    }
    env->PopLocalFrame(nullptr);
}

jclass GameBindings::findMinecraftClass(
    JNIEnv* const env,
    bindings::MappingCandidates& candidates,
    bindings::ClientEnvironment& environment) noexcept
{
    candidates = {};
    if (env == nullptr || m_jvmti == nullptr) {
        return nullptr;
    }

    const bindings::ClientFamily hintedFamily = environment.family();
    if (hintedFamily != bindings::ClientFamily::Unknown &&
        !m_mappingRegistry.hasMappingsForFamily(hintedFamily)) {
        log::info(std::string("Detected ") + bindings::clientFamilyName(hintedFamily) +
                  " client; no verified mapping dictionary is registered.");
        return nullptr;
    }

    jint count = 0;
    jclass* classes = nullptr;
    if (m_jvmti->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE || classes == nullptr) {
        return nullptr;
    }

    // GetLoadedClasses returns JNI local references owned by this native frame.
    // Always delete every entry and deallocate the JVMTI array, including after
    // a match. This is one snapshot for every registered provider/profile.
    jclass result = nullptr;
    bool discoveryFailed = false;
    bool knownAnchorSeen = false;
    bool matchedAnchor = false;
    for (jint index = 0; index < count; ++index) {
        // After finding an anchor which has a dictionary for the detected
        // client family, only release remaining references. A transformed
        // client can load compatibility/wrapper Minecraft classes first; an
        // anchor with no family-compatible dictionary is therefore not enough
        // to stop the one-time scan.
        if (!matchedAnchor && !discoveryFailed) {
            char* candidateSignature = nullptr;
            char* genericSignature = nullptr;
            const jvmtiError signatureResult = m_jvmti->GetClassSignature(
                classes[index], &candidateSignature, &genericSignature);
            if (signatureResult == JVMTI_ERROR_NONE && candidateSignature != nullptr) {
                m_mappingRegistry.observeClassSignature(candidateSignature, environment);
                if (m_mappingRegistry.isMinecraftAnchor(candidateSignature)) {
                    knownAnchorSeen = true;

                    // A custom defining loader is useful client evidence and
                    // costs one lookup rather than another loaded-class pass.
                    jobject loader = nullptr;
                    if (m_jvmti->GetClassLoader(classes[index], &loader) ==
                            JVMTI_ERROR_NONE && loader != nullptr) {
                        jclass loaderClass = env->GetObjectClass(loader);
                        if (env->ExceptionCheck() == JNI_TRUE) {
                            clearException(env);
                            loaderClass = nullptr;
                        }
                        if (loaderClass != nullptr) {
                            char* loaderSignature = nullptr;
                            char* loaderGeneric = nullptr;
                            if (m_jvmti->GetClassSignature(loaderClass, &loaderSignature,
                                                           &loaderGeneric) ==
                                    JVMTI_ERROR_NONE && loaderSignature != nullptr) {
                                m_mappingRegistry.observeClassSignature(loaderSignature,
                                                                         environment);
                            }
                            if (loaderSignature != nullptr) {
                                m_jvmti->Deallocate(
                                    reinterpret_cast<unsigned char*>(loaderSignature));
                            }
                            if (loaderGeneric != nullptr) {
                                m_jvmti->Deallocate(
                                    reinterpret_cast<unsigned char*>(loaderGeneric));
                            }
                            env->DeleteLocalRef(loaderClass);
                        }
                        env->DeleteLocalRef(loader);
                    }

                    candidates = m_mappingRegistry.candidatesForAnchor(
                        candidateSignature, environment);
                    if (!candidates.empty()) {
                        matchedAnchor = true;
                        result = static_cast<jclass>(env->NewLocalRef(classes[index]));
                        if (env->ExceptionCheck() == JNI_TRUE) {
                            env->ExceptionClear();
                            result = nullptr;
                            discoveryFailed = true;
                        }
                    }
                }
            }
            // JVMTI normally allocates outputs only on success, but releasing
            // any non-null buffer also makes unusual error paths leak-free.
            if (candidateSignature != nullptr) {
                m_jvmti->Deallocate(reinterpret_cast<unsigned char*>(candidateSignature));
            }
            if (genericSignature != nullptr) {
                m_jvmti->Deallocate(reinterpret_cast<unsigned char*>(genericSignature));
            }
        }
        env->DeleteLocalRef(classes[index]);
    }
    m_jvmti->Deallocate(reinterpret_cast<unsigned char*>(classes));
    if (knownAnchorSeen && candidates.empty()) {
        log::info(std::string("Minecraft anchor found for ") +
                  bindings::clientFamilyName(environment.family()) +
                  "; no verified provider matched the detected environment.");
    }
    return result;
}

jclass GameBindings::loadWithClassLoader(JNIEnv* const env,
                                         jobject const loader,
                                         const jmethodID loadClass,
                                         const char* const binaryName) noexcept
{
    if (env == nullptr || loader == nullptr || loadClass == nullptr || binaryName == nullptr) {
        return nullptr;
    }

    // Minecraft itself was found in the JVMTI snapshot first, so asking its
    // exact defining loader for dependencies cannot accidentally start the
    // game from Forge's splash context. ClassLoader.loadClass does not run a
    // class initializer and avoids another full JVMTI table walk per class.
    jclass result = nullptr;
    jstring name = env->NewStringUTF(binaryName);
    if (env->ExceptionCheck() == JNI_TRUE || name == nullptr) {
        clearException(env);
        return nullptr;
    }
    result = static_cast<jclass>(env->CallObjectMethod(loader, loadClass, name));
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        if (result != nullptr) {
            env->DeleteLocalRef(result);
        }
        result = nullptr;
    }
    env->DeleteLocalRef(name);
    return result;
}

bool GameBindings::resolveProfile(JNIEnv* const env,
                                  const MappingProfile& profile,
                                  jclass const minecraft,
                                  BindingCache& candidate)
{
    if (env == nullptr || minecraft == nullptr) {
        return false;
    }

    LocalReferenceSet localReferences(env);
    jobject minecraftLoader = nullptr;
    if (m_jvmti->GetClassLoader(minecraft, &minecraftLoader) != JVMTI_ERROR_NONE ||
        minecraftLoader == nullptr) {
        clearException(env);
        return false;
    }
    localReferences.add(minecraftLoader);

    jclass loaderClass = nullptr;
    if (!lookupRequired(env, loaderClass,
                        [&] { return env->FindClass("java/lang/ClassLoader"); })) {
        return false;
    }
    localReferences.add(loaderClass);
    jmethodID loadClassMethod = nullptr;
    if (!lookupRequired(env, loadClassMethod, [&] {
            return env->GetMethodID(loaderClass, "loadClass",
                                    "(Ljava/lang/String;)Ljava/lang/Class;");
        })) {
        return false;
    }

    auto loadClass = [&](jclass& destination, const char* const binaryName) noexcept {
        destination = loadWithClassLoader(env, minecraftLoader, loadClassMethod, binaryName);
        if (destination == nullptr) {
            return false;
        }
        localReferences.add(destination);
        return true;
    };

    jclass player = nullptr;
    jclass living = nullptr;
    jclass entity = nullptr;
    jclass fireball = nullptr;
    jclass aabb = nullptr;
    jclass world = nullptr;
    jclass worldClient = nullptr;
    jclass state = nullptr;
    jclass block = nullptr;
    jclass blockPos = nullptr;
    jclass bed = nullptr;
    jclass chunkProvider = nullptr;
    jclass chunk = nullptr;
    jclass storage = nullptr;
    jclass activeRenderInfo = nullptr;
    jclass renderManager = nullptr;
    jclass timer = nullptr;
    jclass chatComponent = nullptr;
    jclass chatText = nullptr;
    jclass chatSerializer = nullptr;
    jclass scoreboard = nullptr;
    jclass scoreObjective = nullptr;
    jclass score = nullptr;
    jclass scorePlayerTeam = nullptr;
    jclass netHandler = nullptr;
    jclass networkPlayerInfo = nullptr;
    jclass gameProfile = nullptr;
    jclass itemStack = nullptr;
    jclass item = nullptr;
    jclass itemArmor = nullptr;
    jclass inventoryPlayer = nullptr;
    jclass enchantmentHelper = nullptr;
    jclass abstractClientPlayer = nullptr;
    jclass resourceLocation = nullptr;
    jclass textureManager = nullptr;
    jclass textureObject = nullptr;
    jclass uuid = nullptr;
    jclass gameSettings = nullptr;
    jclass keyBinding = nullptr;
    jclass playerController = nullptr;
    jclass serverData = nullptr;
    jclass itemBlock = nullptr;
    jclass enumFacing = nullptr;
    jclass vec3 = nullptr;
    // Only the long-standing game/render bindings are profile-critical.  The
    // BedWars sidebar and armor readers are optional capabilities: a missing or
    // stale auxiliary mapping must not invalidate an otherwise usable client.
    if (!loadClass(player, profile.playerName.c_str()) ||
        !loadClass(living, profile.livingName.c_str()) ||
        !loadClass(entity, profile.entityName.c_str()) ||
        !loadClass(aabb, profile.aabbName.c_str()) ||
        !loadClass(world, profile.worldName.c_str()) ||
        !loadClass(worldClient, profile.worldClientName.c_str()) ||
        !loadClass(state, profile.stateName.c_str()) ||
        !loadClass(block, profile.blockName.c_str()) ||
        !loadClass(blockPos, profile.blockPosName.c_str()) ||
        !loadClass(bed, profile.bedName.c_str()) ||
        !loadClass(chunkProvider, profile.chunkProviderName.c_str()) ||
        !loadClass(chunk, profile.chunkName.c_str()) ||
        !loadClass(storage, profile.storageName.c_str()) ||
        !loadClass(activeRenderInfo, profile.activeRenderInfoName.c_str()) ||
        !loadClass(renderManager, profile.renderManagerName.c_str()) ||
        !loadClass(timer, profile.timerName.c_str()) ||
        !loadClass(chatComponent, profile.chatComponentName.c_str())) {
        log::info(std::string("Core mapping class load failed for profile: ") +
                  profile.label);
        return false;
    }

    auto loadFeatureClass = [&](jclass& destination,
                                const std::string& binaryName,
                                const char* const capability,
                                const char* const logicalName) noexcept {
        if (binaryName.empty()) {
            log::info(std::string(capability) + " mappings unavailable for " +
                      profile.label + ": no " + logicalName + " class mapping.");
            return false;
        }
        destination = loadWithClassLoader(env, minecraftLoader, loadClassMethod,
                                          binaryName.c_str());
        if (destination == nullptr) {
            log::info(std::string(capability) + " mappings unavailable for " +
                      profile.label + ": could not load " + logicalName +
                      " (" + binaryName + ").");
            return false;
        }
        localReferences.add(destination);
        return true;
    };

    const bool fireballClassLoaded = loadFeatureClass(
        fireball, profile.fireballName, "Fireball ESP", "EntityFireball");

    const bool sidebarClassesLoaded =
        loadFeatureClass(scoreboard, profile.scoreboardName, "Sidebar", "Scoreboard") &&
        loadFeatureClass(scoreObjective, profile.scoreObjectiveName, "Sidebar", "ScoreObjective") &&
        loadFeatureClass(score, profile.scoreName, "Sidebar", "Score") &&
        loadFeatureClass(scorePlayerTeam, profile.scorePlayerTeamName, "Sidebar", "ScorePlayerTeam");

    const bool tabClassesLoaded =
        loadFeatureClass(netHandler, profile.netHandlerName, "TAB roster", "NetHandlerPlayClient") &&
        loadFeatureClass(networkPlayerInfo, profile.networkPlayerInfoName,
                         "TAB roster", "NetworkPlayerInfo") &&
        loadFeatureClass(gameProfile, "com.mojang.authlib.GameProfile",
                         "TAB roster", "GameProfile");

    const bool itemClassesLoaded =
        loadFeatureClass(itemStack, profile.itemStackName, "Items", "ItemStack") &&
        loadFeatureClass(item, profile.itemName, "Items", "Item") &&
        loadFeatureClass(inventoryPlayer, profile.inventoryPlayerName,
                         "Items", "InventoryPlayer");

    const bool armorClassesLoaded = itemClassesLoaded &&
        loadFeatureClass(itemArmor, profile.itemArmorName, "Armor", "ItemArmor") &&
        loadFeatureClass(enchantmentHelper, profile.enchantmentHelperName,
                         "Armor", "EnchantmentHelper");

    const bool skinClassesLoaded =
        loadFeatureClass(abstractClientPlayer, profile.abstractClientPlayerName,
                         "Player skin", "AbstractClientPlayer") &&
        loadFeatureClass(resourceLocation, profile.resourceLocationName,
                         "Player skin", "ResourceLocation") &&
        loadFeatureClass(textureManager, profile.textureManagerName,
                         "Player skin", "TextureManager") &&
        loadFeatureClass(textureObject, profile.textureObjectName,
                         "Player skin", "ITextureObject");

    const bool gameSettingsClassLoaded = loadFeatureClass(
        gameSettings, profile.gameSettingsName, "Aim/Movement", "GameSettings");
    const bool keyBindingClassLoaded = loadFeatureClass(
        keyBinding, profile.keyBindingName, "Safewalk", "KeyBinding");
    const bool safewalkClassesLoaded =
        gameSettingsClassLoaded && keyBindingClassLoaded;

    const bool serverDataClassLoaded = loadFeatureClass(
        serverData, profile.serverDataName, "Server guard", "ServerData");
    const bool movementClassesLoaded = safewalkClassesLoaded && itemClassesLoaded &&
        loadFeatureClass(playerController, profile.playerControllerName,
                         "Movement", "PlayerControllerMP") &&
        loadFeatureClass(itemBlock, profile.itemBlockName,
                         "Scaffold", "ItemBlock") &&
        loadFeatureClass(enumFacing, profile.enumFacingName,
                         "Scaffold", "EnumFacing") &&
        loadFeatureClass(vec3, profile.vec3Name, "Scaffold", "Vec3");

    if (lookupRequired(env, uuid, [&] { return env->FindClass("java/util/UUID"); })) {
        localReferences.add(uuid);
    }

    const bool debugChatClassLoaded =
        loadFeatureClass(chatText, profile.chatTextName, "Debug chat", "ChatComponentText");
    const bool richChatClassLoaded = debugChatClassLoaded &&
        loadFeatureClass(chatSerializer, profile.chatSerializerName,
                         "Rich local chat", "IChatComponent.Serializer");

    const std::string getMinecraftSignature = std::string("()") + profile.minecraftSignature;
    const std::string getBoundsSignature = std::string("()") + profile.aabbSignature;
    const std::string getBlockStateSignature =
        std::string("(") + profile.blockPosSignature + ")" + profile.stateSignature;
    const std::string getBlockSignature = std::string("()") + profile.blockSignature;
    const std::string getBlockMetadataSignature =
        std::string("(") + profile.stateSignature + ")I";
    const std::string getStorageArraysSignature =
        std::string("()[") + profile.storageSignature;
    const std::string getRenderManagerSignature =
        std::string("()") + profile.renderManagerSignature;

    const std::string getObjectiveInDisplaySlotSignature = std::string("(I)") + profile.scoreObjectiveSignature;
    const std::string getPlayersTeamSignature = std::string("(Ljava/lang/String;)") + profile.scorePlayerTeamSignature;
    const std::string getSortedScoresSignature = std::string("(") + profile.scoreObjectiveSignature + ")Ljava/util/Collection;";
    const std::string formatPlayerNameSignature = std::string("(") +
        profile.teamSignature + "Ljava/lang/String;)Ljava/lang/String;";
    const std::string getNetHandlerSignature = std::string("()") +
        profile.netHandlerSignature;
    const std::string getItemSignature = std::string("()") + profile.itemSignature;

    const bool singletonResolved = !profile.minecraftInstanceField.empty()
        ? lookupRequired(env, candidate.minecraftInstanceField, [&] {
              return env->GetStaticFieldID(minecraft,
                                           profile.minecraftInstanceField.c_str(),
                                           profile.minecraftSignature.c_str());
          })
        : lookupRequired(env, candidate.getMinecraft, [&] {
              return env->GetStaticMethodID(minecraft, profile.getMinecraft.c_str(),
                                            getMinecraftSignature.c_str());
          });
    if (!singletonResolved ||
        !lookupRequired(env, candidate.playerField, [&] {
            return env->GetFieldID(minecraft, profile.playerField.c_str(),
                                   profile.playerSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getHealth, [&] {
            return env->GetMethodID(living, profile.getHealth.c_str(), "()F");
        }) ||
        !lookupRequired(env, candidate.getMaxHealth, [&] {
            return env->GetMethodID(living, profile.getMaxHealth.c_str(), "()F");
        }) ||
        !lookupRequired(env, candidate.getEntityId, [&] {
            return env->GetMethodID(entity, profile.getEntityId.c_str(), "()I");
        }) ||
        !lookupRequired(env, candidate.getBounds, [&] {
            return env->GetMethodID(entity, profile.getBounds.c_str(),
                                    getBoundsSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.isMainThread, [&] {
            return env->GetMethodID(minecraft, profile.isMainThread.c_str(), "()Z");
        }) ||
        !lookupRequired(env, candidate.isSingleplayer, [&] {
            return env->GetMethodID(minecraft, profile.isSingleplayer.c_str(), "()Z");
        }) ||
        !lookupRequired(env, candidate.worldField, [&] {
            return env->GetFieldID(minecraft, profile.worldField.c_str(),
                                   profile.worldClientSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.timerField, [&] {
            return env->GetFieldID(minecraft, profile.timerField.c_str(),
                                   profile.timerSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.setIngameFocus, [&] {
            return env->GetMethodID(minecraft, profile.setIngameFocus.c_str(), "()V");
        }) ||
        !lookupRequired(env, candidate.setIngameNotInFocus, [&] {
            return env->GetMethodID(minecraft, profile.setIngameNotInFocus.c_str(), "()V");
        }) ||
        !lookupRequired(env, candidate.getRenderManager, [&] {
            return env->GetMethodID(minecraft, profile.getRenderManager.c_str(),
                                    getRenderManagerSignature.c_str());
        })) {
        return false;
    }

    for (std::size_t index = 0U; index < candidate.renderPosition.size(); ++index) {
        if (!lookupRequired(env, candidate.renderPosition[index], [&] {
                return env->GetFieldID(renderManager,
                                       profile.renderPositionFields[index].c_str(), "D");
            })) {
            return false;
        }
    }

    for (std::size_t index = 0U; index < candidate.previousPosition.size(); ++index) {
        if (!lookupRequired(env, candidate.previousPosition[index], [&] {
                return env->GetFieldID(entity,
                                       profile.previousPositionFields[index].c_str(), "D");
            })) {
            return false;
        }
    }

    std::array<jfieldID, 3U> positionFields{};
    for (std::size_t index = 0U; index < positionFields.size(); ++index) {
        if (!lookupRequired(env, positionFields[index], [&] {
                return env->GetFieldID(entity, profile.positionFields[index].c_str(), "D");
            })) {
            return false;
        }
    }

    const bool loadedEntitiesResolved = !profile.loadedEntitiesField.empty()
        ? lookupRequired(env, candidate.loadedEntitiesField, [&] {
              return env->GetFieldID(world, profile.loadedEntitiesField.c_str(),
                                     "Ljava/util/List;");
          })
        : lookupRequired(env, candidate.getLoadedEntities, [&] {
              return env->GetMethodID(world, profile.getLoadedEntities.c_str(),
                                      "()Ljava/util/List;");
          });
    if (!loadedEntitiesResolved ||
        !lookupRequired(env, candidate.playerEntities, [&] {
            return env->GetFieldID(world, profile.playerEntitiesField.c_str(),
                                   "Ljava/util/List;");
        }) ||
        !lookupRequired(env, candidate.getName, [&] {
            return env->GetMethodID(entity, profile.getName.c_str(),
                                    "()Ljava/lang/String;");
        }) ||
        !lookupRequired(env, candidate.getDisplayName, [&] {
            return env->GetMethodID(entity, profile.getDisplayName.c_str(),
                                    (std::string("()") + profile.chatComponentSignature).c_str());
        }) ||
        !lookupRequired(env, candidate.getFormattedText, [&] {
            return env->GetMethodID(chatComponent, profile.getFormattedText.c_str(),
                                    "()Ljava/lang/String;");
        })) {
        return false;
    }
    // Invisibility is auxiliary: an older external mapping dictionary may not
    // provide it, but that must not disable the otherwise safe core binding.
    if (!profile.isInvisible.empty()) {
        candidate.isInvisible = env->GetMethodID(
            entity, profile.isInvisible.c_str(), "()Z");
        if (env->ExceptionCheck() == JNI_TRUE || candidate.isInvisible == nullptr) {
            env->ExceptionClear();
            candidate.isInvisible = nullptr;
            log::info(std::string("Invisibility capability disabled for profile: ") +
                      profile.label + " (auxiliary mapping did not resolve).");
        }
    }
    if (debugChatClassLoaded && !profile.addChatMessage.empty()) {
        candidate.chatTextConstructor = env->GetMethodID(
            chatText, "<init>", "(Ljava/lang/String;)V");
        candidate.addChatMessage = env->GetMethodID(
            player, profile.addChatMessage.c_str(),
            (std::string("(") + profile.chatComponentSignature + ")V").c_str());
        if (env->ExceptionCheck() == JNI_TRUE ||
            candidate.chatTextConstructor == nullptr || candidate.addChatMessage == nullptr) {
            env->ExceptionClear();
            candidate.chatTextConstructor = nullptr;
            candidate.addChatMessage = nullptr;
            log::info(std::string("Debug chat capability disabled for profile: ") +
                      profile.label + " (auxiliary mapping did not resolve).");
        }
    }
    if (richChatClassLoaded && !profile.parseChatJson.empty()) {
        candidate.parseChatJson = env->GetStaticMethodID(
            chatSerializer, profile.parseChatJson.c_str(),
            (std::string("(Ljava/lang/String;)") +
             profile.chatComponentSignature).c_str());
        if (env->ExceptionCheck() == JNI_TRUE || candidate.parseChatJson == nullptr) {
            env->ExceptionClear();
            candidate.parseChatJson = nullptr;
            log::info(std::string("Rich local chat capability disabled for profile: ") +
                      profile.label + " (auxiliary mapping did not resolve).");
        }
    }
    jclass listClass = nullptr;
    if (!lookupRequired(env, listClass,
                        [&] { return env->FindClass("java/util/List"); })) {
        return false;
    }
    localReferences.add(listClass);
    jclass collectionClass = nullptr;
    if (!lookupRequired(env, collectionClass,
                        [&] { return env->FindClass("java/util/Collection"); })) {
        return false;
    }
    localReferences.add(collectionClass);
    if (!lookupRequired(env, candidate.listSize,
                        [&] { return env->GetMethodID(listClass, "size", "()I"); }) ||
        !lookupRequired(env, candidate.listGet,
                        [&] { return env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;"); }) ||
        !lookupRequired(env, candidate.listToArray,
                        [&] { return env->GetMethodID(listClass, "toArray", "()[Ljava/lang/Object;"); }) ||
        !lookupRequired(env, candidate.collectionToArray,
                        [&] { return env->GetMethodID(collectionClass, "toArray", "()[Ljava/lang/Object;"); }) ||
        !lookupRequired(env, candidate.blockPosConstructor, [&] {
            return env->GetMethodID(blockPos, "<init>", "(III)V");
        }) ||
        !lookupRequired(env, candidate.getBlockState, [&] {
            return env->GetMethodID(world, profile.getBlockState.c_str(),
                                    getBlockStateSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getBlock, [&] {
            return env->GetMethodID(state, profile.getBlock.c_str(),
                                    getBlockSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getBlockMetadata, [&] {
            return env->GetMethodID(block, profile.getBlockMetadata.c_str(),
                                    getBlockMetadataSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getChunkProvider, [&] {
            const std::string signature = std::string("()") +
                                          profile.chunkProviderInterfaceSignature;
            return env->GetMethodID(world, profile.getChunkProvider.c_str(),
                                    signature.c_str());
        }) ||
        !lookupRequired(env, candidate.chunkListingField, [&] {
            return env->GetFieldID(chunkProvider, profile.chunkListingField.c_str(),
                                   "Ljava/util/List;");
        }) ||
        !lookupRequired(env, candidate.chunkX, [&] {
            return env->GetFieldID(chunk, profile.chunkCoordinateFields[0].c_str(), "I");
        }) ||
        !lookupRequired(env, candidate.chunkZ, [&] {
            return env->GetFieldID(chunk, profile.chunkCoordinateFields[1].c_str(), "I");
        }) ||
        !lookupRequired(env, candidate.getStorageArrays, [&] {
            return env->GetMethodID(chunk, profile.getStorageArrays.c_str(),
                                    getStorageArraysSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getStorageData, [&] {
            return env->GetMethodID(storage, profile.getStorageData.c_str(), "()[C");
        }) ||
        !lookupRequired(env, candidate.activeModelView, [&] {
            return env->GetStaticFieldID(activeRenderInfo,
                                         profile.activeModelViewField.c_str(),
                                         "Ljava/nio/FloatBuffer;");
        }) ||
        !lookupRequired(env, candidate.activeProjection, [&] {
            return env->GetStaticFieldID(activeRenderInfo,
                                         profile.activeProjectionField.c_str(),
                                         "Ljava/nio/FloatBuffer;");
        }) ||
        !lookupRequired(env, candidate.activeViewport, [&] {
            return env->GetStaticFieldID(activeRenderInfo,
                                         profile.activeViewportField.c_str(),
                                         "Ljava/nio/IntBuffer;");
        }) ||
        !lookupRequired(env, candidate.renderPartialTicks, [&] {
            return env->GetFieldID(timer, profile.renderPartialTicksField.c_str(), "F");
        })) {
        return false;
    }

    bool sidebarCapability = sidebarClassesLoaded &&
        !profile.teamSignature.empty() &&
        !profile.getScoreboard.empty() &&
        !profile.getObjectiveInDisplaySlot.empty() &&
        !profile.getPlayersTeam.empty() &&
        !profile.getSortedScores.empty() &&
        !profile.getPlayerName.empty() &&
        !profile.formatPlayerName.empty();
    if (sidebarCapability) {
        sidebarCapability =
            lookupRequired(env, candidate.getScoreboard, [&] {
                return env->GetMethodID(world, profile.getScoreboard.c_str(),
                                        (std::string("()") + profile.scoreboardSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getObjectiveInDisplaySlot, [&] {
                return env->GetMethodID(scoreboard, profile.getObjectiveInDisplaySlot.c_str(),
                                        getObjectiveInDisplaySlotSignature.c_str());
            }) &&
            lookupRequired(env, candidate.getPlayersTeam, [&] {
                return env->GetMethodID(scoreboard, profile.getPlayersTeam.c_str(),
                                        getPlayersTeamSignature.c_str());
            }) &&
            lookupRequired(env, candidate.getSortedScores, [&] {
                return env->GetMethodID(scoreboard, profile.getSortedScores.c_str(),
                                        getSortedScoresSignature.c_str());
            }) &&
            lookupRequired(env, candidate.getPlayerName, [&] {
                return env->GetMethodID(score, profile.getPlayerName.c_str(),
                                        "()Ljava/lang/String;");
            }) &&
            lookupRequired(env, candidate.formatPlayerName, [&] {
                return env->GetStaticMethodID(scorePlayerTeam,
                                              profile.formatPlayerName.c_str(),
                                              formatPlayerNameSignature.c_str());
            });
    }
    if (!sidebarCapability) {
        candidate.getScoreboard = nullptr;
        candidate.getObjectiveInDisplaySlot = nullptr;
        candidate.getPlayersTeam = nullptr;
        candidate.getSortedScores = nullptr;
        candidate.getPlayerName = nullptr;
        candidate.formatPlayerName = nullptr;
        log::info(std::string("Sidebar capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    bool armorCapability = armorClassesLoaded &&
        !profile.getItem.empty() &&
        !profile.hasColor.empty() &&
        !profile.getColor.empty() && !profile.getEnchantmentLevel.empty() &&
        !profile.getEquipmentInSlot.empty() && !profile.getIdFromItem.empty() &&
        !profile.stackSizeField.empty() && !profile.getItemDamage.empty();
    if (armorCapability) {
        armorCapability =
            lookupRequired(env, candidate.getItem, [&] {
                return env->GetMethodID(itemStack, profile.getItem.c_str(),
                                        getItemSignature.c_str());
            }) &&
            lookupRequired(env, candidate.hasColor, [&] {
                return env->GetMethodID(itemArmor, profile.hasColor.c_str(),
                                        (std::string("(") + profile.itemStackSignature + ")Z").c_str());
            }) &&
            lookupRequired(env, candidate.getColor, [&] {
                return env->GetMethodID(itemArmor, profile.getColor.c_str(),
                                        (std::string("(") + profile.itemStackSignature + ")I").c_str());
            }) &&
            lookupRequired(env, candidate.getEnchantmentLevel, [&] {
                return env->GetStaticMethodID(
                    enchantmentHelper, profile.getEnchantmentLevel.c_str(),
                    (std::string("(I") + profile.itemStackSignature + ")I").c_str());
            }) &&
            lookupRequired(env, candidate.getEquipmentInSlot, [&] {
                return env->GetMethodID(
                    living, profile.getEquipmentInSlot.c_str(),
                    (std::string("(I)") + profile.itemStackSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getIdFromItem, [&] {
                return env->GetStaticMethodID(
                    item, profile.getIdFromItem.c_str(),
                    (std::string("(") + profile.itemSignature + ")I").c_str());
            }) &&
            lookupRequired(env, candidate.stackSize, [&] {
                return env->GetFieldID(itemStack, profile.stackSizeField.c_str(), "I");
            }) &&
            lookupRequired(env, candidate.getItemDamage, [&] {
                return env->GetMethodID(itemStack, profile.getItemDamage.c_str(), "()I");
            });
    }
    if (!armorCapability) {
        candidate.inventoryField = nullptr;
        candidate.armorInventoryField = nullptr;
        candidate.getItem = nullptr;
        candidate.hasColor = nullptr;
        candidate.getColor = nullptr;
        candidate.getEnchantmentLevel = nullptr;
        candidate.getEquipmentInSlot = nullptr;
        candidate.getIdFromItem = nullptr;
        candidate.stackSize = nullptr;
        candidate.getItemDamage = nullptr;
        log::info(std::string("Armor capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    bool identityCapability = uuid != nullptr && !profile.getUniqueId.empty();
    if (identityCapability) {
        identityCapability =
            lookupRequired(env, candidate.getUniqueId, [&] {
                return env->GetMethodID(entity, profile.getUniqueId.c_str(),
                                        "()Ljava/util/UUID;");
            }) &&
            lookupRequired(env, candidate.uuidToString, [&] {
                return env->GetMethodID(uuid, "toString", "()Ljava/lang/String;");
            });
    }
    if (!identityCapability) {
        candidate.getUniqueId = nullptr;
        candidate.uuidToString = nullptr;
        log::info(std::string("UUID identity capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    bool skinCapability = skinClassesLoaded &&
        !profile.getLocationSkin.empty() && !profile.getTextureManager.empty() &&
        !profile.getTexture.empty() && !profile.getGlTextureId.empty();
    if (skinCapability) {
        skinCapability =
            lookupRequired(env, candidate.getLocationSkin, [&] {
                return env->GetMethodID(
                    abstractClientPlayer, profile.getLocationSkin.c_str(),
                    (std::string("()") + profile.resourceLocationSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getTextureManager, [&] {
                return env->GetMethodID(
                    minecraft, profile.getTextureManager.c_str(),
                    (std::string("()") + profile.textureManagerSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getTexture, [&] {
                return env->GetMethodID(
                    textureManager, profile.getTexture.c_str(),
                    (std::string("(") + profile.resourceLocationSignature + ")" +
                     profile.textureObjectSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getGlTextureId, [&] {
                return env->GetMethodID(textureObject, profile.getGlTextureId.c_str(), "()I");
            });
    }
    if (!skinCapability) {
        candidate.getLocationSkin = nullptr;
        candidate.getTextureManager = nullptr;
        candidate.getTexture = nullptr;
        candidate.getGlTextureId = nullptr;
        log::info(std::string("Player skin capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    // Resolve the minimal Aim Assist surface independently. Previously these
    // four fields lived inside the all-or-nothing movement/scaffold chain, so a
    // missing ItemBlock/PlayerController mapping silently disabled aim on an
    // otherwise supported transformed client.
    bool aimCapability = gameSettingsClassLoaded &&
        !profile.gameSettingsField.empty() &&
        !profile.mouseSensitivityField.empty() &&
        !profile.rotationYawField.empty() &&
        !profile.rotationPitchField.empty();
    if (aimCapability) {
        aimCapability =
            lookupRequired(env, candidate.gameSettingsField, [&] {
                return env->GetFieldID(minecraft,
                    profile.gameSettingsField.c_str(),
                    profile.gameSettingsSignature.c_str());
            }) &&
            lookupRequired(env, candidate.mouseSensitivity, [&] {
                return env->GetFieldID(gameSettings,
                    profile.mouseSensitivityField.c_str(), "F");
            }) &&
            lookupRequired(env, candidate.rotationYaw, [&] {
                return env->GetFieldID(entity,
                    profile.rotationYawField.c_str(), "F");
            }) &&
            lookupRequired(env, candidate.rotationPitch, [&] {
                return env->GetFieldID(entity,
                    profile.rotationPitchField.c_str(), "F");
            });
    }
    if (!aimCapability) {
        candidate.gameSettingsField = nullptr;
        candidate.mouseSensitivity = nullptr;
        candidate.rotationYaw = nullptr;
        candidate.rotationPitch = nullptr;
        log::info(std::string("Aim capability disabled for profile: ") +
                  profile.label + " (minimal mapping did not resolve).");
    }

    bool safewalkCapability = safewalkClassesLoaded && aimCapability &&
        !profile.keyBindSneakField.empty() && !profile.getKeyCode.empty() &&
        !profile.setKeyBindState.empty() && !profile.isAirBlock.empty();
    if (safewalkCapability) {
        safewalkCapability =
            lookupRequired(env, candidate.keyBindSneakField, [&] {
                return env->GetFieldID(gameSettings,
                    profile.keyBindSneakField.c_str(),
                    profile.keyBindingSignature.c_str());
            }) &&
            lookupRequired(env, candidate.getKeyCode, [&] {
                return env->GetMethodID(keyBinding,
                    profile.getKeyCode.c_str(), "()I");
            }) &&
            lookupRequired(env, candidate.setKeyBindState, [&] {
                return env->GetStaticMethodID(keyBinding,
                    profile.setKeyBindState.c_str(), "(IZ)V");
            }) &&
            lookupRequired(env, candidate.isAirBlock, [&] {
                return env->GetMethodID(world, profile.isAirBlock.c_str(),
                    (std::string("(") + profile.blockPosSignature + ")Z").c_str());
            });
    }
    if (!safewalkCapability) {
        candidate.keyBindSneakField = nullptr;
        candidate.getKeyCode = nullptr;
        candidate.setKeyBindState = nullptr;
        candidate.isAirBlock = nullptr;
        log::info(std::string("Safewalk capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    bool serverGuardCapability = serverDataClassLoaded &&
        !profile.getCurrentServerData.empty() && !profile.serverIpField.empty();
    if (serverGuardCapability) {
        serverGuardCapability =
            lookupRequired(env, candidate.getCurrentServerData, [&] {
                return env->GetMethodID(minecraft,
                    profile.getCurrentServerData.c_str(),
                    (std::string("()") + profile.serverDataSignature).c_str());
            }) &&
            lookupRequired(env, candidate.serverIp, [&] {
                return env->GetFieldID(serverData, profile.serverIpField.c_str(),
                                       "Ljava/lang/String;");
            });
    }
    if (!serverGuardCapability) {
        candidate.getCurrentServerData = nullptr;
        candidate.serverIp = nullptr;
        log::info(std::string("Server-address guard disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    bool movementCapability = movementClassesLoaded && safewalkCapability;
    if (movementCapability) {
        for (std::size_t index = 0U;
             index < candidate.movementKeyFields.size(); ++index) {
            movementCapability = movementCapability && lookupRequired(
                env, candidate.movementKeyFields[index], [&] {
                    return env->GetFieldID(gameSettings,
                        profile.movementKeyFields[index].c_str(),
                        profile.keyBindingSignature.c_str());
                });
        }
        for (std::size_t index = 0U; index < candidate.motionFields.size(); ++index) {
            movementCapability = movementCapability && lookupRequired(
                env, candidate.motionFields[index], [&] {
                    return env->GetFieldID(entity,
                        profile.motionFields[index].c_str(), "D");
                });
        }
        const std::string rightClickSignature = std::string("(") +
            profile.playerSignature + profile.worldClientSignature +
            profile.itemStackSignature + profile.blockPosSignature +
            profile.enumFacingSignature + profile.vec3Signature + ")Z";
        movementCapability = movementCapability &&
            lookupRequired(env, candidate.onGround, [&] {
                return env->GetFieldID(entity,
                    profile.onGroundField.c_str(), "Z");
            }) &&
            lookupRequired(env, candidate.jump, [&] {
                return env->GetMethodID(living, profile.jump.c_str(), "()V");
            }) &&
            lookupRequired(env, candidate.playerControllerField, [&] {
                return env->GetFieldID(minecraft,
                    profile.playerControllerField.c_str(),
                    profile.playerControllerSignature.c_str());
            }) &&
            lookupRequired(env, candidate.inventoryField, [&] {
                return env->GetFieldID(player, profile.inventoryField.c_str(),
                                       profile.inventoryPlayerSignature.c_str());
            }) &&
            lookupRequired(env, candidate.currentItem, [&] {
                return env->GetFieldID(inventoryPlayer,
                    profile.currentItemField.c_str(), "I");
            }) &&
            lookupRequired(env, candidate.mainInventory, [&] {
                return env->GetFieldID(inventoryPlayer,
                    profile.mainInventoryField.c_str(),
                    (std::string("[") + profile.itemStackSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getItem, [&] {
                return env->GetMethodID(itemStack, profile.getItem.c_str(),
                    (std::string("()") + profile.itemSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getBlockFromItem, [&] {
                return env->GetMethodID(itemBlock,
                    profile.getBlockFromItem.c_str(),
                    (std::string("()") + profile.blockSignature).c_str());
            }) &&
            lookupRequired(env, candidate.getIdFromBlock, [&] {
                return env->GetStaticMethodID(block,
                    profile.getIdFromBlock.c_str(),
                    (std::string("(") + profile.blockSignature + ")I").c_str());
            }) &&
            lookupRequired(env, candidate.getFacingByIndex, [&] {
                return env->GetStaticMethodID(enumFacing,
                    profile.getFacingByIndex.c_str(),
                    (std::string("(I)") + profile.enumFacingSignature).c_str());
            }) &&
            lookupRequired(env, candidate.vec3Constructor, [&] {
                return env->GetMethodID(vec3, "<init>", "(DDD)V");
            }) &&
            lookupRequired(env, candidate.onPlayerRightClick, [&] {
                return env->GetMethodID(playerController,
                    profile.onPlayerRightClick.c_str(),
                    rightClickSignature.c_str());
            });
    }
    if (!movementCapability) {
        candidate.movementKeyFields.fill(nullptr);
        candidate.motionFields.fill(nullptr);
        candidate.onGround = nullptr;
        candidate.jump = nullptr;
        candidate.playerControllerField = nullptr;
        candidate.inventoryField = nullptr;
        candidate.currentItem = nullptr;
        candidate.mainInventory = nullptr;
        candidate.getBlockFromItem = nullptr;
        candidate.getIdFromBlock = nullptr;
        candidate.getFacingByIndex = nullptr;
        candidate.vec3Constructor = nullptr;
        candidate.onPlayerRightClick = nullptr;
        log::info(std::string("Movement capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    std::array<jfieldID, 6U> boundsFields{};
    for (std::size_t index = 0U; index < boundsFields.size(); ++index) {
        if (!lookupRequired(env, boundsFields[index], [&] {
                return env->GetFieldID(aabb, profile.aabbFields[index].c_str(), "D");
            })) {
            return false;
        }
    }

    candidate.positionX = positionFields[0U];
    candidate.positionY = positionFields[1U];
    candidate.positionZ = positionFields[2U];
    candidate.minX = boundsFields[0U];
    candidate.minY = boundsFields[1U];
    candidate.minZ = boundsFields[2U];
    candidate.maxX = boundsFields[3U];
    candidate.maxY = boundsFields[4U];
    candidate.maxZ = boundsFields[5U];

    auto makeGlobal = [&](jclass const local, jclass& global) noexcept {
        global = static_cast<jclass>(env->NewGlobalRef(local));
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            if (global != nullptr) {
                env->DeleteGlobalRef(global);
                global = nullptr;
            }
            return false;
        }
        return global != nullptr;
    };
    if (!makeGlobal(minecraft, candidate.minecraftClass) ||
        !makeGlobal(player, candidate.playerClass) ||
        !makeGlobal(living, candidate.livingClass) ||
        !makeGlobal(entity, candidate.entityClass) ||
        !makeGlobal(aabb, candidate.aabbClass) ||
        !makeGlobal(world, candidate.worldClass) ||
        !makeGlobal(worldClient, candidate.worldClientClass) ||
        !makeGlobal(state, candidate.stateClass) ||
        !makeGlobal(block, candidate.blockClass) ||
        !makeGlobal(blockPos, candidate.blockPosClass) ||
        !makeGlobal(bed, candidate.bedClass) ||
        !makeGlobal(chunkProvider, candidate.chunkProviderClass) ||
        !makeGlobal(chunk, candidate.chunkClass) ||
        !makeGlobal(storage, candidate.storageClass) ||
        !makeGlobal(activeRenderInfo, candidate.activeRenderInfoClass) ||
        !makeGlobal(renderManager, candidate.renderManagerClass) ||
        !makeGlobal(timer, candidate.timerClass) ||
        !makeGlobal(chatComponent, candidate.chatComponentClass)) {
        return false;
    }
    if (fireballClassLoaded && !makeGlobal(fireball, candidate.fireballClass))
        candidate.fireballClass = nullptr;

    bool tabCapability = tabClassesLoaded &&
        !profile.getNetHandler.empty() && !profile.getPlayerInfoMap.empty() &&
        !profile.getGameProfile.empty();
    if (tabCapability) {
        tabCapability =
            lookupRequired(env, candidate.getNetHandler, [&] {
                return env->GetMethodID(minecraft, profile.getNetHandler.c_str(),
                                        getNetHandlerSignature.c_str());
            }) &&
            lookupRequired(env, candidate.getPlayerInfoMap, [&] {
                return env->GetMethodID(netHandler, profile.getPlayerInfoMap.c_str(),
                                        "()Ljava/util/Collection;");
            }) &&
            lookupRequired(env, candidate.getGameProfile, [&] {
                return env->GetMethodID(networkPlayerInfo,
                                        profile.getGameProfile.c_str(),
                                        "()Lcom/mojang/authlib/GameProfile;");
            }) &&
            lookupRequired(env, candidate.gameProfileGetName, [&] {
                return env->GetMethodID(gameProfile, "getName",
                                        "()Ljava/lang/String;");
            });
        if (tabCapability && candidate.uuidToString != nullptr) {
            candidate.gameProfileGetId = env->GetMethodID(
                gameProfile, "getId", "()Ljava/util/UUID;");
            if (env->ExceptionCheck() == JNI_TRUE ||
                candidate.gameProfileGetId == nullptr) {
                clearException(env);
                candidate.gameProfileGetId = nullptr;
                log::info(std::string("TAB UUID bridge disabled for profile: ") +
                          profile.label + " (GameProfile.getId did not resolve).");
            }
        }
    }

    if (!tabCapability) {
        candidate.getNetHandler = nullptr;
        candidate.getPlayerInfoMap = nullptr;
        candidate.getGameProfile = nullptr;
        candidate.gameProfileGetName = nullptr;
        candidate.gameProfileGetId = nullptr;
        log::info(std::string("TAB roster capability disabled for profile: ") +
                  profile.label + " (auxiliary mapping did not resolve).");
    }

    auto clearGlobal = [&](jclass& reference) noexcept {
        if (reference != nullptr) {
            env->DeleteGlobalRef(reference);
            reference = nullptr;
        }
    };

    if (aimCapability &&
        !makeGlobal(gameSettings, candidate.gameSettingsClass)) {
        aimCapability = false;
        safewalkCapability = false;
        movementCapability = false;
        clearGlobal(candidate.gameSettingsClass);
        candidate.gameSettingsField = nullptr;
        candidate.mouseSensitivity = nullptr;
        candidate.rotationYaw = nullptr;
        candidate.rotationPitch = nullptr;
        log::info(std::string("Aim capability disabled for profile: ") +
                  profile.label + " (failed to publish GameSettings class).");
    }

    if (safewalkCapability) {
        safewalkCapability = makeGlobal(keyBinding, candidate.keyBindingClass);
        if (!safewalkCapability) {
            movementCapability = false;
            clearGlobal(candidate.keyBindingClass);
            candidate.keyBindSneakField = nullptr;
            candidate.getKeyCode = nullptr;
            candidate.setKeyBindState = nullptr;
            candidate.isAirBlock = nullptr;
            log::info(std::string("Safewalk capability disabled for profile: ") +
                      profile.label + " (failed to publish class references).");
        }
    }

    if (serverGuardCapability &&
        !makeGlobal(serverData, candidate.serverDataClass)) {
        serverGuardCapability = false;
        candidate.getCurrentServerData = nullptr;
        candidate.serverIp = nullptr;
    }

    bool itemClassRefsPublished = itemClassesLoaded &&
        makeGlobal(itemStack, candidate.itemStackClass) &&
        makeGlobal(item, candidate.itemClass) &&
        makeGlobal(inventoryPlayer, candidate.inventoryPlayerClass);
    if (!itemClassRefsPublished) {
        clearGlobal(candidate.itemStackClass);
        clearGlobal(candidate.itemClass);
        clearGlobal(candidate.inventoryPlayerClass);
        movementCapability = false;
        armorCapability = false;
    }

    if (movementCapability) {
        movementCapability =
            makeGlobal(playerController, candidate.playerControllerClass) &&
            makeGlobal(itemBlock, candidate.itemBlockClass) &&
            makeGlobal(enumFacing, candidate.enumFacingClass) &&
            makeGlobal(vec3, candidate.vec3Class);
        if (!movementCapability) {
            clearGlobal(candidate.playerControllerClass);
            clearGlobal(candidate.itemBlockClass);
            clearGlobal(candidate.enumFacingClass);
            clearGlobal(candidate.vec3Class);
            candidate.movementKeyFields.fill(nullptr);
            candidate.motionFields.fill(nullptr);
            candidate.onGround = nullptr;
            candidate.jump = nullptr;
            candidate.playerControllerField = nullptr;
            candidate.inventoryField = nullptr;
            candidate.currentItem = nullptr;
            candidate.mainInventory = nullptr;
            candidate.getBlockFromItem = nullptr;
            candidate.getIdFromBlock = nullptr;
            candidate.getFacingByIndex = nullptr;
            candidate.vec3Constructor = nullptr;
            candidate.onPlayerRightClick = nullptr;
        }
    }

    if (candidate.chatTextConstructor != nullptr && candidate.addChatMessage != nullptr &&
        !makeGlobal(chatText, candidate.chatTextClass)) {
        candidate.chatTextConstructor = nullptr;
        candidate.addChatMessage = nullptr;
    }
    if (candidate.parseChatJson != nullptr &&
        !makeGlobal(chatSerializer, candidate.chatSerializerClass)) {
        candidate.parseChatJson = nullptr;
    }

    if (identityCapability && !makeGlobal(uuid, candidate.uuidClass)) {
        identityCapability = false;
        candidate.getUniqueId = nullptr;
        candidate.uuidToString = nullptr;
        candidate.gameProfileGetId = nullptr;
    }

    if (skinCapability) {
        skinCapability =
            makeGlobal(abstractClientPlayer, candidate.abstractClientPlayerClass) &&
            makeGlobal(resourceLocation, candidate.resourceLocationClass) &&
            makeGlobal(textureManager, candidate.textureManagerClass) &&
            makeGlobal(textureObject, candidate.textureObjectClass);
        if (!skinCapability) {
            clearGlobal(candidate.abstractClientPlayerClass);
            clearGlobal(candidate.resourceLocationClass);
            clearGlobal(candidate.textureManagerClass);
            clearGlobal(candidate.textureObjectClass);
            candidate.getLocationSkin = nullptr;
            candidate.getTextureManager = nullptr;
            candidate.getTexture = nullptr;
            candidate.getGlTextureId = nullptr;
        }
    }

    if (sidebarCapability) {
        sidebarCapability =
            makeGlobal(scoreboard, candidate.scoreboardClass) &&
            makeGlobal(scoreObjective, candidate.scoreObjectiveClass) &&
            makeGlobal(score, candidate.scoreClass) &&
            makeGlobal(scorePlayerTeam, candidate.scorePlayerTeamClass);
        if (!sidebarCapability) {
            clearGlobal(candidate.scoreboardClass);
            clearGlobal(candidate.scoreObjectiveClass);
            clearGlobal(candidate.scoreClass);
            clearGlobal(candidate.scorePlayerTeamClass);
            candidate.getScoreboard = nullptr;
            candidate.getObjectiveInDisplaySlot = nullptr;
            candidate.getPlayersTeam = nullptr;
            candidate.getSortedScores = nullptr;
            candidate.getPlayerName = nullptr;
            candidate.formatPlayerName = nullptr;
            log::info(std::string("Sidebar capability disabled for profile: ") +
                      profile.label + " (failed to publish class references).");
        }
    }

    if (tabCapability) {
        tabCapability =
            makeGlobal(netHandler, candidate.netHandlerClass) &&
            makeGlobal(networkPlayerInfo, candidate.networkPlayerInfoClass) &&
            makeGlobal(gameProfile, candidate.gameProfileClass);
        if (!tabCapability) {
            clearGlobal(candidate.netHandlerClass);
            clearGlobal(candidate.networkPlayerInfoClass);
            clearGlobal(candidate.gameProfileClass);
            candidate.getNetHandler = nullptr;
            candidate.getPlayerInfoMap = nullptr;
            candidate.getGameProfile = nullptr;
            candidate.gameProfileGetName = nullptr;
            candidate.gameProfileGetId = nullptr;
            log::info(std::string("TAB roster capability disabled for profile: ") +
                      profile.label + " (failed to publish class references).");
        }
    }

    if (armorCapability) {
        armorCapability = itemClassRefsPublished &&
            makeGlobal(itemArmor, candidate.itemArmorClass) &&
            makeGlobal(enchantmentHelper, candidate.enchantmentHelperClass);
        if (!armorCapability) {
            clearGlobal(candidate.itemArmorClass);
            clearGlobal(candidate.enchantmentHelperClass);
            candidate.armorInventoryField = nullptr;
            if (!movementCapability) {
                candidate.inventoryField = nullptr;
                candidate.getItem = nullptr;
            }
            candidate.hasColor = nullptr;
            candidate.getColor = nullptr;
            candidate.getEnchantmentLevel = nullptr;
            candidate.getEquipmentInSlot = nullptr;
            candidate.getIdFromItem = nullptr;
            candidate.stackSize = nullptr;
            candidate.getItemDamage = nullptr;
            log::info(std::string("Armor capability disabled for profile: ") +
                      profile.label + " (failed to publish class references).");
        }
    }

    jobject minecraftObject = candidate.minecraftInstanceField != nullptr
        ? env->GetStaticObjectField(minecraft, candidate.minecraftInstanceField)
        : env->CallStaticObjectMethod(minecraft, candidate.getMinecraft);
    if (env->ExceptionCheck() == JNI_TRUE || minecraftObject == nullptr) {
        clearException(env);
        return false;
    }
    localReferences.add(minecraftObject);
    jobject renderManagerObject = env->CallObjectMethod(minecraftObject,
                                                         candidate.getRenderManager);
    jobject timerObject = env->GetObjectField(minecraftObject, candidate.timerField);
    jobject modelViewBuffer = env->GetStaticObjectField(activeRenderInfo,
                                                         candidate.activeModelView);
    jobject projectionBuffer = env->GetStaticObjectField(activeRenderInfo,
                                                          candidate.activeProjection);
    jobject viewportBuffer = env->GetStaticObjectField(activeRenderInfo,
                                                        candidate.activeViewport);
    if (env->ExceptionCheck() == JNI_TRUE || renderManagerObject == nullptr ||
        timerObject == nullptr ||
        modelViewBuffer == nullptr || projectionBuffer == nullptr || viewportBuffer == nullptr) {
        clearException(env);
        return false;
    }
    localReferences.add(renderManagerObject);
    localReferences.add(timerObject);
    localReferences.add(modelViewBuffer);
    localReferences.add(projectionBuffer);
    localReferences.add(viewportBuffer);
    if (env->GetDirectBufferCapacity(modelViewBuffer) < 16 ||
        env->GetDirectBufferCapacity(projectionBuffer) < 16 ||
        env->GetDirectBufferCapacity(viewportBuffer) < 4 ||
        env->GetDirectBufferAddress(modelViewBuffer) == nullptr ||
        env->GetDirectBufferAddress(projectionBuffer) == nullptr ||
        env->GetDirectBufferAddress(viewportBuffer) == nullptr) {
        clearException(env);
        return false;
    }
    const auto makeGlobalObject = [&](jobject const local, jobject& global) noexcept {
        global = env->NewGlobalRef(local);
        if (env->ExceptionCheck() == JNI_TRUE || global == nullptr) {
            clearException(env);
            if (global != nullptr) env->DeleteGlobalRef(global);
            global = nullptr;
            return false;
        }
        return true;
    };
    if (!makeGlobalObject(renderManagerObject, candidate.renderManagerObject) ||
        !makeGlobalObject(timerObject, candidate.timerObject) ||
        !makeGlobalObject(modelViewBuffer, candidate.modelViewBuffer) ||
        !makeGlobalObject(projectionBuffer, candidate.projectionBuffer) ||
        !makeGlobalObject(viewportBuffer, candidate.viewportBuffer)) {
        return false;
    }

    candidate.profile = &profile;
    return true;
}

bool GameBindings::resolve(JNIEnv* const env) noexcept
{
    if (env == nullptr || !m_mappingRegistry.freeze()) {
        return false;
    }

    bindings::ClientEnvironment environment;
    probeEnvironmentHints(env, environment);
    bindings::MappingCandidates profiles;
    jclass const minecraft = findMinecraftClass(env, profiles, environment);
    if (minecraft == nullptr || profiles.empty()) {
        return false;
    }

    if (profiles.truncated) {
        log::info("Mapping candidate capacity reached; trying the highest-priority profiles only.");
    }
    for (std::size_t index = 0U; index < profiles.count; ++index) {
        const MappingProfile* const profile = profiles.items[index];
        if (profile == nullptr) continue;

        std::unique_ptr<BindingCache> candidate(new (std::nothrow) BindingCache());
        if (candidate == nullptr) {
            env->DeleteLocalRef(minecraft);
            return false;
        }
        bool resolved = false;
        try {
            resolved = resolveProfile(env, *profile, minecraft, *candidate);
        } catch (...) {
            resolved = false;
        }
        if (!resolved) {
            deleteGlobalRefs(env, *candidate);
            clearException(env);
            continue;
        }

        // Publication point. No render-side code can read m_cache until this
        // release store; after it, the cache and owned profile are immutable
        // through callback drain.
        m_cache = std::move(candidate);
        env->DeleteLocalRef(minecraft);
        m_retryAtMilliseconds.store(0U, std::memory_order_relaxed);
        m_resolutionPhase.store(ResolutionPhase::Resolved, std::memory_order_release);
        try {
            log::info(std::string("Minecraft 1.8.9 bindings resolved: ") + profile->label);
        } catch (...) {
            log::info("Minecraft 1.8.9 bindings resolved.");
        }
        return true;
    }

    env->DeleteLocalRef(minecraft);
    return false;
}

bool GameBindings::ensureLwjglMouseBindings(JNIEnv* const env) noexcept
{
    if (env == nullptr) return false;
    if (m_lwjglMouseClass == nullptr || m_lwjglSetGrabbed == nullptr ||
        m_lwjglIsGrabbed == nullptr) {
        jclass localMouse = env->FindClass("org/lwjgl/input/Mouse");
        if (env->ExceptionCheck() == JNI_TRUE || localMouse == nullptr) {
            clearException(env);
            return false;
        }
        jmethodID setGrabbed = env->GetStaticMethodID(localMouse, "setGrabbed", "(Z)V");
        jmethodID isGrabbed = env->GetStaticMethodID(localMouse, "isGrabbed", "()Z");
        if (env->ExceptionCheck() == JNI_TRUE || setGrabbed == nullptr ||
            isGrabbed == nullptr) {
            clearException(env);
            env->DeleteLocalRef(localMouse);
            return false;
        }
        jclass globalMouse = static_cast<jclass>(env->NewGlobalRef(localMouse));
        env->DeleteLocalRef(localMouse);
        if (env->ExceptionCheck() == JNI_TRUE || globalMouse == nullptr) {
            clearException(env);
            return false;
        }
        m_lwjglMouseClass = globalMouse;
        m_lwjglSetGrabbed = setGrabbed;
        m_lwjglIsGrabbed = isGrabbed;
    }
    return true;
}

bool GameBindings::ensureLwjglKeyboardBindings(JNIEnv* const env) noexcept
{
    if (env == nullptr) return false;
    if (m_lwjglKeyboardClass == nullptr || m_lwjglIsKeyDown == nullptr) {
        jclass localKeyboard = env->FindClass("org/lwjgl/input/Keyboard");
        if (env->ExceptionCheck() == JNI_TRUE || localKeyboard == nullptr) {
            clearException(env);
            return false;
        }
        jmethodID isKeyDown = env->GetStaticMethodID(
            localKeyboard, "isKeyDown", "(I)Z");
        if (env->ExceptionCheck() == JNI_TRUE || isKeyDown == nullptr) {
            clearException(env);
            env->DeleteLocalRef(localKeyboard);
            return false;
        }
        jclass globalKeyboard = static_cast<jclass>(
            env->NewGlobalRef(localKeyboard));
        env->DeleteLocalRef(localKeyboard);
        if (env->ExceptionCheck() == JNI_TRUE || globalKeyboard == nullptr) {
            clearException(env);
            return false;
        }
        m_lwjglKeyboardClass = globalKeyboard;
        m_lwjglIsKeyDown = isKeyDown;
    }
    return true;
}

bool GameBindings::queryLwjglKeyDown(JNIEnv* const env, const int lwjglKey,
                                     bool& down) noexcept
{
    if (lwjglKey <= 0 || !ensureLwjglKeyboardBindings(env)) return false;
    const jboolean value = env->CallStaticBooleanMethod(
        m_lwjglKeyboardClass, m_lwjglIsKeyDown, static_cast<jint>(lwjglKey));
    const bool succeeded = env->ExceptionCheck() != JNI_TRUE;
    clearException(env);
    if (succeeded) down = value == JNI_TRUE;
    return succeeded;
}

bool GameBindings::queryLwjglMouseGrabbed(JNIEnv* const env, bool& grabbed) noexcept
{
    if (!ensureLwjglMouseBindings(env)) return false;
    const jboolean value = env->CallStaticBooleanMethod(m_lwjglMouseClass,
                                                        m_lwjglIsGrabbed);
    const bool succeeded = env->ExceptionCheck() != JNI_TRUE;
    clearException(env);
    if (succeeded) grabbed = value == JNI_TRUE;
    return succeeded;
}

bool GameBindings::setLwjglMouseGrabbed(JNIEnv* const env, const bool grabbed) noexcept
{
    if (!ensureLwjglMouseBindings(env)) return false;
    env->CallStaticVoidMethod(m_lwjglMouseClass, m_lwjglSetGrabbed,
                              grabbed ? JNI_TRUE : JNI_FALSE);
    const bool succeeded = env->ExceptionCheck() != JNI_TRUE;
    clearException(env);
    return succeeded;
}

bool GameBindings::setInputCaptured(JNIEnv* const env, const bool guiOpen) noexcept
{
    if (env == nullptr) return false;

    if (guiOpen && !m_overlayInputSessionActive) {
        bool wasGrabbed = true;
        m_inputGrabStateKnown = queryLwjglMouseGrabbed(env, wasGrabbed);
        m_inputWasGrabbed = wasGrabbed;
        m_overlayInputSessionActive = true;
    }
    const bool restoreGrabbed = m_inputGrabStateKnown ? m_inputWasGrabbed : true;
    // On a title/menu screen Mouse.isGrabbed() is false. Calling
    // Minecraft.setIngameFocus() while closing our GUI would incorrectly grab
    // and hide that already-free cursor, so only invoke Minecraft's focus pair
    // when the overlay actually interrupted a grabbed gameplay session.
    const bool changeMinecraftFocus = restoreGrabbed;

    bool minecraftFocusChanged = false;
    if (changeMinecraftFocus &&
        m_resolutionPhase.load(std::memory_order_acquire) == ResolutionPhase::Resolved &&
        m_cache != nullptr) {
        BindingCache* const cache = m_cache.get();
        jobject minecraft = cache->minecraftInstanceField != nullptr
            ? env->GetStaticObjectField(cache->minecraftClass, cache->minecraftInstanceField)
            : env->CallStaticObjectMethod(cache->minecraftClass, cache->getMinecraft);
        if (env->ExceptionCheck() != JNI_TRUE && minecraft != nullptr) {
            // func_71364_i updates Minecraft.inGameHasFocus as well as LWJGL;
            // this prevents a normal 1.8.9 client from immediately re-grabbing.
            env->CallVoidMethod(minecraft,
                                guiOpen ? cache->setIngameNotInFocus : cache->setIngameFocus);
            minecraftFocusChanged = env->ExceptionCheck() != JNI_TRUE;
            clearException(env);
            env->DeleteLocalRef(minecraft);
        } else {
            clearException(env);
        }
    }

    // Lunar may transform every Minecraft symbol and therefore never publish
    // the regular cache, but LWJGL2 Mouse is still a stable public class. This
    // mapping-independent call is the essential fallback which releases raw
    // relative input on the split presentation thread.
    const bool lwjglChanged = setLwjglMouseGrabbed(
        env, guiOpen ? false : restoreGrabbed);
    if (guiOpen) {
        ::ReleaseCapture();
        ::ClipCursor(nullptr);
    } else {
        m_overlayInputSessionActive = false;
        m_inputGrabStateKnown = false;
    }
    return minecraftFocusChanged || lwjglChanged;
}

bool GameBindings::maintainInputReleased(JNIEnv* const env) noexcept
{
    if (env == nullptr) return false;
    const bool released = setLwjglMouseGrabbed(env, false);
    // Do not release Win32 capture here. ImGui deliberately owns capture while
    // dragging/resizing; cancelling it every frame made MouseDelta alternate
    // between the game and the overlay and caused both drag failure and cursor
    // flicker. The one-shot transition in setInputCaptured() releases any old
    // Minecraft capture before ImGui starts interacting.
    ::ClipCursor(nullptr);
    return released;
}

bool GameBindings::updateGameplay(JNIEnv* const env,
                                  const GameplaySettings& requested,
                                  const GameSnapshot& snapshot,
                                  const std::uint64_t tickMilliseconds) noexcept
{
    if (env == nullptr) return false;

    BindingCache* const cache =
        m_resolutionPhase.load(std::memory_order_acquire) == ResolutionPhase::Resolved
        ? m_cache.get() : nullptr;
    const bool aimCapability = cache != nullptr &&
        cache->minecraftClass != nullptr && cache->isMainThread != nullptr &&
        cache->playerField != nullptr && cache->gameSettingsField != nullptr &&
        cache->mouseSensitivity != nullptr && cache->rotationYaw != nullptr &&
        cache->rotationPitch != nullptr;
    const bool safewalkCapability = cache != nullptr &&
        cache->gameSettingsClass != nullptr && cache->keyBindingClass != nullptr &&
        cache->gameSettingsField != nullptr && cache->keyBindSneakField != nullptr &&
        cache->getKeyCode != nullptr && cache->setKeyBindState != nullptr &&
        cache->rotationPitch != nullptr && cache->isAirBlock != nullptr;

    const auto setSneakState = [&](const int keyCode, const bool down) noexcept {
        if (!safewalkCapability || keyCode <= 0) return false;
        env->CallStaticVoidMethod(cache->keyBindingClass,
                                  cache->setKeyBindState,
                                  static_cast<jint>(keyCode),
                                  down ? JNI_TRUE : JNI_FALSE);
        const bool succeeded = env->ExceptionCheck() != JNI_TRUE;
        clearException(env);
        return succeeded;
    };
    const auto releaseForcedSneak = [&]() noexcept {
        if (!m_safewalkSneakForced) return true;
        bool physicalDown = false;
        (void)queryLwjglKeyDown(env, m_safewalkSneakKeyCode, physicalDown);
        const bool released = setSneakState(m_safewalkSneakKeyCode,
                                            physicalDown);
        m_safewalkSneakForced = false;
        m_safewalkSneakKeyCode = 0;
        m_safewalkSupportMask = 0U;
        m_safewalkReleaseAt = 0U;
        return released;
    };

    const bool movementCapability = safewalkCapability &&
        cache->playerControllerClass != nullptr && cache->itemBlockClass != nullptr &&
        cache->enumFacingClass != nullptr && cache->vec3Class != nullptr &&
        cache->movementKeyFields[0U] != nullptr && cache->motionFields[0U] != nullptr &&
        cache->mouseSensitivity != nullptr && cache->rotationYaw != nullptr &&
        cache->onGround != nullptr && cache->jump != nullptr;
    const bool movementRequested = requested.safewalk || requested.scaffold ||
        requested.fly || requested.bhop || requested.longJump;
    const bool anyRequested = movementRequested || requested.aimAssist;
    if ((!anyRequested && !m_aimSensitivityModified) ||
        (!aimCapability && !movementCapability)) {
        (void)releaseForcedSneak();
        m_scaffoldPlatformYValid = false;
        return false;
    }
    if (env->PushLocalFrame(96) < 0) {
        clearException(env);
        (void)releaseForcedSneak();
        return false;
    }
    const auto finish = [&](const bool result) noexcept {
        env->PopLocalFrame(nullptr);
        return result;
    };
    const auto fail = [&]() noexcept {
        clearException(env);
        (void)releaseForcedSneak();
        return finish(false);
    };

    jobject minecraft = cache->minecraftInstanceField != nullptr
        ? env->GetStaticObjectField(cache->minecraftClass,
                                    cache->minecraftInstanceField)
        : env->CallStaticObjectMethod(cache->minecraftClass,
                                      cache->getMinecraft);
    if (env->ExceptionCheck() == JNI_TRUE || minecraft == nullptr) return fail();
    const jboolean mainThread = env->CallBooleanMethod(minecraft,
                                                       cache->isMainThread);
    if (env->ExceptionCheck() == JNI_TRUE || mainThread != JNI_TRUE) return fail();

    jobject player = env->GetObjectField(minecraft, cache->playerField);
    jobject settings = env->GetObjectField(minecraft, cache->gameSettingsField);
    if (env->ExceptionCheck() == JNI_TRUE || player == nullptr ||
        settings == nullptr) return fail();
    const jfloat pitch = env->GetFloatField(player, cache->rotationPitch);
    const jfloat yaw = env->GetFloatField(player, cache->rotationYaw);
    if (env->ExceptionCheck() == JNI_TRUE) return fail();

    // Aim assistance needs only the player rotation and GameSettings
    // sensitivity mappings. It must not inherit Safewalk/Scaffold's block,
    // inventory or controller requirements: transformed clients commonly
    // expose the former while renaming one of the latter.
    if (!requested.aimAssist) {
        m_aimFilterInitialized = false;
        m_aimFilteredTargetEntityId = -1;
    }
    if (aimCapability && (requested.aimAssist || m_aimSensitivityModified)) {
        const EntityMarker* target = nullptr;
        const double minimumDistance = static_cast<double>(std::clamp(
            requested.aimMinimumDistance, 0, 64));
        const double maximumDistance = static_cast<double>(std::clamp(
            requested.aimMaximumDistance,
            std::max(1, requested.aimMinimumDistance), 128));
        const double maximumAngle = static_cast<double>(std::clamp(
            requested.aimFovDegrees, 1, 360)) * 0.5;
        double bestScore = std::numeric_limits<double>::max();
        float desiredYaw = yaw;
        float desiredPitch = pitch;
        const auto wrap = [](double value) noexcept {
            while (value > 180.0) value -= 360.0;
            while (value < -180.0) value += 360.0;
            return value;
        };
        constexpr double aimPi = 3.14159265358979323846;
        if (requested.aimAssist) {
            for (std::uint32_t index = 0U;
                 index < snapshot.entityMarkerCount; ++index) {
                const EntityMarker& entity = snapshot.entityMarkers[index];
                // TAB membership is authoritative on Hypixel and excludes NPCs.
                // A real World.playerEntities member is sufficient in local and
                // other non-match worlds where a persistent TAB roster may not exist.
                const bool validPlayer = entity.confirmedPlayer ||
                    (!snapshot.hypixelServer && !snapshot.matchActive && entity.player);
                if (!validPlayer || entity.entityId == snapshot.entityId ||
                    (snapshot.ownTeam != 'u' &&
                     entity.teamColor == snapshot.ownTeam)) continue;
                // A short extrapolation hides the visible 20 Hz entity-step
                // cadence without inventing a server-side rotation. The local
                // player's real view is still the only state modified.
                const double targetX = entity.currentX +
                    (entity.currentX - entity.previousX) * 0.18;
                const double targetY = entity.currentY +
                    (entity.currentY - entity.previousY) * 0.18;
                const double targetZ = entity.currentZ +
                    (entity.currentZ - entity.previousZ) * 0.18;
                const double dx = targetX - snapshot.x;
                const double dz = targetZ - snapshot.z;
                // Aim at a stable torso point derived from the entity origin.
                // AABB min/max can change abruptly at pose/collision edges and
                // made the desired angle alternate on consecutive snapshots.
                const double entityHeight = std::clamp(
                    entity.bounds.maxY - entity.bounds.minY, 0.6, 2.4);
                const double dy = targetY + entityHeight * 0.62 -
                    (snapshot.y + 1.62);
                const double horizontal = std::hypot(dx, dz);
                if (horizontal < 0.1) continue;
                const double distance = std::hypot(horizontal, dy);
                if (distance < minimumDistance || distance > maximumDistance)
                    continue;
                const float targetYaw = static_cast<float>(
                    std::atan2(dz, dx) * 180.0 / aimPi - 90.0);
                const float targetPitch = static_cast<float>(
                    -std::atan2(dy, horizontal) * 180.0 / aimPi);
                const double angle = std::hypot(
                    wrap(targetYaw - yaw),
                    static_cast<double>(targetPitch - pitch));
                if (angle <= maximumAngle) {
                    // Retain a still-valid target with mild hysteresis so two
                    // nearby entities cannot make the view alternate every
                    // sample. Angle remains the dominant selection metric.
                    double score = angle + distance * 0.025;
                    if (entity.entityId == m_aimTargetEntityId) score *= 0.46;
                    if (score >= bestScore) continue;
                    bestScore = score;
                    target = &entity;
                    desiredYaw = targetYaw;
                    desiredPitch = targetPitch;
                }
            }
        }
        m_aimTargetEntityId = target == nullptr ? -1 : target->entityId;
        if (requested.aimAssist && requested.aimSlowdownMode) {
            if (target != nullptr && !m_aimSensitivityModified) {
                m_originalMouseSensitivity = env->GetFloatField(
                    settings, cache->mouseSensitivity);
                m_aimSensitivityModified = env->ExceptionCheck() != JNI_TRUE;
            }
            if (target != nullptr && m_aimSensitivityModified) {
                env->SetFloatField(settings, cache->mouseSensitivity,
                    m_originalMouseSensitivity * static_cast<float>(std::clamp(
                        requested.aimSlowdownPercent, 5, 95)) / 100.0F);
            } else if (m_aimSensitivityModified) {
                env->SetFloatField(settings, cache->mouseSensitivity,
                                   m_originalMouseSensitivity);
                m_aimSensitivityModified = false;
            }
        } else {
            if (m_aimSensitivityModified) {
                env->SetFloatField(settings, cache->mouseSensitivity,
                                   m_originalMouseSensitivity);
                m_aimSensitivityModified = false;
            }
            if (requested.aimAssist && target != nullptr) {
                const double dt = m_lastGameplayTick == 0U ? 0.05 :
                    std::clamp(static_cast<double>(tickMilliseconds -
                        m_lastGameplayTick) / 1000.0, 0.001, 0.10);
                const double speed = static_cast<double>(std::clamp(
                    requested.aimSpeedPercent, 1, 100)) / 100.0;
                // Squared response makes low settings genuinely gentle. A
                // per-second turn cap prevents any frame from snapping even
                // after a hitch, while the exponential term stays frame-rate
                // independent.
                // Filter the target angle independently from the view angle.
                // Entity updates arrive at 20 Hz while this method may be
                // sampled at a different cadence; directly chasing every new
                // sample produced the visible hitbox-edge oscillation.
                if (!m_aimFilterInitialized ||
                    m_aimFilteredTargetEntityId != target->entityId) {
                    m_aimFilteredYaw = yaw;
                    m_aimFilteredPitch = pitch;
                    m_aimFilteredTargetEntityId = target->entityId;
                    m_aimFilterInitialized = true;
                }
                const double targetFilter = 1.0 - std::exp(
                    -(5.0 + 5.0 * speed) * dt);
                m_aimFilteredYaw = static_cast<float>(yaw + wrap(
                    static_cast<double>(m_aimFilteredYaw) - yaw));
                m_aimFilteredYaw = static_cast<float>(m_aimFilteredYaw +
                    wrap(desiredYaw - m_aimFilteredYaw) * targetFilter);
                m_aimFilteredPitch = static_cast<float>(m_aimFilteredPitch +
                    (desiredPitch - m_aimFilteredPitch) * targetFilter);

                const double response = 0.28 + 5.20 * speed * speed;
                const double alpha = 1.0 - std::exp(-response * dt);
                const double maximumStep = (3.0 + 150.0 * speed * speed) * dt;
                const double yawError = wrap(m_aimFilteredYaw - yaw);
                const double pitchError = static_cast<double>(m_aimFilteredPitch - pitch);
                // A small angular dead zone prevents quantized mouse/entity
                // updates from bouncing between opposite corrections once the
                // crosshair is already settled on the target.
                constexpr double settleDeadZone = 0.22;
                const double yawStep = std::clamp(
                    std::abs(yawError) <= settleDeadZone ? 0.0 : yawError * alpha,
                    -maximumStep, maximumStep);
                const double pitchStep = std::clamp(
                    std::abs(pitchError) <= settleDeadZone ? 0.0 : pitchError * alpha,
                    -maximumStep, maximumStep);
                env->SetFloatField(player, cache->rotationYaw,
                    yaw + static_cast<float>(yawStep));
                env->SetFloatField(player, cache->rotationPitch,
                    std::clamp(pitch + static_cast<float>(pitchStep),
                               -90.0F, 90.0F));
            }
            if (!requested.aimAssist || target == nullptr) {
                m_aimFilterInitialized = false;
                m_aimFilteredTargetEntityId = -1;
            }
        }
        if (env->ExceptionCheck() == JNI_TRUE) return fail();
    }

    // Aim-only operation intentionally stops here. The remainder reads block
    // support, movement keys and inventory/controller mappings.
    if (!movementRequested || !movementCapability) {
        (void)releaseForcedSneak();
        m_scaffoldPlatformYValid = false;
        m_lastGameplayTick = tickMilliseconds;
        return finish(requested.aimAssist && aimCapability);
    }

    jobject world = env->GetObjectField(minecraft, cache->worldField);
    if (env->ExceptionCheck() == JNI_TRUE || world == nullptr) return fail();
    jobject sneakBinding = env->GetObjectField(settings, cache->keyBindSneakField);
    if (env->ExceptionCheck() == JNI_TRUE || sneakBinding == nullptr) return fail();
    const jint keyCode = env->CallIntMethod(sneakBinding, cache->getKeyCode);
    jobject bounds = env->CallObjectMethod(player, cache->getBounds);
    if (env->ExceptionCheck() == JNI_TRUE || keyCode <= 0 || bounds == nullptr)
        return fail();

    const double minX = env->GetDoubleField(bounds, cache->minX);
    const double minY = env->GetDoubleField(bounds, cache->minY);
    const double minZ = env->GetDoubleField(bounds, cache->minZ);
    const double maxX = env->GetDoubleField(bounds, cache->maxX);
    const double maxZ = env->GetDoubleField(bounds, cache->maxZ);
    if (env->ExceptionCheck() == JNI_TRUE) return fail();

    // Read physical movement once and reuse it for edge prediction, movement
    // modules and scaffold targeting. This mirrors Minecraft's movement-input
    // stage and avoids one-frame disagreement between those systems.
    std::array<bool, 6U> input{}; // forward, back, left, right, jump, sneak
    if (movementCapability) {
        for (std::size_t index = 0U; index < 5U; ++index) {
            jobject binding = env->GetObjectField(settings,
                cache->movementKeyFields[index]);
            if (env->ExceptionCheck() == JNI_TRUE || binding == nullptr) return fail();
            const jint code = env->CallIntMethod(binding, cache->getKeyCode);
            if (env->ExceptionCheck() == JNI_TRUE) return fail();
            (void)queryLwjglKeyDown(env, code, input[index]);
        }
        (void)queryLwjglKeyDown(env, keyCode, input[5U]);
    }
    const bool onGround = movementCapability &&
        env->GetBooleanField(player, cache->onGround) == JNI_TRUE;
    if (env->ExceptionCheck() == JNI_TRUE) return fail();
    const double forward = (input[0U] ? 1.0 : 0.0) - (input[1U] ? 1.0 : 0.0);
    // Minecraft's positive moveStrafing direction is left. The previous
    // right-minus-left expression inverted A and D for every movement module.
    const double strafe = (input[2U] ? 1.0 : 0.0) - (input[3U] ? 1.0 : 0.0);
    const double magnitude = std::hypot(forward, strafe);
    const double normalizedForward = magnitude > 0.001 ? forward / magnitude : 0.0;
    const double normalizedStrafe = magnitude > 0.001 ? strafe / magnitude : 0.0;
    constexpr double pi = 3.14159265358979323846;
    const double radians = static_cast<double>(yaw) * pi / 180.0;
    const double directionX = -std::sin(radians) * normalizedForward +
                              std::cos(radians) * normalizedStrafe;
    const double directionZ =  std::cos(radians) * normalizedForward +
                              std::sin(radians) * normalizedStrafe;

    // Use the motion that Minecraft actually calculated for this tick.  Key
    // intent alone is insufficient while airborne (or after sprinting over a
    // diagonal edge), because inertia can carry the player somewhere that no
    // currently pressed key points at.
    const double actualMotionX = env->GetDoubleField(
        player, cache->motionFields[0U]);
    const double actualMotionY = env->GetDoubleField(
        player, cache->motionFields[1U]);
    const double actualMotionZ = env->GetDoubleField(
        player, cache->motionFields[2U]);
    if (env->ExceptionCheck() == JNI_TRUE) return fail();

    const double sensitivity = static_cast<double>(std::clamp(
        requested.safewalkEdgeSensitivity, 0, 100)) / 100.0;
    // Preview a fraction of Minecraft's already-computed motion. At the low
    // end we still look far enough ahead to set sneak before the next physics
    // tick; at the high end the probe spans over two ticks. Together with the
    // configurable unsupported-corner threshold below this covers a much
    // wider late/early range without ever waiting until the player is airborne.
    const double previewFraction = 0.34 + sensitivity * sensitivity * 1.86;
    const double fallbackStep = magnitude > 0.001
        ? 0.012 + sensitivity * sensitivity * 0.10 : 0.0;
    const double projectedX = std::abs(actualMotionX) > 0.001
        ? actualMotionX * previewFraction : directionX * fallbackStep;
    const double projectedZ = std::abs(actualMotionZ) > 0.001
        ? actualMotionZ * previewFraction : directionZ * fallbackStep;
    constexpr double probeInset = 0.018;
    const std::array<int, 2U> supportX{
        static_cast<int>(std::floor(minX + projectedX + probeInset)),
        static_cast<int>(std::floor(maxX + projectedX - probeInset))};
    const std::array<int, 2U> supportZ{
        static_cast<int>(std::floor(minZ + projectedZ + probeInset)),
        static_cast<int>(std::floor(maxZ + projectedZ - probeInset))};
    const int supportY = static_cast<int>(std::floor(minY - 0.06));
    std::uint8_t immediateAirMask = 0U;
    std::uint8_t deepVoidMask = 0U;
    std::uint8_t bit = 1U;
    for (const int x : supportX) {
        for (const int z : supportZ) {
            jobject position = env->NewObject(cache->blockPosClass,
                                              cache->blockPosConstructor,
                                              static_cast<jint>(x),
                                              static_cast<jint>(supportY),
                                              static_cast<jint>(z));
            if (env->ExceptionCheck() == JNI_TRUE || position == nullptr)
                return fail();
            const jboolean air = env->CallBooleanMethod(world, cache->isAirBlock,
                                                        position);
            if (env->ExceptionCheck() == JNI_TRUE) return fail();
            if (air == JNI_TRUE) {
                immediateAirMask = static_cast<std::uint8_t>(immediateAirMask | bit);
                jobject deepPosition = env->NewObject(cache->blockPosClass,
                    cache->blockPosConstructor, static_cast<jint>(x),
                    static_cast<jint>(supportY - 1), static_cast<jint>(z));
                if (env->ExceptionCheck() == JNI_TRUE || deepPosition == nullptr)
                    return fail();
                const jboolean deepAir = env->CallBooleanMethod(
                    world, cache->isAirBlock, deepPosition);
                if (env->ExceptionCheck() == JNI_TRUE) return fail();
                if (deepAir == JNI_TRUE)
                    deepVoidMask = static_cast<std::uint8_t>(deepVoidMask | bit);
            }
            bit = static_cast<std::uint8_t>(bit << 1U);
        }
    }

    const auto countBits = [](std::uint8_t value) noexcept {
        int result = 0;
        for (; value != 0U; value = static_cast<std::uint8_t>(value >> 1U))
            result += static_cast<int>(value & 1U);
        return result;
    };
    const std::uint8_t unsafeMask = static_cast<std::uint8_t>(
        immediateAirMask & deepVoidMask);
    const int unsafeCorners = countBits(unsafeMask);
    // 0% requires all four projected corners to be over a two-block void;
    // 100% needs only one. Intermediate values cover the old conservative
    // behaviour as well as a late, edge-hugging mode while remaining safe.
    const int requiredUnsafeCorners = std::clamp(
        4 - static_cast<int>(std::floor(sensitivity * 3.999)), 1, 4);
    const bool supportRestored = unsafeCorners < requiredUnsafeCorners;
    const bool atEdge = unsafeCorners >= requiredUnsafeCorners && onGround;
    const bool pitchAllowsSafewalk = pitch >= static_cast<float>(std::clamp(
        requested.safewalkMinimumPitch, -90, 90));
    m_safewalkSupportMask = deepVoidMask;

    if (m_safewalkSneakForced && supportRestored &&
        m_safewalkReleaseAt == 0U) {
        m_safewalkReleaseAt = tickMilliseconds + static_cast<std::uint64_t>(
            std::clamp(requested.safewalkReleaseDelayMs, 0, 750));
    }
    if (m_safewalkSneakForced && atEdge) m_safewalkReleaseAt = 0U;
    if (m_safewalkSneakForced && m_safewalkReleaseAt != 0U &&
        tickMilliseconds >= m_safewalkReleaseAt) {
        const bool released = releaseForcedSneak();
        if (!movementCapability) return finish(released);
    }

    if (!requested.safewalk) {
        (void)releaseForcedSneak();
    } else if (!m_safewalkSneakForced && atEdge && pitchAllowsSafewalk) {
        if (setSneakState(keyCode, true)) {
            m_safewalkSneakForced = true;
            m_safewalkSneakKeyCode = keyCode;
            m_safewalkReleaseAt = 0U;
        } else return fail();
    }
    if (m_safewalkSneakForced && !pitchAllowsSafewalk) {
        const bool released = releaseForcedSneak();
        if (!movementCapability) return finish(released);
    }

    if (!movementCapability) return finish(m_safewalkSneakForced);

    if (requested.fly) {
        const double speed = 0.34 * static_cast<double>(std::clamp(
            requested.flySpeedPercent, 10, 500)) / 100.0;
        env->SetDoubleField(player, cache->motionFields[0U], directionX * speed);
        env->SetDoubleField(player, cache->motionFields[2U], directionZ * speed);
        const double vertical = (input[4U] ? speed : 0.0) -
                                (input[5U] ? speed : 0.0);
        env->SetDoubleField(player, cache->motionFields[1U], vertical);
        if (env->ExceptionCheck() == JNI_TRUE) return fail();
    } else if (requested.bhop && magnitude > 0.001) {
        const double airSpeed = 0.30 * static_cast<double>(std::clamp(
            requested.bhopAirSpeedPercent, 10, 300)) / 100.0;
        if (onGround && requested.bhopAutoJump) {
            env->CallVoidMethod(player, cache->jump);
            if (env->ExceptionCheck() == JNI_TRUE) return fail();
            // EntityLivingBase.jump applies the vanilla sprint impulse. Clamp
            // only excess speed on the landing/jump frame so Auto Jump cannot
            // create a one-tick boost, while preserving slower player motion.
            const double jumpX = env->GetDoubleField(player, cache->motionFields[0U]);
            const double jumpZ = env->GetDoubleField(player, cache->motionFields[2U]);
            const double jumpHorizontal = std::hypot(jumpX, jumpZ);
            if (jumpHorizontal > airSpeed && jumpHorizontal > 0.0001) {
                const double scale = airSpeed / jumpHorizontal;
                env->SetDoubleField(player, cache->motionFields[0U], jumpX * scale);
                env->SetDoubleField(player, cache->motionFields[2U], jumpZ * scale);
            }
        } else if (!onGround) {
            env->SetDoubleField(player, cache->motionFields[0U], directionX * airSpeed);
            env->SetDoubleField(player, cache->motionFields[2U], directionZ * airSpeed);
            if (env->ExceptionCheck() == JNI_TRUE) return fail();
        }
    } else if (requested.longJump && onGround && magnitude > 0.001 &&
               tickMilliseconds - m_lastLongJumpTick >= 650U) {
        const double speed = 0.72 * static_cast<double>(std::clamp(
            requested.longJumpSpeedPercent, 25, 250)) / 100.0;
        env->SetDoubleField(player, cache->motionFields[0U], directionX * speed);
        env->SetDoubleField(player, cache->motionFields[1U], 0.42);
        env->SetDoubleField(player, cache->motionFields[2U], directionZ * speed);
        if (env->ExceptionCheck() == JNI_TRUE) return fail();
        m_lastLongJumpTick = tickMilliseconds;
    }

    if (requested.scaffold &&
        tickMilliseconds - m_lastScaffoldPlacementTick >= 35U) {
        const int supportLayer = static_cast<int>(std::floor(minY - 0.06));
        if (!m_scaffoldPlatformYValid || onGround) {
            m_scaffoldPlatformY = supportLayer;
            m_scaffoldPlatformYValid = true;
        }
        jobject inventory = env->GetObjectField(player, cache->inventoryField);
        jobject controller = env->GetObjectField(minecraft,
            cache->playerControllerField);
        jobjectArray hotbar = inventory == nullptr ? nullptr :
            static_cast<jobjectArray>(env->GetObjectField(inventory,
                cache->mainInventory));
        if (env->ExceptionCheck() == JNI_TRUE) return fail();
        auto allowedBlock = [](const int id) noexcept {
            if (id == 12 || id == 13) return false; // sand / gravel fall
            switch (id) {
            case 1: case 4: case 5: case 24: case 35: case 45:
            case 87: case 98: case 121: case 159: return true;
            default: return false;
            }
        };
        int selectedSlot = -1;
        jobject selectedStack = nullptr;
        if (hotbar != nullptr && controller != nullptr) {
            const jsize length = std::min<jsize>(9, env->GetArrayLength(hotbar));
            const int current = std::clamp(
                static_cast<int>(env->GetIntField(inventory, cache->currentItem)),
                0, std::max(0, static_cast<int>(length) - 1));
            for (jsize pass = 0; pass < length; ++pass) {
                const int slot = pass == 0 ? current :
                    (static_cast<int>(pass) <= current
                        ? static_cast<int>(pass) - 1 : static_cast<int>(pass));
                jobject stack = env->GetObjectArrayElement(hotbar, slot);
                if (stack == nullptr) continue;
                jobject itemObject = env->CallObjectMethod(stack, cache->getItem);
                if (env->ExceptionCheck() == JNI_TRUE) return fail();
                if (itemObject != nullptr && env->IsInstanceOf(
                        itemObject, cache->itemBlockClass) == JNI_TRUE) {
                    jobject blockObject = env->CallObjectMethod(
                        itemObject, cache->getBlockFromItem);
                    const jint blockId = blockObject == nullptr ? -1 :
                        env->CallStaticIntMethod(cache->blockClass,
                            cache->getIdFromBlock, blockObject);
                    if (env->ExceptionCheck() == JNI_TRUE) return fail();
                    if (allowedBlock(blockId)) {
                        selectedSlot = slot;
                        selectedStack = stack;
                        break;
                    }
                }
            }
        }
        if (selectedSlot >= 0 && selectedStack != nullptr) {
            const double centerX = (minX + maxX) * 0.5;
            const double centerZ = (minZ + maxZ) * 0.5;
            std::array<std::array<int, 3U>, 32U> targets{};
            std::size_t targetCount = 0U;
            const auto addTargetAt = [&](const double x, const int layer,
                                         const double z) noexcept {
                const std::array<int, 3U> candidate{
                    static_cast<int>(std::floor(x)), layer,
                    static_cast<int>(std::floor(z))};
                for (std::size_t i = 0; i < targetCount; ++i)
                    if (targets[i] == candidate) return;
                if (targetCount < targets.size()) targets[targetCount++] = candidate;
            };
            // Start with the current footprint, then integrate the player's
            // real velocity through several vanilla-like air ticks. This keeps
            // diagonal sprint jumps covered even after the player releases or
            // changes a movement key mid-air.
            constexpr double cornerInset = 0.025;
            const double halfWidthX = std::max(0.0,
                (maxX - minX) * 0.5 - cornerInset);
            const double halfWidthZ = std::max(0.0,
                (maxZ - minZ) * 0.5 - cornerInset);
            const auto addFootprintAt = [&](const double x, const int layer,
                                            const double z) noexcept {
                addTargetAt(x, layer, z);
                addTargetAt(x - halfWidthX, layer, z - halfWidthZ);
                addTargetAt(x - halfWidthX, layer, z + halfWidthZ);
                addTargetAt(x + halfWidthX, layer, z - halfWidthZ);
                addTargetAt(x + halfWidthX, layer, z + halfWidthZ);
            };
            // Safety layer one: repair the cells immediately beneath the live
            // collision footprint, even with no movement key held. This is the
            // path that catches residual sprint/jump inertia and vertical jumps.
            addFootprintAt(centerX, supportLayer, centerZ);
            if (supportLayer != m_scaffoldPlatformY)
                addFootprintAt(centerX, m_scaffoldPlatformY, centerZ);

            double simulatedX = centerX;
            double simulatedY = minY;
            double simulatedZ = centerZ;
            double simulatedMotionX = actualMotionX;
            double simulatedMotionY = actualMotionY;
            double simulatedMotionZ = actualMotionZ;
            if (std::hypot(simulatedMotionX, simulatedMotionZ) < 0.012 &&
                magnitude > 0.001) {
                simulatedMotionX = directionX * 0.10;
                simulatedMotionZ = directionZ * 0.10;
            }
            for (int predictionTick = 0; predictionTick < 6; ++predictionTick) {
                simulatedX += simulatedMotionX;
                simulatedY += simulatedMotionY;
                simulatedZ += simulatedMotionZ;
                // Safety layer two: predict from actual motion, then add input
                // acceleration only as a secondary correction.
                addFootprintAt(simulatedX, m_scaffoldPlatformY, simulatedZ);

                // 1.8.x EntityLivingBase air motion approximation. Input is a
                // small acceleration/fallback; existing inertia remains the
                // dominant signal and therefore also covers jump momentum.
                if (magnitude > 0.001) {
                    simulatedMotionX += directionX * 0.012;
                    simulatedMotionZ += directionZ * 0.012;
                }
                simulatedMotionX *= 0.91;
                simulatedMotionZ *= 0.91;
                simulatedMotionY = (simulatedMotionY - 0.08) * 0.98;
                if (simulatedY <= static_cast<double>(m_scaffoldPlatformY) +
                                  1.02 && predictionTick >= 1) break;
            }
            constexpr std::array<std::array<int, 4U>, 5U> neighbours{{
                {{0,-1,0,1}}, {{0,0,-1,3}}, {{0,0,1,2}},
                {{-1,0,0,5}}, {{1,0,0,4}}}};
            bool placed = false;
            for (std::size_t targetIndex = 0U; targetIndex < targetCount; ++targetIndex) {
                const auto& target = targets[targetIndex];
                jobject targetPos = env->NewObject(cache->blockPosClass,
                    cache->blockPosConstructor, target[0U], target[1U], target[2U]);
                if (targetPos == nullptr || env->ExceptionCheck() == JNI_TRUE) return fail();
                if (env->CallBooleanMethod(world, cache->isAirBlock, targetPos) != JNI_TRUE) {
                    if (env->ExceptionCheck() == JNI_TRUE) return fail();
                    continue;
                }
                for (const auto& side : neighbours) {
                    jobject neighbour = env->NewObject(cache->blockPosClass,
                        cache->blockPosConstructor, target[0U] + side[0U],
                        target[1U] + side[1U], target[2U] + side[2U]);
                    if (neighbour == nullptr || env->ExceptionCheck() == JNI_TRUE) return fail();
                    const jboolean neighbourAir = env->CallBooleanMethod(
                        world, cache->isAirBlock, neighbour);
                    if (env->ExceptionCheck() == JNI_TRUE) return fail();
                    if (neighbourAir == JNI_TRUE) continue;
                    jobject face = env->CallStaticObjectMethod(cache->enumFacingClass,
                        cache->getFacingByIndex, side[3U]);
                    jobject hit = env->NewObject(cache->vec3Class,
                        cache->vec3Constructor, target[0U] + 0.5,
                        target[1U] + 0.5, target[2U] + 0.5);
                    if (face == nullptr || hit == nullptr ||
                        env->ExceptionCheck() == JNI_TRUE) return fail();
                    env->SetIntField(inventory, cache->currentItem, selectedSlot);
                    const jboolean accepted = env->CallBooleanMethod(controller,
                        cache->onPlayerRightClick, player, world, selectedStack,
                        neighbour, face, hit);
                    if (env->ExceptionCheck() == JNI_TRUE) return fail();
                    if (accepted == JNI_TRUE) {
                        m_lastScaffoldPlacementTick = tickMilliseconds;
                        placed = true;
                        break;
                    }
                }
                if (placed) break;
            }
        }
    } else if (!requested.scaffold) {
        m_scaffoldPlatformYValid = false;
    }

    m_lastGameplayTick = tickMilliseconds;
    return finish(m_safewalkSneakForced || requested.scaffold || requested.fly ||
                  requested.bhop || requested.aimAssist || requested.longJump);
}

void GameBindings::enqueueDebugChatLine(const std::string_view line) noexcept
{
    if (line.empty()) return;
    std::array<char, DebugLineCapacity> copy{};
    std::size_t length = 0U;
    for (const char character : line) {
        if (length + 1U >= copy.size()) break;
        if (character == '\r' || character == '\n' || character == '\t') continue;
        copy[length++] = character;
    }
    if (length == 0U) return;

    ::AcquireSRWLockExclusive(&m_debugQueueLock);
    std::uint32_t target = 0U;
    if (m_debugQueueCount < DebugQueueCapacity) {
        target = (m_debugQueueHead + m_debugQueueCount) % DebugQueueCapacity;
        ++m_debugQueueCount;
    } else {
        target = m_debugQueueHead;
        m_debugQueueHead = (m_debugQueueHead + 1U) % DebugQueueCapacity;
    }
    m_debugQueue[target] = copy;
    ::ReleaseSRWLockExclusive(&m_debugQueueLock);
}

void GameBindings::enqueueWarningChatLine(
    const std::string_view playerName,
    const std::string_view reason) noexcept
{
    if (playerName.empty()) return;
    WarningChatLine line{};
    const auto copyClean = [](const std::string_view source, char* const target,
                              const std::size_t capacity) noexcept {
        std::size_t length = 0U;
        for (const char character : source) {
            if (length + 1U >= capacity) break;
            if (character == '\r' || character == '\n' || character == '\t' ||
                character == '\0') continue;
            target[length++] = character;
        }
    };
    copyClean(playerName, line.playerName.data(), line.playerName.size());
    copyClean(reason, line.reason.data(), line.reason.size());
    if (line.playerName[0U] == '\0') return;

    ::AcquireSRWLockExclusive(&m_warningQueueLock);
    std::uint32_t target = 0U;
    if (m_warningQueueCount < WarningQueueCapacity) {
        target = (m_warningQueueHead + m_warningQueueCount) % WarningQueueCapacity;
        ++m_warningQueueCount;
    } else {
        target = m_warningQueueHead;
        m_warningQueueHead = (m_warningQueueHead + 1U) % WarningQueueCapacity;
    }
    m_warningQueue[target] = line;
    ::ReleaseSRWLockExclusive(&m_warningQueueLock);
}

void GameBindings::publishDebugChat(JNIEnv* const env, const bool enabled) noexcept
{
    std::uint32_t warningCount = 0U;
    ::AcquireSRWLockShared(&m_warningQueueLock);
    warningCount = m_warningQueueCount;
    ::ReleaseSRWLockShared(&m_warningQueueLock);
    if (!enabled) {
        // Re-enabling the option should print the current state even if the
        // roster itself did not change while the option was disabled.
        m_debugRosterGeneration = 0U;
        m_debugBedOwnershipGeneration = 0U;
        m_debugMatchProbeGeneration = 0U;
        ::AcquireSRWLockExclusive(&m_debugQueueLock);
        m_debugQueueHead = 0U;
        m_debugQueueCount = 0U;
        ::ReleaseSRWLockExclusive(&m_debugQueueLock);
    }
    std::uint32_t pendingCount = 0U;
    ::AcquireSRWLockShared(&m_debugQueueLock);
    pendingCount = m_debugQueueCount;
    ::ReleaseSRWLockShared(&m_debugQueueLock);
    const bool probeChanged = enabled && m_matchProbeGeneration !=
        m_debugMatchProbeGeneration;
    const bool rosterChanged = enabled && m_snapshot.matchActive &&
        m_snapshot.playerRosterGeneration != 0U &&
        m_snapshot.playerRosterGeneration != m_debugRosterGeneration;
    const bool bedChanged = enabled && m_snapshot.matchActive &&
        m_bedOwnershipGeneration !=
        m_debugBedOwnershipGeneration;
    if (env == nullptr ||
        (!probeChanged && !rosterChanged && !bedChanged && pendingCount == 0U &&
         warningCount == 0U) ||
        m_resolutionPhase.load(std::memory_order_acquire) != ResolutionPhase::Resolved ||
        m_cache == nullptr) {
        return;
    }

    BindingCache* const cache = m_cache.get();
    if (cache->chatTextClass == nullptr || cache->chatTextConstructor == nullptr ||
        cache->addChatMessage == nullptr || env->PushLocalFrame(192) != JNI_OK) {
        clearException(env);
        return;
    }

    jobject minecraft = cache->minecraftInstanceField != nullptr
        ? env->GetStaticObjectField(cache->minecraftClass, cache->minecraftInstanceField)
        : env->CallStaticObjectMethod(cache->minecraftClass, cache->getMinecraft);
    jobject player = minecraft == nullptr ? nullptr :
        env->GetObjectField(minecraft, cache->playerField);
    if (env->ExceptionCheck() == JNI_TRUE || player == nullptr) {
        clearException(env);
        env->PopLocalFrame(nullptr);
        return;
    }

    // Consume generations only after a usable local chat endpoint and player
    // object exist. This avoids silently discarding the exact diagnostics that
    // are needed when a transformed client temporarily exposes an incomplete
    // game state.
    if (enabled) {
        m_debugMatchProbeGeneration = m_matchProbeGeneration;
        m_debugRosterGeneration = m_snapshot.playerRosterGeneration;
        m_debugBedOwnershipGeneration = m_bedOwnershipGeneration;
    }
    std::array<std::array<char, DebugLineCapacity>, DebugQueueCapacity> queued{};
    ::AcquireSRWLockExclusive(&m_debugQueueLock);
    const std::uint32_t queuedCount = m_debugQueueCount;
    for (std::uint32_t index = 0U; index < queuedCount; ++index) {
        queued[index] = m_debugQueue[
            (m_debugQueueHead + index) % DebugQueueCapacity];
    }
    m_debugQueueHead = 0U;
    m_debugQueueCount = 0U;
    ::ReleaseSRWLockExclusive(&m_debugQueueLock);
    std::array<WarningChatLine, WarningQueueCapacity> warnings{};
    ::AcquireSRWLockExclusive(&m_warningQueueLock);
    const std::uint32_t queuedWarningCount = m_warningQueueCount;
    for (std::uint32_t index = 0U; index < queuedWarningCount; ++index) {
        warnings[index] = m_warningQueue[
            (m_warningQueueHead + index) % WarningQueueCapacity];
    }
    m_warningQueueHead = 0U;
    m_warningQueueCount = 0U;
    ::ReleaseSRWLockExclusive(&m_warningQueueLock);

    // Each letter is deliberately assigned a different legacy chat color.
    // This component is added straight to EntityPlayerSP and never reaches a
    // network handler, so the message remains visible only to this client.
    constexpr std::string_view prefix{
        "[\xC2\xA7" "bD\xC2\xA7" "de\xC2\xA7" "ab\xC2\xA7" "eu\xC2\xA7" "6g\xC2\xA7" "r] "};
    auto addLine = [&](const std::string& body) noexcept {
        const std::string line = std::string(prefix) + body;
        jstring text = env->NewStringUTF(line.c_str());
        jobject component = text == nullptr ? nullptr :
            env->NewObject(cache->chatTextClass, cache->chatTextConstructor, text);
        if (component != nullptr && env->ExceptionCheck() != JNI_TRUE) {
            env->CallVoidMethod(player, cache->addChatMessage, component);
        }
        clearException(env);
    };
    const auto jsonEscape = [](const char* const source) {
        std::string escaped;
        if (source == nullptr) return escaped;
        escaped.reserve(std::strlen(source) + 8U);
        for (const unsigned char character : std::string_view(source)) {
            switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character >= 0x20U) escaped.push_back(
                    static_cast<char>(character));
                break;
            }
        }
        return escaped;
    };
    auto addWarning = [&](const WarningChatLine& warning) noexcept {
        const std::string name = jsonEscape(warning.playerName.data());
        const std::string reason = jsonEscape(warning.reason.data());
        if (cache->chatSerializerClass != nullptr &&
            cache->parseChatJson != nullptr) {
            const std::string command = "/wdr " + name;
            const std::string json =
                "{\"text\":\"\",\"clickEvent\":{\"action\":\"suggest_command\","
                "\"value\":\"" + command + "\"},\"extra\":["
                "{\"text\":\"[WARNING]\",\"color\":\"red\",\"bold\":true},"
                "{\"text\":\" Blacklisted player \"},"
                "{\"text\":\"" + name + "\",\"color\":\"gold\",\"bold\":true},"
                "{\"text\":\" — " + reason +
                " (click to prepare /wdr)\",\"color\":\"yellow\"}]}";
            jstring text = env->NewStringUTF(json.c_str());
            jobject component = text == nullptr ? nullptr :
                env->CallStaticObjectMethod(cache->chatSerializerClass,
                                            cache->parseChatJson, text);
            if (component != nullptr && env->ExceptionCheck() != JNI_TRUE) {
                env->CallVoidMethod(player, cache->addChatMessage, component);
                clearException(env);
                return;
            }
            clearException(env);
        }
        std::string fallback =
            "\xC2\xA7" "c\xC2\xA7" "l[WARNING]\xC2\xA7" "r Blacklisted player ";
        fallback += warning.playerName.data();
        fallback += " - ";
        fallback += warning.reason.data();
        fallback += " \xC2\xA7" "e(/wdr ";
        fallback += warning.playerName.data();
        fallback += ')';
        jstring text = env->NewStringUTF(fallback.c_str());
        jobject component = text == nullptr ? nullptr :
            env->NewObject(cache->chatTextClass, cache->chatTextConstructor, text);
        if (component != nullptr && env->ExceptionCheck() != JNI_TRUE)
            env->CallVoidMethod(player, cache->addChatMessage, component);
        clearException(env);
    };

    if (probeChanged) {
        const auto teamText = [](const char team) noexcept {
            return team == 'u' ? std::string("unknown") : std::string(1U, team);
        };
        std::string body = "match_probe active=";
        body += m_snapshot.matchActive ? "true" : "false";
        body += " sidebar=" + std::to_string(m_matchProbe.sidebarAvailable ? 1 : 0);
        body += " tab=" + std::to_string(m_matchProbe.tabAvailable ? 1 : 0);
        body += " lines=" + std::to_string(m_matchProbe.sidebarLines);
        body += " teams=" + std::to_string(m_matchProbe.sidebarTeams);
        body += " you=" + std::to_string(m_matchProbe.sidebarYouRows);
        body += " roster_tags=" + std::to_string(m_matchProbe.rosterTaggedPlayers);
        body += " roster_players=" + std::to_string(m_matchProbe.rosterPlayers);
        body += " roster_teams=" + std::to_string(m_matchProbe.rosterTeams);
        body += " roster_own=" + teamText(m_matchProbe.rosterOwnTeam);
        body += " armor_own=" + teamText(m_matchProbe.localArmorTeam);
        body += " armor_teams=" + std::to_string(m_matchProbe.armorTeams);
        body += " evidence=";
        if (m_matchProbe.sidebarEvidence) body += 'S';
        if (m_matchProbe.rosterEvidence) body += 'R';
        if (m_matchProbe.armorEvidence) body += 'A';
        if (!m_matchProbe.sidebarEvidence && !m_matchProbe.rosterEvidence &&
            !m_matchProbe.armorEvidence) body += '-';
        body += " stable=" + std::to_string(m_matchProbe.stableCount);
        body += " beds=" + std::to_string(m_snapshot.bedMarkerCount);
        addLine(body);
    }

    if (rosterChanged) {
        std::string matchLine = "game_started=true own_team=";
        if (m_snapshot.ownTeam == 'u') {
            matchLine += "unknown";
        } else {
            matchLine += "\xC2\xA7";
            matchLine.push_back(m_snapshot.ownTeam);
            matchLine.push_back(m_snapshot.ownTeam);
            matchLine += "\xC2\xA7r";
        }
        addLine(matchLine);

        for (std::uint32_t index = 0U; index < m_snapshot.playerCount; ++index) {
            const PlayerIdentity& identity = m_snapshot.players[index];
            if (identity.name[0U] == '\0') continue;
            const bool teammate = m_snapshot.ownTeam != 'u' &&
                identity.teamColor == m_snapshot.ownTeam;
            std::string body = "player=";
            if (identity.teamColor != 'u') {
                body += "\xC2\xA7";
                body.push_back(identity.teamColor);
            }
            body += identity.name.data();
            body += "\xC2\xA7r team=";
            if (identity.teamColor == 'u') {
                body += "unknown";
            } else {
                body += "\xC2\xA7";
                body.push_back(identity.teamColor);
                body.push_back(identity.teamColor);
                body += "\xC2\xA7r";
            }
            body += " teammate=";
            body += teammate ? "true" : "false";
            addLine(body);
        }
    }
    if (bedChanged) {
        std::string body = "own_bed=";
        if (!m_snapshot.ownBedKnown) {
            body += "unknown";
        } else {
            body += "true pos=" + std::to_string(m_snapshot.ownBedX) + "," +
                std::to_string(m_snapshot.ownBedY) + "," +
                std::to_string(m_snapshot.ownBedZ) + " source=";
            body += m_snapshot.ownBedSource == GameSnapshot::OwnBedSource::TeamWool
                ? "team_wool" : "match_spawn";
        }
        addLine(body);
    }
    for (std::uint32_t index = 0U; index < queuedCount; ++index) {
        if (queued[index][0U] != '\0') addLine(queued[index].data());
    }
    for (std::uint32_t index = 0U; index < queuedWarningCount; ++index) {
        if (warnings[index].playerName[0U] != '\0') addWarning(warnings[index]);
    }
    env->PopLocalFrame(nullptr);
}

void GameBindings::sampleCamera(JNIEnv* const env) noexcept
{
    m_snapshot.camera.valid = false;
    if (env == nullptr ||
        m_resolutionPhase.load(std::memory_order_acquire) != ResolutionPhase::Resolved ||
        m_cache == nullptr) {
        return;
    }
    BindingCache* const cache = m_cache.get();
    if (cache->renderManagerObject == nullptr || cache->timerObject == nullptr ||
        cache->modelViewBuffer == nullptr ||
        cache->projectionBuffer == nullptr || cache->viewportBuffer == nullptr) {
        return;
    }

    WorldCameraSnapshot camera{};
    camera.renderX = env->GetDoubleField(cache->renderManagerObject,
                                         cache->renderPosition[0U]);
    camera.renderY = env->GetDoubleField(cache->renderManagerObject,
                                         cache->renderPosition[1U]);
    camera.renderZ = env->GetDoubleField(cache->renderManagerObject,
                                         cache->renderPosition[2U]);
    camera.partialTicks = env->GetFloatField(cache->timerObject,
                                             cache->renderPartialTicks);
    if (env->ExceptionCheck() == JNI_TRUE) {
        clearException(env);
        return;
    }

    const auto* const modelView = static_cast<const float*>(
        env->GetDirectBufferAddress(cache->modelViewBuffer));
    const auto* const projection = static_cast<const float*>(
        env->GetDirectBufferAddress(cache->projectionBuffer));
    const auto* const viewport = static_cast<const jint*>(
        env->GetDirectBufferAddress(cache->viewportBuffer));
    if (modelView == nullptr || projection == nullptr || viewport == nullptr ||
        env->GetDirectBufferCapacity(cache->modelViewBuffer) < 16 ||
        env->GetDirectBufferCapacity(cache->projectionBuffer) < 16 ||
        env->GetDirectBufferCapacity(cache->viewportBuffer) < 4) {
        clearException(env);
        return;
    }
    std::copy_n(modelView, camera.modelView.size(), camera.modelView.begin());
    std::copy_n(projection, camera.projection.size(), camera.projection.begin());
    for (std::size_t index = 0U; index < camera.viewport.size(); ++index) {
        camera.viewport[index] = viewport[index];
    }

    const bool finiteMatrices = std::all_of(
        camera.modelView.begin(), camera.modelView.end(),
        [](const float value) noexcept { return std::isfinite(value); }) &&
        std::all_of(camera.projection.begin(), camera.projection.end(),
        [](const float value) noexcept { return std::isfinite(value); });
    const bool saneViewport = camera.viewport[2U] >= 1 && camera.viewport[2U] <= 32768 &&
                              camera.viewport[3U] >= 1 && camera.viewport[3U] <= 32768;
    const bool nonEmptyMatrices = std::abs(camera.modelView[15U]) > 0.000001F &&
                                  std::abs(camera.projection[0U]) > 0.000001F &&
                                  std::abs(camera.projection[5U]) > 0.000001F;
    camera.valid = finiteMatrices && saneViewport && nonEmptyMatrices &&
                   std::isfinite(camera.renderX) && std::isfinite(camera.renderY) &&
                   std::isfinite(camera.renderZ) && std::isfinite(camera.partialTicks) &&
                   camera.partialTicks >= 0.0F && camera.partialTicks <= 1.5F;
    m_snapshot.camera = camera;
}

void GameBindings::runBedScanner(JNIEnv* const env, HANDLE const stopEvent) noexcept
{
    if (env == nullptr || stopEvent == nullptr) return;

    using ChunkBeds = std::unordered_map<std::uint64_t, std::vector<BedMarker>>;
    struct DefenseSample final {
        int x = 0;
        int y = 0;
        int z = 0;
        std::uint16_t blockId = 0U;
        std::uint8_t metadata = 0U;
    };
    using ChunkDefense = std::unordered_map<std::uint64_t, std::vector<DefenseSample>>;
    ChunkBeds bedsByChunk;
    ChunkDefense defenseByChunk;
    std::unordered_set<std::uint64_t> processedChunks;
    std::unordered_set<std::uint64_t> currentChunks;
    jobject worldIdentity = nullptr;
    std::uint64_t generation = 0U;
    std::uint64_t lastVerification = 0U;
    std::uint64_t lastDefenseRefresh = 0U;
    const HANDLE waitHandles[2]{stopEvent, m_bedRescanEvent};
    const DWORD waitHandleCount = m_bedRescanEvent != nullptr ? 2U : 1U;
    const auto stopRequested = [&](const DWORD timeout) noexcept {
        const DWORD wait = ::WaitForMultipleObjects(
            waitHandleCount, waitHandles, FALSE, timeout);
        return wait == WAIT_OBJECT_0 || wait == WAIT_FAILED;
    };

    const auto chunkKey = [](const jint x, const jint z) noexcept {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
               static_cast<std::uint32_t>(z);
    };
    const auto publish = [&]() noexcept {
        PublishedBedCache next{};
        next.loadedChunkCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(processedChunks.size(), UINT32_MAX));
        next.generation = ++generation;
        for (const auto& [key, markers] : bedsByChunk) {
            (void)key;
            for (const BedMarker& marker : markers) {
                if (next.markerCount >= next.markers.size()) break;
                BedMarker enriched = marker;
                std::array<unsigned, 8U> teamWoolEvidence{};
                // The bulk-copied sparse material cache is folded into exact
                // distance rings here on the scanner thread. SwapBuffers only
                // sums at most ten small counters when drawing the panel.
                for (const auto& [defenseKey, samples] : defenseByChunk) {
                    const auto chunkX = static_cast<std::int32_t>(defenseKey >> 32U);
                    const auto chunkZ = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(defenseKey));
                    const auto floorChunk = [](const int coordinate) noexcept {
                        return coordinate >= 0 ? coordinate / 16 : (coordinate - 15) / 16;
                    };
                    const int headChunkX = floorChunk(marker.x);
                    const int headChunkZ = floorChunk(marker.z);
                    const int footChunkX = floorChunk(marker.footX);
                    const int footChunkZ = floorChunk(marker.footZ);
                    const bool nearHead = std::abs(chunkX - headChunkX) <= 1 &&
                                          std::abs(chunkZ - headChunkZ) <= 1;
                    const bool nearFoot = std::abs(chunkX - footChunkX) <= 1 &&
                                          std::abs(chunkZ - footChunkZ) <= 1;
                    if (!nearHead && !nearFoot) continue;

                    for (const DefenseSample& sample : samples) {
                        const int vertical = sample.y - marker.y;
                        if (vertical < 0 || vertical > 10) continue;
                        const int dx = std::min(std::abs(sample.x - marker.x),
                                                std::abs(sample.x - marker.footX));
                        const int dz = std::min(std::abs(sample.z - marker.z),
                                                std::abs(sample.z - marker.footZ));
                        const int ring = std::max({dx, dz, vertical});
                        if (ring < 1 || ring > 10) continue;

                        if (sample.blockId == 35U && ring <= 6) {
                            const bedwars::Team woolTeam =
                                bedwars::fromWoolMetadata(sample.metadata);
                            const std::uint8_t woolIndex = bedwars::teamIndex(woolTeam);
                            if (woolIndex < teamWoolEvidence.size()) {
                                // Nearby wool is stronger evidence than map
                                // decoration near the edge of the scan radius.
                                teamWoolEvidence[woolIndex] +=
                                    static_cast<unsigned>(7 - ring);
                            }
                        }

                        std::uint8_t normalizedMeta = sample.metadata;
                        if (sample.blockId == 17U || sample.blockId == 162U) {
                            normalizedMeta &= 0x3U;
                        }

                        BedDefenseBlock* summary = nullptr;
                        for (std::size_t index = 0U; index < enriched.defenseCount; ++index) {
                            BedDefenseBlock& candidate = enriched.defense[index];
                            if (candidate.blockId == sample.blockId &&
                                candidate.metadata == normalizedMeta) {
                                summary = &candidate;
                                break;
                            }
                        }
                        if (summary == nullptr &&
                            enriched.defenseCount < enriched.defense.size()) {
                            summary = &enriched.defense[enriched.defenseCount++];
                            summary->blockId = sample.blockId;
                            summary->metadata = normalizedMeta;
                        }
                        if (summary != nullptr) {
                            std::uint16_t& count = summary->ringCounts[
                                static_cast<std::size_t>(ring)];
                            if (count != std::numeric_limits<std::uint16_t>::max()) ++count;
                        }
                    }
                }
                unsigned bestEvidence = 0U;
                unsigned secondEvidence = 0U;
                std::uint8_t bestTeam = 0xFFU;
                for (std::uint8_t index = 0U; index < teamWoolEvidence.size(); ++index) {
                    const unsigned evidence = teamWoolEvidence[index];
                    if (evidence > bestEvidence) {
                        secondEvidence = bestEvidence;
                        bestEvidence = evidence;
                        bestTeam = index;
                    } else if (evidence > secondEvidence) {
                        secondEvidence = evidence;
                    }
                }
                // Fail closed on weak or mixed-colour evidence. This removes
                // the previous "nearest bed" false ownership assignment.
                if (bestTeam < 8U && bestEvidence >= 4U &&
                    bestEvidence >= secondEvidence + 2U) {
                    enriched.teamColor = bedwars::formatCode(
                        static_cast<bedwars::Team>(bestTeam + 1U));
                }
                next.markers[next.markerCount++] = enriched;
            }
            if (next.markerCount >= next.markers.size()) break;
        }
        ::AcquireSRWLockExclusive(&m_bedCacheLock);
        m_publishedBedCache = next;
        ::ReleaseSRWLockExclusive(&m_bedCacheLock);
    };
    const auto clearWorld = [&]() noexcept {
        processedChunks.clear();
        currentChunks.clear();
        bedsByChunk.clear();
        defenseByChunk.clear();
        publish();
    };

    while (::WaitForSingleObject(stopEvent, 0U) == WAIT_TIMEOUT) {
        if (m_resolutionPhase.load(std::memory_order_acquire) != ResolutionPhase::Resolved ||
            m_cache == nullptr) {
            if (stopRequested(100U)) break;
            continue;
        }
        BindingCache* const cache = m_cache.get();
        // Only this scanner thread owns processedChunks/bedsByChunk. The UI and
        // IPC threads publish one atomic bit, so a manual refresh cannot race
        // vector/hash-table mutation or issue JNI calls from the wrong thread.
        if (m_bedRescanRequested.exchange(false, std::memory_order_acq_rel)) {
            processedChunks.clear();
        }
        if (env->PushLocalFrame(96) < 0) {
            clearException(env);
            if (stopRequested(500U)) break;
            continue;
        }

        bool cycleValid = true;
        jobject minecraft = cache->minecraftInstanceField != nullptr
            ? env->GetStaticObjectField(cache->minecraftClass, cache->minecraftInstanceField)
            : env->CallStaticObjectMethod(cache->minecraftClass, cache->getMinecraft);
        if (env->ExceptionCheck() == JNI_TRUE || minecraft == nullptr) cycleValid = false;
        jboolean singlePlayer = JNI_FALSE;
        jobject world = nullptr;
        if (cycleValid) {
            singlePlayer = JNI_TRUE;
            if (env->ExceptionCheck() == JNI_TRUE) cycleValid = false;
        }
        if (cycleValid) {
            world = env->GetObjectField(minecraft, cache->worldField);
            if (env->ExceptionCheck() == JNI_TRUE) cycleValid = false;
        }

        if (!cycleValid || singlePlayer != JNI_TRUE || world == nullptr) {
            clearException(env);
            if (worldIdentity != nullptr) {
                env->DeleteGlobalRef(worldIdentity);
                worldIdentity = nullptr;
            }
            if (!processedChunks.empty() || !bedsByChunk.empty()) clearWorld();
            env->PopLocalFrame(nullptr);
            if (stopRequested(500U)) break;
            continue;
        }

        if (worldIdentity == nullptr || env->IsSameObject(worldIdentity, world) != JNI_TRUE) {
            if (worldIdentity != nullptr) env->DeleteGlobalRef(worldIdentity);
            worldIdentity = env->NewGlobalRef(world);
            clearWorld();
        }

        jobject provider = env->CallObjectMethod(world, cache->getChunkProvider);
        if (env->ExceptionCheck() == JNI_TRUE || provider == nullptr ||
            env->IsInstanceOf(provider, cache->chunkProviderClass) != JNI_TRUE) {
            clearException(env);
            env->PopLocalFrame(nullptr);
            if (stopRequested(500U)) break;
            continue;
        }
        jobject listing = env->GetObjectField(provider, cache->chunkListingField);
        jobjectArray chunks = listing == nullptr ? nullptr : static_cast<jobjectArray>(
            env->CallObjectMethod(listing, cache->listToArray));
        if (env->ExceptionCheck() == JNI_TRUE || chunks == nullptr) {
            clearException(env);
            env->PopLocalFrame(nullptr);
            if (stopRequested(500U)) break;
            continue;
        }

        currentChunks.clear();
        const jsize chunkCount = std::min<jsize>(env->GetArrayLength(chunks), 2048);
        for (jsize chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            jobject chunk = env->GetObjectArrayElement(chunks, chunkIndex);
            if (env->ExceptionCheck() == JNI_TRUE) {
                clearException(env);
                continue;
            }
            if (chunk == nullptr || env->IsInstanceOf(chunk, cache->chunkClass) != JNI_TRUE) {
                if (chunk != nullptr) env->DeleteLocalRef(chunk);
                continue;
            }
            const jint chunkX = env->GetIntField(chunk, cache->chunkX);
            const jint chunkZ = env->GetIntField(chunk, cache->chunkZ);
            if (env->ExceptionCheck() == JNI_TRUE) {
                clearException(env);
                env->DeleteLocalRef(chunk);
                continue;
            }
            const std::uint64_t key = chunkKey(chunkX, chunkZ);
            currentChunks.insert(key);
            if (!processedChunks.insert(key).second) {
                env->DeleteLocalRef(chunk);
                continue;
            }

            std::vector<BedMarker> discovered;
            std::vector<DefenseSample> defenseSamples;
            jobjectArray sections = static_cast<jobjectArray>(
                env->CallObjectMethod(chunk, cache->getStorageArrays));
            if (env->ExceptionCheck() == JNI_TRUE || sections == nullptr) {
                clearException(env);
                processedChunks.erase(key); // transient read: retry next cycle
                env->DeleteLocalRef(chunk);
                continue;
            }
            const jsize sectionCount = std::min<jsize>(env->GetArrayLength(sections), 16);
            std::array<jchar, 4096U> states{};
            bool sectionReadFailed = false;
            for (jsize sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
                jobject section = env->GetObjectArrayElement(sections, sectionIndex);
                if (env->ExceptionCheck() == JNI_TRUE) {
                    clearException(env);
                    sectionReadFailed = true;
                    continue;
                }
                if (section == nullptr) continue;
                jcharArray data = static_cast<jcharArray>(
                    env->CallObjectMethod(section, cache->getStorageData));
                if (env->ExceptionCheck() == JNI_TRUE || data == nullptr ||
                    env->GetArrayLength(data) != static_cast<jsize>(states.size())) {
                    clearException(env);
                    sectionReadFailed = true;
                    if (data != nullptr) env->DeleteLocalRef(data);
                    env->DeleteLocalRef(section);
                    continue;
                }
                // 1.8.9 stores the global block-state ID in char[4096]. A
                // single bulk copy replaces 4096 getBlockState JNI calls.
                env->GetCharArrayRegion(data, 0, static_cast<jsize>(states.size()), states.data());
                if (env->ExceptionCheck() == JNI_TRUE) {
                    clearException(env);
                    sectionReadFailed = true;
                    env->DeleteLocalRef(data);
                    env->DeleteLocalRef(section);
                    continue;
                }
                for (std::size_t index = 0U; index < states.size(); ++index) {
                    const unsigned encoded = static_cast<unsigned>(states[index]);
                    const unsigned blockId = encoded >> 4U;
                    // Bed Wars defenses use a small, stable material palette.
                    // Retaining only those sparse samples avoids a 128 KiB
                    // dense copy per loaded chunk while still showing the
                    // blocks players actually place around a bed.
                    const bool defenseMaterial = blockId == 1U || blockId == 4U ||
                        blockId == 5U || blockId == 17U || blockId == 20U ||
                        blockId == 24U || blockId == 35U || blockId == 45U ||
                        blockId == 49U || blockId == 95U || blockId == 121U ||
                        blockId == 159U;
                    if (defenseMaterial) {
                        const int localX = static_cast<int>(index & 15U);
                        const int localZ = static_cast<int>((index >> 4U) & 15U);
                        const int localY = static_cast<int>((index >> 8U) & 15U);
                        defenseSamples.push_back(DefenseSample{
                            chunkX * 16 + localX,
                            static_cast<int>(sectionIndex) * 16 + localY,
                            chunkZ * 16 + localZ,
                            static_cast<std::uint16_t>(blockId),
                            static_cast<std::uint8_t>(encoded & 0xFU)});
                    }
                    if ((encoded >> 4U) != 26U || (encoded & 0x8U) == 0U) continue;
                    const int localX = static_cast<int>(index & 15U);
                    const int localZ = static_cast<int>((index >> 4U) & 15U);
                    const int localY = static_cast<int>((index >> 8U) & 15U);
                    const int headX = chunkX * 16 + localX;
                    const int headZ = chunkZ * 16 + localZ;
                    int footX = headX;
                    int footZ = headZ;
                    // BlockBed stores horizontal facing in metadata bits 0-1.
                    // getHorizontal maps 0=SOUTH, 1=WEST, 2=NORTH, 3=EAST;
                    // the scanned HEAD is one block along that facing from
                    // the FOOT, so walk in the opposite direction here.
                    switch (encoded & 0x3U) {
                    case 0U: --footZ; break;
                    case 1U: ++footX; break;
                    case 2U: ++footZ; break;
                    case 3U: --footX; break;
                    default: break;
                    }
                    discovered.push_back(BedMarker{
                        headX,
                        static_cast<int>(sectionIndex) * 16 + localY,
                        headZ,
                        footX,
                        footZ});
                }
                env->DeleteLocalRef(data);
                env->DeleteLocalRef(section);
            }
            bedsByChunk[key] = std::move(discovered);
            defenseByChunk[key] = std::move(defenseSamples);
            if (sectionReadFailed) {
                // Concurrent chunk mutation can invalidate one local section
                // read. Publish any safe partial result now, but remove the
                // processed marker so the complete chunk is retried in 500 ms.
                processedChunks.erase(key);
            }
            env->DeleteLocalRef(sections);
            env->DeleteLocalRef(chunk);
        }

        for (auto it = processedChunks.begin(); it != processedChunks.end();) {
            if (!currentChunks.contains(*it)) {
                bedsByChunk.erase(*it);
                defenseByChunk.erase(*it);
                it = processedChunks.erase(it);
            } else {
                ++it;
            }
        }

        const std::uint64_t now = static_cast<std::uint64_t>(::GetTickCount64());
        if (now - lastVerification >= 100U) {
            lastVerification = now;
            for (auto& [key, markers] : bedsByChunk) {
                (void)key;
                for (auto marker = markers.begin(); marker != markers.end();) {
                    jobject position = env->NewObject(cache->blockPosClass,
                                                      cache->blockPosConstructor,
                                                      marker->x, marker->y, marker->z);
                    jobject state = position == nullptr ? nullptr :
                        env->CallObjectMethod(world, cache->getBlockState, position);
                    jobject block = state == nullptr ? nullptr :
                        env->CallObjectMethod(state, cache->getBlock);
                    const bool failed = env->ExceptionCheck() == JNI_TRUE;
                    if (failed) clearException(env);
                    const bool stillBed = !failed && block != nullptr &&
                        env->IsInstanceOf(block, cache->bedClass) == JNI_TRUE;
                    if (block != nullptr) env->DeleteLocalRef(block);
                    if (state != nullptr) env->DeleteLocalRef(state);
                    if (position != nullptr) env->DeleteLocalRef(position);
                    if (!failed && !stillBed) marker = markers.erase(marker);
                    else ++marker;
                }
            }
        }

        if (now - lastDefenseRefresh >= 750U) {
            lastDefenseRefresh = now;
            const auto floorChunk = [](const int coordinate) noexcept {
                return coordinate >= 0 ? coordinate / 16 : (coordinate - 15) / 16;
            };
            // Chunk diffing remains the discovery mechanism for arbitrary
            // world data. Only the bounded 3x3 neighbourhood around an
            // already-known bed is invalidated here, so placed/broken defense
            // blocks refresh automatically without rescanning every loaded
            // chunk or issuing per-block JNI calls.
            for (const auto& [bedChunk, markers] : bedsByChunk) {
                (void)bedChunk;
                for (const BedMarker& marker : markers) {
                    const int centerChunkX = floorChunk(marker.x);
                    const int centerChunkZ = floorChunk(marker.z);
                    for (int dz = -1; dz <= 1; ++dz) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            processedChunks.erase(chunkKey(
                                static_cast<jint>(centerChunkX + dx),
                                static_cast<jint>(centerChunkZ + dz)));
                        }
                    }
                }
            }
        }

        publish();
        env->PopLocalFrame(nullptr);
        if (stopRequested(100U)) break;
    }

    if (worldIdentity != nullptr) env->DeleteGlobalRef(worldIdentity);
    clearException(env);
}

void GameBindings::requestBedRescan() noexcept
{
    m_bedRescanRequested.store(true, std::memory_order_release);
    if (m_bedRescanEvent != nullptr) {
        ::SetEvent(m_bedRescanEvent);
    }
}

const GameSnapshot& GameBindings::sample(JNIEnv* const env,
                                         const std::uint64_t tickMilliseconds) noexcept
{
    // Minecraft simulation runs at 20 TPS. Sampling once per tick keeps the
    // JNI budget bounded while the renderer uses last/current positions plus
    // renderPartialTicks to produce frame-rate-smooth hitboxes.
    constexpr std::uint64_t kSampleIntervalMs = 50U;
    if (env == nullptr ||
        m_resolutionPhase.load(std::memory_order_acquire) != ResolutionPhase::Resolved) {
        return m_snapshot;
    }
    BindingCache* const cache = m_cache.get();
    if (cache == nullptr || cache->profile == nullptr) {
        m_snapshot.state = GameSnapshot::State::JniError;
        return m_snapshot;
    }

    if (tickMilliseconds - m_lastSample < kSampleIntervalMs && m_lastSample != 0U) {
        return m_snapshot;
    }
    m_lastSample = tickMilliseconds;

    if (env->PushLocalFrame(256) < 0) {
        clearException(env);
        m_snapshot.state = GameSnapshot::State::JniError;
        return m_snapshot;
    }

    auto failJni = [&]() noexcept -> const GameSnapshot& {
        clearException(env);
        env->PopLocalFrame(nullptr);
        m_snapshot.state = GameSnapshot::State::JniError;
        return m_snapshot;
    };

    jobject minecraft = cache->minecraftInstanceField != nullptr
        ? env->GetStaticObjectField(cache->minecraftClass, cache->minecraftInstanceField)
        : env->CallStaticObjectMethod(cache->minecraftClass, cache->getMinecraft);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    if (minecraft == nullptr) {
        env->PopLocalFrame(nullptr);
        m_snapshot.state = GameSnapshot::State::WaitingForGameThread;
        return m_snapshot;
    }
    const jboolean onMainThread = env->CallBooleanMethod(minecraft, cache->isMainThread);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    if (onMainThread != JNI_TRUE) {
        env->PopLocalFrame(nullptr);
        // SwapBuffers can also belong to Forge's splash renderer. Never read
        // world state from that thread/context.
        m_snapshot.state = GameSnapshot::State::WaitingForGameThread;
        return m_snapshot;
    }

    m_snapshot.hypixelServer = false;
    if (cache->getCurrentServerData != nullptr && cache->serverIp != nullptr) {
        jobject serverData = env->CallObjectMethod(
            minecraft, cache->getCurrentServerData);
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
        } else if (serverData != nullptr) {
            jstring address = static_cast<jstring>(
                env->GetObjectField(serverData, cache->serverIp));
            if (env->ExceptionCheck() == JNI_TRUE) {
                env->ExceptionClear();
            } else if (address != nullptr) {
                const char* utf = env->GetStringUTFChars(address, nullptr);
                if (utf != nullptr) {
                    std::string host(utf);
                    env->ReleaseStringUTFChars(address, utf);
                    std::transform(host.begin(), host.end(), host.begin(),
                        [](const unsigned char value) noexcept {
                            return static_cast<char>(std::tolower(value));
                        });
                    const std::size_t colon = host.find(':');
                    if (colon != std::string::npos) host.resize(colon);
                    m_snapshot.hypixelServer = host == "hypixel.net" ||
                        (host.size() > 12U && host.ends_with(".hypixel.net"));
                }
            }
        }
    }

    jobject player = env->GetObjectField(minecraft, cache->playerField);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    jobject world = env->GetObjectField(minecraft, cache->worldField);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    if (player == nullptr || world == nullptr) {
        if (m_lastWorld != nullptr) {
            env->DeleteWeakGlobalRef(m_lastWorld);
            m_lastWorld = nullptr;
        }
        m_sidebarCandidateTeam = bedwars::Team::Unknown;
        m_sidebarStableCount = 0U;
        m_sidebarMissingCount = 0U;
        if (!(m_matchProbe == MatchProbeState{})) {
            m_matchProbe = {};
            ++m_matchProbeGeneration;
        }
        m_matchAnchorValid = false;
        m_lockedOwnBedKnown = false;
        m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
        if (m_snapshot.matchActive || m_snapshot.playerCount != 0U ||
            m_snapshot.ownTeam != 'u' ||
            m_snapshot.localPlayerName[0U] != '\0') {
            m_snapshot.matchActive = false;
            m_snapshot.playerCount = 0U;
            m_snapshot.players = {};
            m_snapshot.localPlayerName = {};
            m_snapshot.ownTeam = 'u';
            m_snapshot.ownBedKnown = false;
            m_snapshot.ownBedSource = GameSnapshot::OwnBedSource::Unknown;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
        }
        env->PopLocalFrame(nullptr);
        m_snapshot.state = GameSnapshot::State::NoPlayer;
        return m_snapshot;
    }

    // A WorldClient identity change is the hard lifecycle boundary for every
    // match-derived value. No translated server text or /rejoin command needs
    // to be recognized, and stale team/bed state cannot leak into the next map.
    if (m_lastWorld == nullptr || env->IsSameObject(m_lastWorld, world) != JNI_TRUE) {
        if (m_lastWorld != nullptr) env->DeleteWeakGlobalRef(m_lastWorld);
        m_lastWorld = env->NewWeakGlobalRef(world);
        m_sidebarCandidateTeam = bedwars::Team::Unknown;
        m_sidebarStableCount = 0U;
        m_sidebarMissingCount = 0U;
        m_matchProbe = {};
        ++m_matchProbeGeneration;
        m_matchAnchorValid = false;
        m_lockedOwnBedKnown = false;
        m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
        m_snapshot.matchActive = false;
        m_snapshot.ownTeam = 'u';
        m_snapshot.ownBedKnown = false;
        m_snapshot.ownBedSource = GameSnapshot::OwnBedSource::Unknown;
        m_snapshot.players = {};
        m_snapshot.playerCount = 0U;
        m_debugRosterGeneration = 0U;
        m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
    }

    const jfloat health = env->CallFloatMethod(player, cache->getHealth);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    const jfloat maxHealth = env->CallFloatMethod(player, cache->getMaxHealth);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    const jint entityId = env->CallIntMethod(player, cache->getEntityId);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    const jdouble positionX = env->GetDoubleField(player, cache->positionX);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    const jdouble positionY = env->GetDoubleField(player, cache->positionY);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    const jdouble positionZ = env->GetDoubleField(player, cache->positionZ);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    jobject bounds = env->CallObjectMethod(player, cache->getBounds);
    if (env->ExceptionCheck() == JNI_TRUE || bounds == nullptr) return failJni();

    AxisAlignedBox box;
    box.minX = env->GetDoubleField(bounds, cache->minX);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    box.minY = env->GetDoubleField(bounds, cache->minY);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    box.minZ = env->GetDoubleField(bounds, cache->minZ);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    box.maxX = env->GetDoubleField(bounds, cache->maxX);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    box.maxY = env->GetDoubleField(bounds, cache->maxY);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    box.maxZ = env->GetDoubleField(bounds, cache->maxZ);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();

    jobject loadedEntities = cache->loadedEntitiesField != nullptr
        ? env->GetObjectField(world, cache->loadedEntitiesField)
        : env->CallObjectMethod(world, cache->getLoadedEntities);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    const jint loadedEntityCount = loadedEntities == nullptr
        ? 0 : env->CallIntMethod(loadedEntities, cache->listSize);
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    jboolean integratedSinglePlayer = env->CallBooleanMethod(
        minecraft, cache->isSingleplayer);
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        integratedSinglePlayer = JNI_FALSE;
    }
    const jboolean singlePlayer = JNI_TRUE;
    if (!std::isfinite(health) || !std::isfinite(maxHealth)) return failJni();

    m_snapshot.singlePlayer = singlePlayer == JNI_TRUE;
    m_snapshot.integratedSinglePlayer = integratedSinglePlayer == JNI_TRUE;

    auto readEntityMarker = [&](jobject const entity, EntityMarker& marker,
                                const bool livingEntity) noexcept {
        marker = {};
        marker.entityId = env->CallIntMethod(entity, cache->getEntityId);
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            return false;
        }
        if (livingEntity) {
            marker.health = env->CallFloatMethod(entity, cache->getHealth);
            marker.maxHealth = env->CallFloatMethod(entity, cache->getMaxHealth);
            if (env->ExceptionCheck() == JNI_TRUE) {
                env->ExceptionClear();
                marker.health = 0.0F;
                marker.maxHealth = 0.0F;
            }
        }
        if (cache->isInvisible != nullptr) {
            marker.invisible = env->CallBooleanMethod(entity, cache->isInvisible) == JNI_TRUE;
            if (env->ExceptionCheck() == JNI_TRUE) {
                env->ExceptionClear();
                marker.invisible = false;
            }
        }
        jobject entityBounds = env->CallObjectMethod(entity, cache->getBounds);
        if (env->ExceptionCheck() == JNI_TRUE || entityBounds == nullptr) {
            env->ExceptionClear();
            return false;
        }
        marker.bounds.minX = env->GetDoubleField(entityBounds, cache->minX);
        marker.bounds.minY = env->GetDoubleField(entityBounds, cache->minY);
        marker.bounds.minZ = env->GetDoubleField(entityBounds, cache->minZ);
        marker.bounds.maxX = env->GetDoubleField(entityBounds, cache->maxX);
        marker.bounds.maxY = env->GetDoubleField(entityBounds, cache->maxY);
        marker.bounds.maxZ = env->GetDoubleField(entityBounds, cache->maxZ);
        marker.currentX = env->GetDoubleField(entity, cache->positionX);
        marker.currentY = env->GetDoubleField(entity, cache->positionY);
        marker.currentZ = env->GetDoubleField(entity, cache->positionZ);
        marker.previousX = env->GetDoubleField(entity, cache->previousPosition[0U]);
        marker.previousY = env->GetDoubleField(entity, cache->previousPosition[1U]);
        marker.previousZ = env->GetDoubleField(entity, cache->previousPosition[2U]);
        if (cache->motionFields[0U] != nullptr &&
            cache->motionFields[1U] != nullptr &&
            cache->motionFields[2U] != nullptr) {
            marker.motionX = env->GetDoubleField(entity, cache->motionFields[0U]);
            marker.motionY = env->GetDoubleField(entity, cache->motionFields[1U]);
            marker.motionZ = env->GetDoubleField(entity, cache->motionFields[2U]);
        }
        const bool failed = env->ExceptionCheck() == JNI_TRUE;
        if (failed) env->ExceptionClear();
        env->DeleteLocalRef(entityBounds);
        if (failed) return false;
        const double centerEntityX = (marker.bounds.minX + marker.bounds.maxX) * 0.5;
        const double centerEntityY = (marker.bounds.minY + marker.bounds.maxY) * 0.5;
        const double centerEntityZ = (marker.bounds.minZ + marker.bounds.maxZ) * 0.5;
        const double deltaX = centerEntityX - positionX;
        const double deltaY = centerEntityY - positionY;
        const double deltaZ = centerEntityZ - positionZ;
        marker.distance = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
        return std::isfinite(marker.distance) &&
               std::isfinite(marker.currentX) && std::isfinite(marker.currentY) &&
               std::isfinite(marker.currentZ) && std::isfinite(marker.previousX) &&
               std::isfinite(marker.previousY) && std::isfinite(marker.previousZ) &&
               std::isfinite(marker.motionX) && std::isfinite(marker.motionY) &&
               std::isfinite(marker.motionZ) &&
               std::isfinite(marker.bounds.minX) && std::isfinite(marker.bounds.minY) &&
               std::isfinite(marker.bounds.minZ) && std::isfinite(marker.bounds.maxX) &&
               std::isfinite(marker.bounds.maxY) && std::isfinite(marker.bounds.maxZ);
    };

    // Read the actual dyed leather chestplate data, never the rendered/glint
    // colour. The same routine is used for the local player (match fallback)
    // and remote players (teammate/threat classification), which prevents the
    // two paths from drifting apart.
    auto readPlayerArmorTeam = [&](jobject const playerObject,
                                   bool& chestplatePresent,
                                   std::uint8_t& protectionLevel) noexcept {
        chestplatePresent = false;
        protectionLevel = 0U;
        if (playerObject == nullptr || cache->getEquipmentInSlot == nullptr ||
            cache->getItem == nullptr ||
            cache->itemArmorClass == nullptr || cache->hasColor == nullptr ||
            cache->getColor == nullptr) {
            return bedwars::Team::Unknown;
        }

        bedwars::Team result = bedwars::Team::Unknown;
        // Remote inventories are private server state. S04PacketEntityEquipment
        // is instead applied to EntityLivingBase's public equipment slots, so
        // getEquipmentInSlot is the authoritative path for other players.
        // Slot layout in 1.8.9 is 0=held, 1=boots, 2=leggings,
        // 3=chestplate, 4=helmet.
        jobject chestplate = env->CallObjectMethod(
            playerObject, cache->getEquipmentInSlot, 3);
        if (env->ExceptionCheck() == JNI_TRUE) {
            clearException(env);
            chestplate = nullptr;
        }
        if (chestplate != nullptr) {
            chestplatePresent = true;
            jobject item = env->CallObjectMethod(chestplate, cache->getItem);
            if (env->ExceptionCheck() != JNI_TRUE && item != nullptr &&
                env->IsInstanceOf(item, cache->itemArmorClass) == JNI_TRUE) {
                const jboolean coloured = env->CallBooleanMethod(
                    item, cache->hasColor, chestplate);
                if (env->ExceptionCheck() != JNI_TRUE && coloured == JNI_TRUE) {
                    const jint rgb = env->CallIntMethod(item, cache->getColor, chestplate);
                    if (env->ExceptionCheck() != JNI_TRUE) {
                        result = bedwars::fromLeatherRgb(
                            static_cast<std::uint32_t>(rgb));
                    }
                }
            }
            clearException(env);
            if (item != nullptr) env->DeleteLocalRef(item);
        }

        if (cache->enchantmentHelperClass != nullptr &&
            cache->getEnchantmentLevel != nullptr) {
            // Protection can be carried by any visible armour piece. Query all
            // four packet-backed slots and keep the strongest level instead of
            // assuming that a transformed client mirrors the chestplate into
            // InventoryPlayer. Protection's legacy enchantment ID is 0.
            for (jint slot = 1; slot <= 4; ++slot) {
                jobject armorStack = env->CallObjectMethod(
                    playerObject, cache->getEquipmentInSlot, slot);
                if (env->ExceptionCheck() == JNI_TRUE) {
                    clearException(env);
                    armorStack = nullptr;
                }
                if (armorStack != nullptr) {
                    const jint level = env->CallStaticIntMethod(
                        cache->enchantmentHelperClass,
                        cache->getEnchantmentLevel, 0, armorStack);
                    if (env->ExceptionCheck() != JNI_TRUE) {
                        protectionLevel = std::max(
                            protectionLevel,
                            static_cast<std::uint8_t>(std::clamp(level, 0, 10)));
                    }
                    clearException(env);
                    env->DeleteLocalRef(armorStack);
                }
            }
        }
        if (chestplate != nullptr) env->DeleteLocalRef(chestplate);
        clearException(env);
        return result;
    };

    auto copyUuidString = [&](jobject const uuidObject,
                              std::array<char, 37U>& destination) noexcept {
        if (uuidObject == nullptr || cache->uuidToString == nullptr) return false;
        jstring text = static_cast<jstring>(
            env->CallObjectMethod(uuidObject, cache->uuidToString));
        if (env->ExceptionCheck() == JNI_TRUE || text == nullptr) {
            clearException(env);
            if (text != nullptr) env->DeleteLocalRef(text);
            return false;
        }
        bool copied = false;
        const char* const utf8 = env->GetStringUTFChars(text, nullptr);
        if (env->ExceptionCheck() != JNI_TRUE && utf8 != nullptr) {
            const std::string_view value(utf8);
            if (value.size() == 36U) {
                std::copy(value.begin(), value.end(), destination.begin());
                copied = true;
            }
        }
        if (utf8 != nullptr) env->ReleaseStringUTFChars(text, utf8);
        clearException(env);
        env->DeleteLocalRef(text);
        return copied;
    };

    jobject textureManagerObject = nullptr;
    if (cache->getTextureManager != nullptr) {
        textureManagerObject = env->CallObjectMethod(
            minecraft, cache->getTextureManager);
        if (env->ExceptionCheck() == JNI_TRUE) {
            clearException(env);
            textureManagerObject = nullptr;
        }
    }

    auto readSkinTextureId = [&](jobject const playerObject) noexcept -> std::uint32_t {
        if (playerObject == nullptr || textureManagerObject == nullptr ||
            cache->abstractClientPlayerClass == nullptr ||
            cache->getLocationSkin == nullptr || cache->getTexture == nullptr ||
            cache->getGlTextureId == nullptr ||
            env->IsInstanceOf(playerObject, cache->abstractClientPlayerClass) != JNI_TRUE) {
            clearException(env);
            return 0U;
        }
        jobject location = env->CallObjectMethod(playerObject, cache->getLocationSkin);
        jobject texture = nullptr;
        if (env->ExceptionCheck() != JNI_TRUE && location != nullptr) {
            texture = env->CallObjectMethod(textureManagerObject,
                                             cache->getTexture, location);
        }
        jint textureId = 0;
        if (env->ExceptionCheck() != JNI_TRUE && texture != nullptr) {
            textureId = env->CallIntMethod(texture, cache->getGlTextureId);
        }
        if (env->ExceptionCheck() == JNI_TRUE) {
            clearException(env);
            textureId = 0;
        }
        if (texture != nullptr) env->DeleteLocalRef(texture);
        if (location != nullptr) env->DeleteLocalRef(location);
        return textureId > 0 ? static_cast<std::uint32_t>(textureId) : 0U;
    };

    // ESP collection is deliberately unavailable on remote multiplayer worlds.
    // A single List.toArray() avoids one virtual JNI call per list index. The
    // fixed 128-marker cap and 20 Hz cadence are both deterministic; smooth
    // motion is reconstructed in OverlayRenderer from previous/current tick
    // coordinates and the per-frame Timer.renderPartialTicks value.
    if (singlePlayer == JNI_TRUE && loadedEntities != nullptr && loadedEntityCount > 0) {
        jobject playerList = env->GetObjectField(world, cache->playerEntities);
        jobjectArray playerObjects = playerList == nullptr ? nullptr :
            static_cast<jobjectArray>(env->CallObjectMethod(playerList, cache->listToArray));
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            playerObjects = nullptr;
        }
        jobjectArray entities = static_cast<jobjectArray>(
            env->CallObjectMethod(loadedEntities, cache->listToArray));
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            entities = nullptr;
        }
        m_snapshot.entityMarkerCount = 0U;
        if (entities != nullptr) {
            const jsize entityLimit = std::min<jsize>(env->GetArrayLength(entities), 512);
            for (jsize index = 0; index < entityLimit &&
                 m_snapshot.entityMarkerCount < GameSnapshot::MaxEntityMarkers; ++index) {
                jobject entity = env->GetObjectArrayElement(entities, index);
                if (env->ExceptionCheck() == JNI_TRUE) {
                    env->ExceptionClear();
                    continue;
                }
                if (entity == nullptr) continue;
                const bool isLocalPlayer = env->IsSameObject(entity, player) == JNI_TRUE;
                const bool isLiving = env->IsInstanceOf(entity, cache->livingClass) == JNI_TRUE;
                const bool isFireball = cache->fireballClass != nullptr &&
                    env->IsInstanceOf(entity, cache->fireballClass) == JNI_TRUE;
                if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
                if (!isLocalPlayer && (isLiving || isFireball)) {
                    EntityMarker marker;
                    if (readEntityMarker(entity, marker, isLiving)) {
                        marker.fireball = isFireball;
                        if (playerObjects != nullptr) {
                            const jsize playerLimit = std::min<jsize>(
                                env->GetArrayLength(playerObjects), 64);
                            for (jsize playerIndex = 0; playerIndex < playerLimit;
                                 ++playerIndex) {
                                jobject candidate = env->GetObjectArrayElement(
                                    playerObjects, playerIndex);
                                if (candidate != nullptr) {
                                    marker.player = env->IsSameObject(entity, candidate) == JNI_TRUE;
                                    env->DeleteLocalRef(candidate);
                                }
                                if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
                                if (marker.player) break;
                            }
                        }

                        if (marker.player) {
                            jstring markerName = static_cast<jstring>(
                                env->CallObjectMethod(entity, cache->getName));
                            if (env->ExceptionCheck() != JNI_TRUE && markerName != nullptr) {
                                const char* const utf8 = env->GetStringUTFChars(markerName, nullptr);
                                if (env->ExceptionCheck() != JNI_TRUE && utf8 != nullptr) {
                                    const std::string_view nameView(utf8);
                                    if (!nameView.empty() && nameView.size() <= 16U) {
                                        std::copy(nameView.begin(), nameView.end(),
                                                  marker.playerName.begin());
                                    }
                                }
                                if (utf8 != nullptr) env->ReleaseStringUTFChars(markerName, utf8);
                            }
                            clearException(env);
                            if (markerName != nullptr) env->DeleteLocalRef(markerName);

                            if (cache->getUniqueId != nullptr) {
                                jobject uuidObject = env->CallObjectMethod(
                                    entity, cache->getUniqueId);
                                if (env->ExceptionCheck() != JNI_TRUE && uuidObject != nullptr) {
                                    (void)copyUuidString(uuidObject, marker.uuid);
                                }
                                clearException(env);
                                if (uuidObject != nullptr) env->DeleteLocalRef(uuidObject);
                            }
                            marker.skinTextureId = readSkinTextureId(entity);
                        }

                        if (marker.player) {
                            const bedwars::Team armorTeam =
                                readPlayerArmorTeam(entity, marker.hasArmor,
                                                    marker.protectionLevel);
                            marker.armorTeam = bedwars::formatCode(armorTeam);
                            if (cache->getEquipmentInSlot != nullptr &&
                                cache->getIdFromItem != nullptr &&
                                cache->stackSize != nullptr &&
                                cache->getItemDamage != nullptr) {
                                jobject heldStack = env->CallObjectMethod(
                                    entity, cache->getEquipmentInSlot, 0);
                                if (env->ExceptionCheck() != JNI_TRUE && heldStack != nullptr) {
                                    jobject heldItem = env->CallObjectMethod(
                                        heldStack, cache->getItem);
                                    if (env->ExceptionCheck() != JNI_TRUE && heldItem != nullptr) {
                                        const jint itemId = env->CallStaticIntMethod(
                                            cache->itemClass, cache->getIdFromItem, heldItem);
                                        const jint count = env->GetIntField(
                                            heldStack, cache->stackSize);
                                        const jint damage = env->CallIntMethod(
                                            heldStack, cache->getItemDamage);
                                        if (env->ExceptionCheck() != JNI_TRUE) {
                                            marker.heldItemId = static_cast<std::int16_t>(
                                                std::clamp(itemId, -1, 32767));
                                            marker.heldItemCount = static_cast<std::uint8_t>(
                                                std::clamp(count, 0, 255));
                                            marker.heldItemDamage = static_cast<std::uint16_t>(
                                                std::clamp(damage, 0, 65535));
                                        }
                                    }
                                    clearException(env);
                                    if (heldItem != nullptr) env->DeleteLocalRef(heldItem);
                                    env->DeleteLocalRef(heldStack);
                                } else {
                                    clearException(env);
                                }
                            }
                        }

                        marker.teamColor = marker.armorTeam;
                        if (marker.playerName[0U] != '\0' || marker.uuid[0U] != '\0') {
                            for (std::uint32_t rosterIndex = 0U;
                                 rosterIndex < m_snapshot.playerCount; ++rosterIndex) {
                                const PlayerIdentity& identity = m_snapshot.players[rosterIndex];
                                const bool uuidMatch = marker.uuid[0U] != '\0' &&
                                    identity.uuid[0U] != '\0' &&
                                    std::strcmp(identity.uuid.data(), marker.uuid.data()) == 0;
                                const bool nameMatch = marker.playerName[0U] != '\0' &&
                                    std::strcmp(identity.name.data(),
                                                marker.playerName.data()) == 0;
                                if (uuidMatch || nameMatch) {
                                    marker.teamColor = identity.teamColor;
                                    marker.confirmedPlayer = true;
                                    // TAB owns the player-facing nickname.
                                    // Use it for alerts/nametags after the UUID
                                    // join instead of exposing Hypixel's entity
                                    // alias when the two strings differ.
                                    std::copy(identity.name.begin(), identity.name.end(),
                                              marker.playerName.begin());
                                    break;
                                }
                            }
                        }
                        
                        m_snapshot.entityMarkers[m_snapshot.entityMarkerCount++] = marker;
                    }
                }
                env->DeleteLocalRef(entity);
            }
            env->DeleteLocalRef(entities);
        }
        if (playerObjects != nullptr) env->DeleteLocalRef(playerObjects);
        if (playerList != nullptr) env->DeleteLocalRef(playerList);
    } else {
        m_snapshot.entityMarkerCount = 0U;
    }

    // Trajectory sampling deliberately stays on Minecraft's 20 TPS thread.
    // Rendering consumes only immutable POD arrays and therefore performs no
    // JNI work at monitor refresh rate.
    m_snapshot.knockbackTrajectoryCount = 0U;
    const bool trajectoryBlocksAvailable = cache->isAirBlock != nullptr &&
        cache->blockPosClass != nullptr && cache->blockPosConstructor != nullptr;
    const auto blockIsAir = [&](const double x, const double y,
                                const double z, bool& air) noexcept {
        if (!trajectoryBlocksAvailable) return false;
        jobject position = env->NewObject(cache->blockPosClass,
            cache->blockPosConstructor,
            static_cast<jint>(std::floor(x)),
            static_cast<jint>(std::floor(y)),
            static_cast<jint>(std::floor(z)));
        if (position == nullptr || env->ExceptionCheck() == JNI_TRUE) {
            clearException(env);
            return false;
        }
        air = env->CallBooleanMethod(world, cache->isAirBlock, position) == JNI_TRUE;
        const bool valid = env->ExceptionCheck() != JNI_TRUE;
        clearException(env);
        env->DeleteLocalRef(position);
        return valid;
    };

    if (trajectoryBlocksAvailable) {
        for (std::uint32_t markerIndex = 0U;
             markerIndex < m_snapshot.entityMarkerCount &&
             m_snapshot.knockbackTrajectoryCount <
                 GameSnapshot::MaxKnockbackTrajectories; ++markerIndex) {
            const EntityMarker& marker = m_snapshot.entityMarkers[markerIndex];
            const double horizontalImpulse = std::hypot(marker.motionX,
                                                        marker.motionZ);
            // Ordinary walking never has this upward impulse. Requiring both
            // components prevents a trajectory from appearing for every
            // moving entity and makes one prediction correspond to one hit.
            if (!marker.player || marker.motionY < 0.075 ||
                horizontalImpulse < 0.055) continue;
            KnockbackTrajectory prediction;
            prediction.entityId = marker.entityId;
            prediction.startBounds = marker.bounds;
            double simulatedX = marker.currentX;
            double simulatedY = marker.bounds.minY;
            double simulatedZ = marker.currentZ;
            double velocityX = marker.motionX;
            double velocityY = marker.motionY;
            double velocityZ = marker.motionZ;
            prediction.points[prediction.pointCount++] = {
                simulatedX, simulatedY, simulatedZ};
            for (std::size_t step = 1U;
                 step < prediction.points.size(); ++step) {
                simulatedX += velocityX;
                simulatedY += velocityY;
                simulatedZ += velocityZ;
                prediction.points[prediction.pointCount++] = {
                    simulatedX, simulatedY, simulatedZ};

                if (velocityY <= 0.0) {
                    bool airBelow = true;
                    const double probeY = simulatedY - 0.06;
                    if (blockIsAir(simulatedX, probeY, simulatedZ, airBelow) &&
                        !airBelow) {
                        const double blockTop = std::floor(probeY) + 1.0;
                        if (simulatedY <= blockTop + 0.16) {
                            prediction.points[prediction.pointCount - 1U].y = blockTop;
                            prediction.landed = true;
                            break;
                        }
                    }
                }
                // 1.8 living-entity airborne approximation. The visual is a
                // client prediction; server corrections naturally replace it
                // on the next immutable entity snapshot.
                velocityX *= 0.91;
                velocityZ *= 0.91;
                velocityY = (velocityY - 0.08) * 0.98;
            }
            m_snapshot.knockbackTrajectories[
                m_snapshot.knockbackTrajectoryCount++] = prediction;
        }
    }

    // Bow draw strength follows ItemBow 1.8.9: t/20, transformed by
    // (t^2 + 2t) / 3 and capped at one. We track the focused right-button hold
    // because it avoids another fragile transformed-client method mapping.
    int localHeldItemId = -1;
    if (cache->getEquipmentInSlot != nullptr && cache->getItem != nullptr &&
        cache->getIdFromItem != nullptr && cache->itemClass != nullptr) {
        jobject heldStack = env->CallObjectMethod(player,
            cache->getEquipmentInSlot, 0);
        if (env->ExceptionCheck() != JNI_TRUE && heldStack != nullptr) {
            jobject heldItem = env->CallObjectMethod(heldStack, cache->getItem);
            if (env->ExceptionCheck() != JNI_TRUE && heldItem != nullptr) {
                localHeldItemId = env->CallStaticIntMethod(cache->itemClass,
                    cache->getIdFromItem, heldItem);
            }
            clearException(env);
            if (heldItem != nullptr) env->DeleteLocalRef(heldItem);
            env->DeleteLocalRef(heldStack);
        } else {
            clearException(env);
        }
    }
    DWORD foregroundProcess = 0U;
    const HWND foregroundWindow = ::GetForegroundWindow();
    if (foregroundWindow != nullptr)
        (void)::GetWindowThreadProcessId(foregroundWindow, &foregroundProcess);
    const bool bowButtonDown = foregroundProcess == ::GetCurrentProcessId() &&
        (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (localHeldItemId == 261 && bowButtonDown) {
        if (m_bowDrawStartedAt == 0U) m_bowDrawStartedAt = tickMilliseconds;
    } else {
        m_bowDrawStartedAt = 0U;
        m_snapshot.bowTrajectory = {};
    }
    if (m_bowDrawStartedAt != 0U && trajectoryBlocksAvailable &&
        (m_lastBowTrajectoryAt == 0U ||
         tickMilliseconds - m_lastBowTrajectoryAt >= 75U)) {
        m_lastBowTrajectoryAt = tickMilliseconds;
        BowTrajectory trajectory;
        const double useTicks = static_cast<double>(
            tickMilliseconds - m_bowDrawStartedAt) / 50.0;
        double draw = useTicks / 20.0;
        draw = std::clamp((draw * draw + draw * 2.0) / 3.0, 0.0, 1.0);
        if (draw >= 0.10) {
            constexpr double trajectoryPi = 3.14159265358979323846;
            const double playerYaw = static_cast<double>(
                env->GetFloatField(player, cache->rotationYaw)) *
                trajectoryPi / 180.0;
            const double playerPitch = static_cast<double>(
                env->GetFloatField(player, cache->rotationPitch)) *
                trajectoryPi / 180.0;
            if (env->ExceptionCheck() == JNI_TRUE) return failJni();
            double arrowX = positionX - std::cos(playerYaw) * 0.16;
            double arrowY = positionY + 1.52;
            double arrowZ = positionZ - std::sin(playerYaw) * 0.16;
            const double speed = draw * 3.0;
            double velocityX = -std::sin(playerYaw) * std::cos(playerPitch) * speed;
            double velocityY = -std::sin(playerPitch) * speed;
            double velocityZ =  std::cos(playerYaw) * std::cos(playerPitch) * speed;
            trajectory.active = true;
            trajectory.points[trajectory.pointCount++] = {arrowX, arrowY, arrowZ};
            for (std::size_t tick = 0U;
                 tick + 1U < trajectory.points.size() && !trajectory.hasImpact; ++tick) {
                const double nextX = arrowX + velocityX;
                const double nextY = arrowY + velocityY;
                const double nextZ = arrowZ + velocityZ;
                // Exact segment-vs-expanded-AABB test avoids point-sampling
                // misses when a fully charged arrow crosses several blocks in
                // one tick.
                double entityHitT = std::numeric_limits<double>::infinity();
                jint entityHitId = -1;
                for (std::uint32_t markerIndex = 0U;
                     markerIndex < m_snapshot.entityMarkerCount; ++markerIndex) {
                    const EntityMarker& marker =
                        m_snapshot.entityMarkers[markerIndex];
                    if (!marker.player) continue;
                    constexpr double padding = 0.30;
                    double entry = 0.0;
                    double exit = 1.0;
                    const auto clipAxis = [&](const double origin,
                                              const double direction,
                                              const double minimum,
                                              const double maximum) noexcept {
                        if (std::abs(direction) < 1.0e-9)
                            return origin >= minimum && origin <= maximum;
                        double first = (minimum - origin) / direction;
                        double second = (maximum - origin) / direction;
                        if (first > second) std::swap(first, second);
                        entry = std::max(entry, first);
                        exit = std::min(exit, second);
                        return entry <= exit;
                    };
                    if (clipAxis(arrowX, velocityX,
                                 marker.bounds.minX - padding,
                                 marker.bounds.maxX + padding) &&
                        clipAxis(arrowY, velocityY,
                                 marker.bounds.minY - padding,
                                 marker.bounds.maxY + padding) &&
                        clipAxis(arrowZ, velocityZ,
                                 marker.bounds.minZ - padding,
                                 marker.bounds.maxZ + padding) &&
                        entry >= 0.0 && entry <= 1.0 && entry < entityHitT) {
                        entityHitT = entry;
                        entityHitId = marker.entityId;
                    }
                }

                // Traverse only the voxels actually crossed by this segment.
                // This is both exact at block boundaries and an order of
                // magnitude cheaper than issuing JNI isAirBlock calls every
                // fraction of a block.
                int voxelX = static_cast<int>(std::floor(arrowX));
                int voxelY = static_cast<int>(std::floor(arrowY));
                int voxelZ = static_cast<int>(std::floor(arrowZ));
                const int endX = static_cast<int>(std::floor(nextX));
                const int endY = static_cast<int>(std::floor(nextY));
                const int endZ = static_cast<int>(std::floor(nextZ));
                const int stepX = velocityX > 0.0 ? 1 : velocityX < 0.0 ? -1 : 0;
                const int stepY = velocityY > 0.0 ? 1 : velocityY < 0.0 ? -1 : 0;
                const int stepZ = velocityZ > 0.0 ? 1 : velocityZ < 0.0 ? -1 : 0;
                const double infinity = std::numeric_limits<double>::infinity();
                double maxTX = stepX > 0 ? (voxelX + 1.0 - arrowX) / velocityX
                    : stepX < 0 ? (arrowX - voxelX) / -velocityX : infinity;
                double maxTY = stepY > 0 ? (voxelY + 1.0 - arrowY) / velocityY
                    : stepY < 0 ? (arrowY - voxelY) / -velocityY : infinity;
                double maxTZ = stepZ > 0 ? (voxelZ + 1.0 - arrowZ) / velocityZ
                    : stepZ < 0 ? (arrowZ - voxelZ) / -velocityZ : infinity;
                const double deltaTX = stepX == 0 ? infinity : 1.0 / std::abs(velocityX);
                const double deltaTY = stepY == 0 ? infinity : 1.0 / std::abs(velocityY);
                const double deltaTZ = stepZ == 0 ? infinity : 1.0 / std::abs(velocityZ);
                double blockHitT = infinity;
                for (int crossing = 0; crossing < 32 &&
                     (voxelX != endX || voxelY != endY || voxelZ != endZ);
                     ++crossing) {
                    double crossingT = 0.0;
                    if (maxTX <= maxTY && maxTX <= maxTZ) {
                        crossingT = maxTX;
                        maxTX += deltaTX;
                        voxelX += stepX;
                    } else if (maxTY <= maxTZ) {
                        crossingT = maxTY;
                        maxTY += deltaTY;
                        voxelY += stepY;
                    } else {
                        crossingT = maxTZ;
                        maxTZ += deltaTZ;
                        voxelZ += stepZ;
                    }
                    if (crossingT > 1.0) break;
                    bool air = true;
                    if (blockIsAir(voxelX + 0.5, voxelY + 0.5,
                                   voxelZ + 0.5, air) && !air) {
                        blockHitT = std::clamp(crossingT, 0.0, 1.0);
                        break;
                    }
                }
                const double hitT = std::min(entityHitT, blockHitT);
                if (std::isfinite(hitT)) {
                    trajectory.hasImpact = true;
                    trajectory.impactPlayer = entityHitT <= blockHitT;
                    trajectory.impactEntityId = trajectory.impactPlayer
                        ? entityHitId : -1;
                    trajectory.impact = {
                        arrowX + velocityX * hitT,
                        arrowY + velocityY * hitT,
                        arrowZ + velocityZ * hitT};
                }
                if (trajectory.hasImpact) {
                    trajectory.points[trajectory.pointCount++] = trajectory.impact;
                    break;
                }
                arrowX = nextX;
                arrowY = nextY;
                arrowZ = nextZ;
                trajectory.points[trajectory.pointCount++] = {arrowX, arrowY, arrowZ};
                velocityX *= 0.99;
                velocityY = velocityY * 0.99 - 0.05;
                velocityZ *= 0.99;
            }
        }
        m_snapshot.bowTrajectory = trajectory;
    }
    m_snapshot.entitySampleGeneration = ++m_entitySampleGeneration;

    // Multiplayer discovery is metadata-only and independent of ESP. At 2 Hz,
    // two stable Sidebar snapshots activate a match in about one second while
    // keeping all collection outside the per-frame renderer path.
    if (tickMilliseconds - m_lastPlayerScan >= 500U || m_lastPlayerScan == 0U) {
        m_lastPlayerScan = tickMilliseconds;
        const bool previousMatchActive = m_snapshot.matchActive;
        const char previousOwnTeam = m_snapshot.ownTeam;
        std::array<PlayerIdentity, GameSnapshot::MaxDiscoveredPlayers> nextPlayers{};
        std::array<std::string, GameSnapshot::MaxDiscoveredPlayers> rosterFormatted{};
        std::array<char, 17U> nextLocalName{};
        std::uint32_t nextCount = 0U;
        jstring localName = static_cast<jstring>(env->CallObjectMethod(player, cache->getName));
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            localName = nullptr;
        }
        if (localName != nullptr) {
            const char* const localUtf8 = env->GetStringUTFChars(localName, nullptr);
            if (env->ExceptionCheck() != JNI_TRUE && localUtf8 != nullptr) {
                const std::string_view localView(localUtf8);
                if (!localView.empty() && localView.size() <= 16U) {
                    std::copy(localView.begin(), localView.end(), nextLocalName.begin());
                }
            }
            if (localUtf8 != nullptr) env->ReleaseStringUTFChars(localName, localUtf8);
            if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
            env->DeleteLocalRef(localName);
        }
        auto appendRosterPlayer = [&](const std::string_view nameView,
                                      const std::string_view formattedView,
                                      const std::array<char, 37U>& uuidValue) noexcept {
            const bool validName = !nameView.empty() && nameView.size() <= 16U &&
                std::all_of(nameView.begin(), nameView.end(), [](const char c) noexcept {
                    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_';
                });
            if (!validName || nextCount >= nextPlayers.size()) return;
            for (std::uint32_t existing = 0U; existing < nextCount; ++existing) {
                const bool sameUuid = uuidValue[0U] != '\0' &&
                    nextPlayers[existing].uuid[0U] != '\0' &&
                    std::strcmp(uuidValue.data(), nextPlayers[existing].uuid.data()) == 0;
                if (sameUuid || nameView == nextPlayers[existing].name.data()) return;
            }
            PlayerIdentity identity{};
            std::copy(nameView.begin(), nameView.end(), identity.name.begin());
            identity.uuid = uuidValue;
            identity.teamColor = 'u';
            rosterFormatted[nextCount].assign(formattedView);
            nextPlayers[nextCount++] = identity;
        };

        // Prefer the server-maintained TAB collection. Unlike playerEntities,
        // it contains roster members before their entity is spawned/tracked by
        // the client and remains available when they are far outside render
        // distance. Mid-match additions are picked up by this continuous 2 Hz
        // scan as well.
        bool tabRosterRead = false;
        if (cache->getNetHandler != nullptr && cache->getPlayerInfoMap != nullptr &&
            cache->getGameProfile != nullptr && cache->gameProfileGetName != nullptr) {
            jobject netHandler = env->CallObjectMethod(minecraft, cache->getNetHandler);
            jobject playerInfoMap = nullptr;
            jobjectArray playerInfos = nullptr;
            if (env->ExceptionCheck() != JNI_TRUE && netHandler != nullptr) {
                playerInfoMap = env->CallObjectMethod(netHandler, cache->getPlayerInfoMap);
            }
            if (env->ExceptionCheck() != JNI_TRUE && playerInfoMap != nullptr) {
                playerInfos = static_cast<jobjectArray>(env->CallObjectMethod(
                    playerInfoMap, cache->collectionToArray));
            }
            if (env->ExceptionCheck() != JNI_TRUE && playerInfos != nullptr) {
                const jsize count = std::min<jsize>(env->GetArrayLength(playerInfos),
                    static_cast<jsize>(nextPlayers.size()));
                for (jsize index = 0; index < count; ++index) {
                    jobject info = env->GetObjectArrayElement(playerInfos, index);
                    jobject profileObject = info == nullptr ? nullptr :
                        env->CallObjectMethod(info, cache->getGameProfile);
                    jstring name = profileObject == nullptr ? nullptr : static_cast<jstring>(
                        env->CallObjectMethod(profileObject, cache->gameProfileGetName));
                    std::array<char, 37U> profileUuid{};
                    if (profileObject != nullptr && cache->gameProfileGetId != nullptr) {
                        jobject uuidObject = env->CallObjectMethod(
                            profileObject, cache->gameProfileGetId);
                        if (env->ExceptionCheck() != JNI_TRUE && uuidObject != nullptr) {
                            (void)copyUuidString(uuidObject, profileUuid);
                        }
                        clearException(env);
                        if (uuidObject != nullptr) env->DeleteLocalRef(uuidObject);
                    }
                    if (env->ExceptionCheck() != JNI_TRUE && name != nullptr) {
                        const char* const utf8 = env->GetStringUTFChars(name, nullptr);
                        if (env->ExceptionCheck() != JNI_TRUE && utf8 != nullptr) {
                            appendRosterPlayer(utf8, {}, profileUuid);
                            env->ReleaseStringUTFChars(name, utf8);
                        }
                    }
                    clearException(env);
                    if (name != nullptr) env->DeleteLocalRef(name);
                    if (profileObject != nullptr) env->DeleteLocalRef(profileObject);
                    if (info != nullptr) env->DeleteLocalRef(info);
                }
                tabRosterRead = nextCount != 0U;
            }
            clearException(env);
            if (playerInfos != nullptr) env->DeleteLocalRef(playerInfos);
            if (playerInfoMap != nullptr) env->DeleteLocalRef(playerInfoMap);
            if (netHandler != nullptr) env->DeleteLocalRef(netHandler);
        }

        // Compatibility fallback for transformed clients without a usable TAB
        // mapping. This path sees only currently spawned player entities.
        if (!tabRosterRead) {
            jobject playerList = env->GetObjectField(world, cache->playerEntities);
            jobjectArray playerArray = playerList == nullptr ? nullptr :
                static_cast<jobjectArray>(env->CallObjectMethod(
                    playerList, cache->listToArray));
            if (env->ExceptionCheck() == JNI_TRUE) {
                env->ExceptionClear();
                playerArray = nullptr;
            }
            if (playerArray != nullptr) {
                const jsize count = std::min<jsize>(env->GetArrayLength(playerArray),
                    static_cast<jsize>(nextPlayers.size()));
                for (jsize index = 0; index < count; ++index) {
                    jobject remotePlayer = env->GetObjectArrayElement(playerArray, index);
                    if (env->ExceptionCheck() == JNI_TRUE || remotePlayer == nullptr) {
                        clearException(env);
                        continue;
                    }
                    jstring name = static_cast<jstring>(
                        env->CallObjectMethod(remotePlayer, cache->getName));
                    jobject display = env->CallObjectMethod(remotePlayer, cache->getDisplayName);
                    jstring formatted = display == nullptr ? nullptr : static_cast<jstring>(
                        env->CallObjectMethod(display, cache->getFormattedText));
                    if (env->ExceptionCheck() != JNI_TRUE && name != nullptr) {
                        const char* const nameUtf8 = env->GetStringUTFChars(name, nullptr);
                        const char* const formattedUtf8 = formatted == nullptr ? nullptr :
                            env->GetStringUTFChars(formatted, nullptr);
                        if (env->ExceptionCheck() != JNI_TRUE && nameUtf8 != nullptr) {
                            std::array<char, 37U> entityUuid{};
                            if (cache->getUniqueId != nullptr) {
                                jobject uuidObject = env->CallObjectMethod(
                                    remotePlayer, cache->getUniqueId);
                                if (env->ExceptionCheck() != JNI_TRUE && uuidObject != nullptr) {
                                    (void)copyUuidString(uuidObject, entityUuid);
                                }
                                clearException(env);
                                if (uuidObject != nullptr) env->DeleteLocalRef(uuidObject);
                            }
                            appendRosterPlayer(nameUtf8,
                                formattedUtf8 == nullptr ? std::string_view{} :
                                std::string_view(formattedUtf8), entityUuid);
                        }
                        if (formattedUtf8 != nullptr)
                            env->ReleaseStringUTFChars(formatted, formattedUtf8);
                        if (nameUtf8 != nullptr)
                            env->ReleaseStringUTFChars(name, nameUtf8);
                    }
                    clearException(env);
                    if (formatted != nullptr) env->DeleteLocalRef(formatted);
                    if (display != nullptr) env->DeleteLocalRef(display);
                    if (name != nullptr) env->DeleteLocalRef(name);
                    env->DeleteLocalRef(remotePlayer);
                }
                env->DeleteLocalRef(playerArray);
            }
            if (playerList != nullptr) env->DeleteLocalRef(playerList);
        }
        std::array<std::string, 32U> sidebarStorage{};
        std::array<std::string_view, 32U> sidebarViews{};
        std::size_t sidebarLineCount = 0U;
        const bool sidebarAvailable =
            cache->getScoreboard != nullptr &&
            cache->getObjectiveInDisplaySlot != nullptr &&
            cache->getPlayersTeam != nullptr &&
            cache->getSortedScores != nullptr &&
            cache->getPlayerName != nullptr &&
            cache->formatPlayerName != nullptr &&
            cache->scorePlayerTeamClass != nullptr;
        jobject scoreboard = sidebarAvailable
            ? env->CallObjectMethod(world, cache->getScoreboard)
            : nullptr;
        if (env->ExceptionCheck() != JNI_TRUE && scoreboard != nullptr) {
            // TAB's GameProfile only carries the raw account name. Resolve its
            // current team through the world scoreboard and format it exactly
            // as Minecraft does; entries without a recognised colour/tag are
            // intentionally excluded later (lobby NPCs and start robots).
            for (std::uint32_t index = 0U; index < nextCount; ++index) {
                if (!rosterFormatted[index].empty()) continue;
                jstring name = env->NewStringUTF(nextPlayers[index].name.data());
                jobject team = name == nullptr ? nullptr : env->CallObjectMethod(
                    scoreboard, cache->getPlayersTeam, name);
                jstring formatted = team == nullptr ? nullptr : static_cast<jstring>(
                    env->CallStaticObjectMethod(cache->scorePlayerTeamClass,
                        cache->formatPlayerName, team, name));
                if (env->ExceptionCheck() != JNI_TRUE && formatted != nullptr) {
                    const char* const utf8 = env->GetStringUTFChars(formatted, nullptr);
                    if (env->ExceptionCheck() != JNI_TRUE && utf8 != nullptr) {
                        rosterFormatted[index] = utf8;
                        env->ReleaseStringUTFChars(formatted, utf8);
                    }
                }
                clearException(env);
                if (formatted != nullptr) env->DeleteLocalRef(formatted);
                if (team != nullptr) env->DeleteLocalRef(team);
                if (name != nullptr) env->DeleteLocalRef(name);
            }
            jobject objective = env->CallObjectMethod(scoreboard, cache->getObjectiveInDisplaySlot, 1);
            if (env->ExceptionCheck() != JNI_TRUE && objective != nullptr) {
                jobject scores = env->CallObjectMethod(scoreboard, cache->getSortedScores, objective);
                if (env->ExceptionCheck() != JNI_TRUE && scores != nullptr) {
                    jobjectArray scoresArray = static_cast<jobjectArray>(env->CallObjectMethod(
                        scores, cache->collectionToArray));
                    if (env->ExceptionCheck() != JNI_TRUE && scoresArray != nullptr) {
                        const jsize len = std::min<jsize>(
                            env->GetArrayLength(scoresArray),
                            static_cast<jsize>(sidebarStorage.size()));
                        for (jsize i = 0; i < len; i++) {
                            jobject scoreObj = env->GetObjectArrayElement(scoresArray, i);
                            if (env->ExceptionCheck() != JNI_TRUE && scoreObj != nullptr) {
                                jstring playerNameStr = static_cast<jstring>(env->CallObjectMethod(scoreObj, cache->getPlayerName));
                                if (env->ExceptionCheck() != JNI_TRUE && playerNameStr != nullptr) {
                                    jobject team = env->CallObjectMethod(scoreboard, cache->getPlayersTeam, playerNameStr);
                                    if (env->ExceptionCheck() != JNI_TRUE && team != nullptr) {
                                        jstring formattedStr = static_cast<jstring>(env->CallStaticObjectMethod(cache->scorePlayerTeamClass, cache->formatPlayerName, team, playerNameStr));
                                        if (env->ExceptionCheck() != JNI_TRUE && formattedStr != nullptr) {
                                            const char* formattedUtf8 = env->GetStringUTFChars(formattedStr, nullptr);
                                            if (formattedUtf8 != nullptr) {
                                                if (sidebarLineCount < sidebarStorage.size()) {
                                                    sidebarStorage[sidebarLineCount] = formattedUtf8;
                                                    sidebarViews[sidebarLineCount] =
                                                        sidebarStorage[sidebarLineCount];
                                                    ++sidebarLineCount;
                                                }
                                                env->ReleaseStringUTFChars(formattedStr, formattedUtf8);
                                            }
                                            env->DeleteLocalRef(formattedStr);
                                        }
                                        env->DeleteLocalRef(team);
                                    }
                                    env->DeleteLocalRef(playerNameStr);
                                }
                                env->DeleteLocalRef(scoreObj);
                            }
                        }
                        env->DeleteLocalRef(scoresArray);
                    }
                    env->DeleteLocalRef(scores);
                }
                env->DeleteLocalRef(objective);
            }
            env->DeleteLocalRef(scoreboard);
        }
        env->ExceptionClear();

        bool localChestplatePresent = false;
        std::uint8_t localProtectionLevel = 0U;
        const bedwars::Team localArmorTeam =
            readPlayerArmorTeam(player, localChestplatePresent,
                                localProtectionLevel);
        (void)localChestplatePresent;

        bedwars::Team rosterTaggedOwnTeam = bedwars::Team::Unknown;
        bedwars::Team rosterColourOwnTeam = bedwars::Team::Unknown;
        std::uint16_t rosterTeamMask = 0U;
        std::uint32_t taggedPlayers = 0U;
        for (std::uint32_t index = 0U; index < nextCount; ++index) {
            const bedwars::Team tagged =
                bedwars::parseRosterTeamTag(rosterFormatted[index]);
            const std::uint8_t teamIndex = bedwars::teamIndex(tagged);
            if (teamIndex >= 8U) continue;
            ++taggedPlayers;
            rosterTeamMask |= static_cast<std::uint16_t>(1U << teamIndex);
            if (nextLocalName[0U] != '\0' &&
                std::strcmp(nextPlayers[index].name.data(), nextLocalName.data()) == 0) {
                rosterTaggedOwnTeam = tagged;
                rosterColourOwnTeam =
                    bedwars::parseRosterTeam(rosterFormatted[index]);
            }
        }
        std::uint8_t rosterDistinctTeams = 0U;
        for (std::uint16_t mask = rosterTeamMask; mask != 0U; mask >>= 1U)
            rosterDistinctTeams += static_cast<std::uint8_t>(mask & 1U);
        // Restore the former roster detector: explicit [R]/[B]/... tags are
        // match evidence even when Lunar inserts an unrelated colour before
        // the tag. Prefer an explicit local tag, then the actual dyed local
        // chestplate, and only then the formatted-name colour.
        const bedwars::Team rosterOwnTeam =
            rosterTaggedOwnTeam != bedwars::Team::Unknown ? rosterTaggedOwnTeam :
            localArmorTeam != bedwars::Team::Unknown ? localArmorTeam :
            rosterColourOwnTeam;
        const bool rosterValid = taggedPlayers >= 2U &&
            rosterDistinctTeams >= 2U && rosterOwnTeam != bedwars::Team::Unknown;

        const bedwars::SidebarSnapshot sidebar = bedwars::parseSidebar(
            std::span<const std::string_view>(sidebarViews.data(), sidebarLineCount));

        // Last-resort match evidence for transformed clients whose scoreboard
        // wrappers remove both Sidebar suffixes and roster tags. It is accepted
        // only when (a) the local leather colour is known, (b) at least two
        // distinct live player armour teams are present, and (c) the world bed
        // scanner has found a bed. This avoids treating lobby rank colours as a
        // match while keeping team detection usable on Lunar.
        std::uint16_t armorTeamMask = 0U;
        const std::uint8_t localArmorIndex = bedwars::teamIndex(localArmorTeam);
        if (localArmorIndex < 8U)
            armorTeamMask |= static_cast<std::uint16_t>(1U << localArmorIndex);
        for (std::uint32_t index = 0U; index < m_snapshot.entityMarkerCount; ++index) {
            const EntityMarker& marker = m_snapshot.entityMarkers[index];
            if (!marker.player) continue;
            const std::uint8_t armorIndex = bedwars::teamIndex(
                bedwars::fromFormatCode(marker.armorTeam));
            if (armorIndex < 8U)
                armorTeamMask |= static_cast<std::uint16_t>(1U << armorIndex);
        }
        std::uint8_t armorDistinctTeams = 0U;
        for (std::uint16_t mask = armorTeamMask; mask != 0U; mask >>= 1U)
            armorDistinctTeams += static_cast<std::uint8_t>(mask & 1U);
        std::uint32_t publishedBedCount = 0U;
        ::AcquireSRWLockShared(&m_bedCacheLock);
        publishedBedCount = m_publishedBedCache.markerCount;
        ::ReleaseSRWLockShared(&m_bedCacheLock);
        const bool armorValid = localArmorTeam != bedwars::Team::Unknown &&
            armorDistinctTeams >= 2U && publishedBedCount > 0U;

        const bool matchEvidenceValid = sidebar.valid || rosterValid || armorValid;
        const bedwars::Team candidateTeam = sidebar.valid ? sidebar.ownTeam :
            rosterValid ? rosterOwnTeam : localArmorTeam;
        if (matchEvidenceValid) {
            m_sidebarMissingCount = 0U;
            if (candidateTeam == m_sidebarCandidateTeam) {
                if (m_sidebarStableCount < UINT8_MAX) ++m_sidebarStableCount;
            } else {
                m_sidebarCandidateTeam = candidateTeam;
                m_sidebarStableCount = 1U;
            }
            // A valid Sidebar "YOU" row is server-authored and can activate
            // immediately. Roster/armor fallbacks still require two samples.
            const std::uint8_t requiredStableSamples = sidebar.valid ? 1U : 2U;
            if (m_sidebarStableCount >= requiredStableSamples &&
                !m_snapshot.matchActive) {
                m_snapshot.matchActive = true;
                m_snapshot.ownTeam = bedwars::formatCode(candidateTeam);
            }
        } else {
            if (m_snapshot.matchActive) {
                // Entity tracking, TAB refreshes and Sidebar packet changes are
                // all transient. Once a match has been confirmed, only a
                // WorldClient lifecycle boundary may clear it; otherwise a far
                // enemy or a respawning teammate would flip the entire session
                // back into lobby mode.
                if (m_sidebarMissingCount < UINT8_MAX) ++m_sidebarMissingCount;
            } else {
                m_sidebarCandidateTeam = bedwars::Team::Unknown;
                m_sidebarStableCount = 0U;
            }
        }

        MatchProbeState nextProbe{};
        nextProbe.sidebarAvailable = sidebarAvailable;
        nextProbe.tabAvailable = cache->getNetHandler != nullptr &&
            cache->getPlayerInfoMap != nullptr && cache->getGameProfile != nullptr &&
            cache->gameProfileGetName != nullptr;
        nextProbe.sidebarEvidence = sidebar.valid;
        nextProbe.rosterEvidence = rosterValid;
        nextProbe.armorEvidence = armorValid;
        nextProbe.sidebarLines = static_cast<std::uint8_t>(
            std::min<std::size_t>(sidebarLineCount, UINT8_MAX));
        nextProbe.sidebarTeams = sidebar.distinctTeams;
        nextProbe.sidebarYouRows = sidebar.youRows;
        nextProbe.rosterTaggedPlayers = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(taggedPlayers, UINT8_MAX));
        nextProbe.rosterPlayers = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(nextCount, UINT8_MAX));
        nextProbe.rosterTeams = rosterDistinctTeams;
        nextProbe.rosterOwnTeam = bedwars::formatCode(rosterOwnTeam);
        nextProbe.localArmorTeam = bedwars::formatCode(localArmorTeam);
        nextProbe.armorTeams = armorDistinctTeams;
        nextProbe.stableCount = m_sidebarStableCount;
        if (!(nextProbe == m_matchProbe)) {
            m_matchProbe = nextProbe;
            ++m_matchProbeGeneration;
        }

        const bool nextMatchActive = m_snapshot.matchActive;
        const char nextOwnTeam = nextMatchActive ? m_snapshot.ownTeam : 'u';
        if (nextMatchActive && !previousMatchActive) {
            m_matchAnchorValid = true;
            m_matchAnchorX = positionX;
            m_matchAnchorY = positionY;
            m_matchAnchorZ = positionZ;
            m_lockedOwnBedKnown = false;
            m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
            ++m_bedOwnershipGeneration;
        } else if (!nextMatchActive && previousMatchActive) {
            m_matchAnchorValid = false;
            m_lockedOwnBedKnown = false;
            m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
        }
        if (nextMatchActive) {
            std::uint32_t compactCount = 0U;
            for (std::uint32_t index = 0U; index < nextCount; ++index) {
                const bedwars::Team team = bedwars::parseRosterTeam(rosterFormatted[index]);
                if (team == bedwars::Team::Unknown) continue;
                nextPlayers[index].teamColor = bedwars::formatCode(team);
                if (compactCount != index) nextPlayers[compactCount] = nextPlayers[index];
                ++compactCount;
            }
            nextCount = compactCount;

            // A confirmed world's roster is monotonic: TAB can temporarily
            // omit an entry during respawn/network refresh, but that must not
            // turn a known teammate into an enemy or trigger duplicate API
            // queries. New, colour-tagged players are appended; uncoloured
            // startup NPCs/bots are never admitted.
            for (std::uint32_t oldIndex = 0U;
                 oldIndex < m_snapshot.playerCount &&
                 nextCount < nextPlayers.size(); ++oldIndex) {
                const PlayerIdentity& oldPlayer = m_snapshot.players[oldIndex];
                bool alreadyPresent = false;
                for (std::uint32_t index = 0U; index < nextCount; ++index) {
                    const bool sameUuid = nextPlayers[index].uuid[0U] != '\0' &&
                        oldPlayer.uuid[0U] != '\0' &&
                        std::strcmp(nextPlayers[index].uuid.data(),
                                    oldPlayer.uuid.data()) == 0;
                    if (sameUuid || std::strcmp(nextPlayers[index].name.data(),
                                                oldPlayer.name.data()) == 0) {
                        // Preserve the first confirmed team for this world.
                        nextPlayers[index].teamColor = oldPlayer.teamColor;
                        alreadyPresent = true;
                        break;
                    }
                }
                if (!alreadyPresent) nextPlayers[nextCount++] = oldPlayer;
            }
        } else {
            // Outside a confirmed match we deliberately publish no team
            // decisions, preventing lobby ranks/NPCs from reaching Debug chat
            // or the automatic Hypixel query pipeline.
            nextPlayers = {};
            nextCount = 0U;
        }

        std::sort(nextPlayers.begin(), nextPlayers.begin() + nextCount,
                  [](const PlayerIdentity& first, const PlayerIdentity& second) noexcept {
                      return std::strcmp(first.name.data(), second.name.data()) < 0;
                  });
        bool rosterChanged = nextCount != m_snapshot.playerCount;
        for (std::uint32_t index = 0U; !rosterChanged && index < nextCount; ++index) {
            rosterChanged = std::strcmp(nextPlayers[index].name.data(),
                                        m_snapshot.players[index].name.data()) != 0 ||
                            std::strcmp(nextPlayers[index].uuid.data(),
                                        m_snapshot.players[index].uuid.data()) != 0 ||
                            nextPlayers[index].teamColor != m_snapshot.players[index].teamColor;
        }

        const bool statusChanged = nextMatchActive != previousMatchActive ||
            nextOwnTeam != previousOwnTeam ||
            std::strcmp(nextLocalName.data(), m_snapshot.localPlayerName.data()) != 0;
        if (rosterChanged || statusChanged) {
            m_snapshot.players = nextPlayers;
            m_snapshot.playerCount = nextCount;
            m_snapshot.localPlayerName = nextLocalName;
            m_snapshot.matchActive = nextMatchActive;
            m_snapshot.ownTeam = nextOwnTeam;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
        }
    }

    // The scanner thread publishes immutable fixed storage. SwapBuffers only
    // takes a short shared SRW lock and performs no block JNI calls.
    if (singlePlayer == JNI_TRUE) {
        const bool previousOwnBedKnown = m_snapshot.ownBedKnown;
        const int previousOwnBedX = m_snapshot.ownBedX;
        const int previousOwnBedY = m_snapshot.ownBedY;
        const int previousOwnBedZ = m_snapshot.ownBedZ;
        const GameSnapshot::OwnBedSource previousOwnBedSource =
            m_snapshot.ownBedSource;
        ::AcquireSRWLockShared(&m_bedCacheLock);
        m_snapshot.bedMarkerCount = m_publishedBedCache.markerCount;
        m_snapshot.bedCount = m_publishedBedCache.markerCount;
        std::copy_n(m_publishedBedCache.markers.begin(),
                    m_publishedBedCache.markerCount, m_snapshot.bedMarkers.begin());
        m_snapshot.bedScanProgress = m_publishedBedCache.generation == 0U ? 0.0F : 1.0F;
        ::ReleaseSRWLockShared(&m_bedCacheLock);

        m_snapshot.ownBedKnown = false;
        m_snapshot.ownBedSource = GameSnapshot::OwnBedSource::Unknown;
        if (m_snapshot.matchActive && m_snapshot.ownTeam != 'u') {
            const BedMarker* teamCandidate = nullptr;
            std::uint32_t matchingBeds = 0U;
            for (std::uint32_t index = 0U; index < m_snapshot.bedMarkerCount; ++index) {
                const BedMarker& bed = m_snapshot.bedMarkers[index];
                if (bed.teamColor != m_snapshot.ownTeam) continue;
                teamCandidate = &bed;
                ++matchingBeds;
            }

            // Team-coloured wool is authoritative whenever exactly one bed
            // matches. If a bed is initially unprotected, lock the nearest
            // bed to the stable match-start position and upgrade that lock
            // later when team-wool evidence arrives.
            if (matchingBeds == 1U && teamCandidate != nullptr &&
                (!m_lockedOwnBedKnown ||
                 m_lockedOwnBedSource == GameSnapshot::OwnBedSource::MatchSpawn)) {
                m_lockedOwnBedKnown = true;
                m_lockedOwnBedX = teamCandidate->x;
                m_lockedOwnBedY = teamCandidate->y;
                m_lockedOwnBedZ = teamCandidate->z;
                m_lockedOwnBedSource = GameSnapshot::OwnBedSource::TeamWool;
            }
            if (!m_lockedOwnBedKnown && m_matchAnchorValid) {
                const BedMarker* nearest = nullptr;
                double nearestDistanceSq = 36.0 * 36.0;
                for (std::uint32_t index = 0U; index < m_snapshot.bedMarkerCount; ++index) {
                    const BedMarker& bed = m_snapshot.bedMarkers[index];
                    const double centerX =
                        (static_cast<double>(bed.x + bed.footX) + 1.0) * 0.5;
                    const double centerZ =
                        (static_cast<double>(bed.z + bed.footZ) + 1.0) * 0.5;
                    const double dx = centerX - m_matchAnchorX;
                    const double dy = static_cast<double>(bed.y) - m_matchAnchorY;
                    const double dz = centerZ - m_matchAnchorZ;
                    if (std::abs(dy) > 16.0) continue;
                    const double distanceSq = dx * dx + dy * dy + dz * dz;
                    if (distanceSq < nearestDistanceSq) {
                        nearestDistanceSq = distanceSq;
                        nearest = &bed;
                    }
                }
                if (nearest != nullptr) {
                    m_lockedOwnBedKnown = true;
                    m_lockedOwnBedX = nearest->x;
                    m_lockedOwnBedY = nearest->y;
                    m_lockedOwnBedZ = nearest->z;
                    m_lockedOwnBedSource = GameSnapshot::OwnBedSource::MatchSpawn;
                }
            }

            if (m_lockedOwnBedKnown) {
                bool lockedMarkerLoaded = false;
                for (std::uint32_t index = 0U; index < m_snapshot.bedMarkerCount; ++index) {
                    const BedMarker& bed = m_snapshot.bedMarkers[index];
                    if (bed.x != m_lockedOwnBedX || bed.y != m_lockedOwnBedY ||
                        bed.z != m_lockedOwnBedZ) continue;
                    lockedMarkerLoaded = true;
                    break;
                }
                // Bed caches are intentionally evicted when their chunk
                // unloads. Keep the ownership lock while the local player is
                // far away, but treat a missing marker in nearby/loaded range
                // as a real bed destruction so alerts disappear promptly.
                const double bedDx = (static_cast<double>(m_lockedOwnBedX) + 0.5) - positionX;
                const double bedDz = (static_cast<double>(m_lockedOwnBedZ) + 0.5) - positionZ;
                const bool bedShouldBeLoaded = bedDx * bedDx + bedDz * bedDz <= 96.0 * 96.0;
                if (!lockedMarkerLoaded && bedShouldBeLoaded) {
                    m_lockedOwnBedKnown = false;
                    m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
                } else {
                    m_snapshot.ownBedKnown = true;
                    m_snapshot.ownBedX = m_lockedOwnBedX;
                    m_snapshot.ownBedY = m_lockedOwnBedY;
                    m_snapshot.ownBedZ = m_lockedOwnBedZ;
                    m_snapshot.ownBedSource = m_lockedOwnBedSource;
                }
            }
        } else {
            m_lockedOwnBedKnown = false;
            m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
        }
        if (previousOwnBedKnown != m_snapshot.ownBedKnown ||
            previousOwnBedX != m_snapshot.ownBedX ||
            previousOwnBedY != m_snapshot.ownBedY ||
            previousOwnBedZ != m_snapshot.ownBedZ ||
            previousOwnBedSource != m_snapshot.ownBedSource) {
            ++m_bedOwnershipGeneration;
        }
    } else {
        m_snapshot.bedCount = 0U;
        m_snapshot.bedMarkerCount = 0U;
        m_snapshot.bedScanProgress = 0.0F;
        m_snapshot.ownBedKnown = false;
        m_snapshot.ownBedSource = GameSnapshot::OwnBedSource::Unknown;
    }
    if (textureManagerObject != nullptr) env->DeleteLocalRef(textureManagerObject);

    env->PopLocalFrame(nullptr);
    m_snapshot.health = health;
    m_snapshot.maxHealth = maxHealth;
    m_snapshot.entityId = entityId;
    m_snapshot.x = positionX;
    m_snapshot.y = positionY;
    m_snapshot.z = positionZ;
    m_snapshot.bounds = box;
    m_snapshot.loadedEntities = loadedEntityCount;
    m_snapshot.mappingAttempt = m_mappingAttempt.load(std::memory_order_acquire);
    m_snapshot.mappingRetryInMs = 0U;
    m_snapshot.mapping = cache->profile->label.c_str();
    m_snapshot.state = GameSnapshot::State::Ready;
    return m_snapshot;
}

void GameBindings::deleteGlobalRefs(JNIEnv* const env, BindingCache& cache) noexcept
{
    if (env == nullptr) {
        return;
    }
    if (cache.minecraftClass != nullptr) env->DeleteGlobalRef(cache.minecraftClass);
    if (cache.playerClass != nullptr) env->DeleteGlobalRef(cache.playerClass);
    if (cache.livingClass != nullptr) env->DeleteGlobalRef(cache.livingClass);
    if (cache.entityClass != nullptr) env->DeleteGlobalRef(cache.entityClass);
    if (cache.fireballClass != nullptr) env->DeleteGlobalRef(cache.fireballClass);
    if (cache.aabbClass != nullptr) env->DeleteGlobalRef(cache.aabbClass);
    if (cache.worldClass != nullptr) env->DeleteGlobalRef(cache.worldClass);
    if (cache.worldClientClass != nullptr) env->DeleteGlobalRef(cache.worldClientClass);
    if (cache.stateClass != nullptr) env->DeleteGlobalRef(cache.stateClass);
    if (cache.blockClass != nullptr) env->DeleteGlobalRef(cache.blockClass);
    if (cache.blockPosClass != nullptr) env->DeleteGlobalRef(cache.blockPosClass);
    if (cache.bedClass != nullptr) env->DeleteGlobalRef(cache.bedClass);
    if (cache.chunkProviderClass != nullptr) env->DeleteGlobalRef(cache.chunkProviderClass);
    if (cache.chunkClass != nullptr) env->DeleteGlobalRef(cache.chunkClass);
    if (cache.storageClass != nullptr) env->DeleteGlobalRef(cache.storageClass);
    if (cache.activeRenderInfoClass != nullptr) env->DeleteGlobalRef(cache.activeRenderInfoClass);
    if (cache.renderManagerClass != nullptr) env->DeleteGlobalRef(cache.renderManagerClass);
    if (cache.timerClass != nullptr) env->DeleteGlobalRef(cache.timerClass);
    if (cache.gameSettingsClass != nullptr)
        env->DeleteGlobalRef(cache.gameSettingsClass);
    if (cache.keyBindingClass != nullptr)
        env->DeleteGlobalRef(cache.keyBindingClass);
    if (cache.playerControllerClass != nullptr)
        env->DeleteGlobalRef(cache.playerControllerClass);
    if (cache.serverDataClass != nullptr)
        env->DeleteGlobalRef(cache.serverDataClass);
    if (cache.itemBlockClass != nullptr)
        env->DeleteGlobalRef(cache.itemBlockClass);
    if (cache.enumFacingClass != nullptr)
        env->DeleteGlobalRef(cache.enumFacingClass);
    if (cache.vec3Class != nullptr)
        env->DeleteGlobalRef(cache.vec3Class);
    if (cache.chatComponentClass != nullptr) env->DeleteGlobalRef(cache.chatComponentClass);
    if (cache.chatTextClass != nullptr) env->DeleteGlobalRef(cache.chatTextClass);
    if (cache.chatSerializerClass != nullptr)
        env->DeleteGlobalRef(cache.chatSerializerClass);
    if (cache.scoreboardClass != nullptr) env->DeleteGlobalRef(cache.scoreboardClass);
    if (cache.scoreObjectiveClass != nullptr) env->DeleteGlobalRef(cache.scoreObjectiveClass);
    if (cache.scoreClass != nullptr) env->DeleteGlobalRef(cache.scoreClass);
    if (cache.scorePlayerTeamClass != nullptr) env->DeleteGlobalRef(cache.scorePlayerTeamClass);
    if (cache.netHandlerClass != nullptr) env->DeleteGlobalRef(cache.netHandlerClass);
    if (cache.networkPlayerInfoClass != nullptr) env->DeleteGlobalRef(cache.networkPlayerInfoClass);
    if (cache.gameProfileClass != nullptr) env->DeleteGlobalRef(cache.gameProfileClass);
    if (cache.itemStackClass != nullptr) env->DeleteGlobalRef(cache.itemStackClass);
    if (cache.itemClass != nullptr) env->DeleteGlobalRef(cache.itemClass);
    if (cache.itemArmorClass != nullptr) env->DeleteGlobalRef(cache.itemArmorClass);
    if (cache.inventoryPlayerClass != nullptr) env->DeleteGlobalRef(cache.inventoryPlayerClass);
    if (cache.enchantmentHelperClass != nullptr)
        env->DeleteGlobalRef(cache.enchantmentHelperClass);
    if (cache.abstractClientPlayerClass != nullptr)
        env->DeleteGlobalRef(cache.abstractClientPlayerClass);
    if (cache.resourceLocationClass != nullptr)
        env->DeleteGlobalRef(cache.resourceLocationClass);
    if (cache.textureManagerClass != nullptr)
        env->DeleteGlobalRef(cache.textureManagerClass);
    if (cache.textureObjectClass != nullptr)
        env->DeleteGlobalRef(cache.textureObjectClass);
    if (cache.uuidClass != nullptr) env->DeleteGlobalRef(cache.uuidClass);
    if (cache.renderManagerObject != nullptr) env->DeleteGlobalRef(cache.renderManagerObject);
    if (cache.timerObject != nullptr) env->DeleteGlobalRef(cache.timerObject);
    if (cache.modelViewBuffer != nullptr) env->DeleteGlobalRef(cache.modelViewBuffer);
    if (cache.projectionBuffer != nullptr) env->DeleteGlobalRef(cache.projectionBuffer);
    if (cache.viewportBuffer != nullptr) env->DeleteGlobalRef(cache.viewportBuffer);

    cache.minecraftClass = nullptr;
    cache.playerClass = nullptr;
    cache.livingClass = nullptr;
    cache.entityClass = nullptr;
    cache.fireballClass = nullptr;
    cache.aabbClass = nullptr;
    cache.worldClass = nullptr;
    cache.worldClientClass = nullptr;
    cache.stateClass = nullptr;
    cache.blockClass = nullptr;
    cache.blockPosClass = nullptr;
    cache.bedClass = nullptr;
    cache.chunkProviderClass = nullptr;
    cache.chunkClass = nullptr;
    cache.storageClass = nullptr;
    cache.activeRenderInfoClass = nullptr;
    cache.renderManagerClass = nullptr;
    cache.timerClass = nullptr;
    cache.gameSettingsClass = nullptr;
    cache.keyBindingClass = nullptr;
    cache.playerControllerClass = nullptr;
    cache.serverDataClass = nullptr;
    cache.itemBlockClass = nullptr;
    cache.enumFacingClass = nullptr;
    cache.vec3Class = nullptr;
    cache.chatComponentClass = nullptr;
    cache.chatTextClass = nullptr;
    cache.chatSerializerClass = nullptr;
    cache.scoreboardClass = nullptr;
    cache.scoreObjectiveClass = nullptr;
    cache.scoreClass = nullptr;
    cache.scorePlayerTeamClass = nullptr;
    cache.netHandlerClass = nullptr;
    cache.networkPlayerInfoClass = nullptr;
    cache.gameProfileClass = nullptr;
    cache.itemStackClass = nullptr;
    cache.itemClass = nullptr;
    cache.itemArmorClass = nullptr;
    cache.inventoryPlayerClass = nullptr;
    cache.enchantmentHelperClass = nullptr;
    cache.abstractClientPlayerClass = nullptr;
    cache.resourceLocationClass = nullptr;
    cache.textureManagerClass = nullptr;
    cache.textureObjectClass = nullptr;
    cache.uuidClass = nullptr;
    cache.renderManagerObject = nullptr;
    cache.timerObject = nullptr;
    cache.modelViewBuffer = nullptr;
    cache.projectionBuffer = nullptr;
    cache.viewportBuffer = nullptr;
}

void GameBindings::release(JNIEnv* const env) noexcept
{
    // AgentRuntime guarantees the resolver has joined and all other frame
    // callbacks have drained before this method can destroy published globals.
    if (env != nullptr && (m_safewalkSneakForced || m_aimSensitivityModified)) {
        (void)updateGameplay(env, GameplaySettings{}, m_snapshot, 0U);
    }
    m_resolutionPhase.store(ResolutionPhase::Stopped, std::memory_order_release);
    if (env != nullptr && m_cache != nullptr) {
        deleteGlobalRefs(env, *m_cache);
    }
    if (env != nullptr && m_lastWorld != nullptr) {
        env->DeleteWeakGlobalRef(m_lastWorld);
    }
    m_lastWorld = nullptr;
    m_cache.reset();
    m_snapshot = {};
    m_mappingAttempt.store(0U, std::memory_order_relaxed);
    m_retryAtMilliseconds.store(0U, std::memory_order_relaxed);
    m_lastSample = 0U;
    m_entitySampleGeneration = 0U;
    m_lastPlayerScan = 0U;
    m_playerRosterGeneration = 0U;
    m_debugRosterGeneration = 0U;
    m_bedOwnershipGeneration = 0U;
    m_debugBedOwnershipGeneration = 0U;
    m_matchProbe = {};
    m_matchProbeGeneration = 0U;
    m_debugMatchProbeGeneration = 0U;
    m_sidebarCandidateTeam = bedwars::Team::Unknown;
    m_sidebarStableCount = 0U;
    m_sidebarMissingCount = 0U;
    m_matchAnchorValid = false;
    m_lockedOwnBedKnown = false;
    m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
    ::AcquireSRWLockExclusive(&m_debugQueueLock);
    m_debugQueue = {};
    m_debugQueueHead = 0U;
    m_debugQueueCount = 0U;
    ::ReleaseSRWLockExclusive(&m_debugQueueLock);
    ::AcquireSRWLockExclusive(&m_warningQueueLock);
    m_warningQueue = {};
    m_warningQueueHead = 0U;
    m_warningQueueCount = 0U;
    ::ReleaseSRWLockExclusive(&m_warningQueueLock);
    m_bedRescanRequested.store(false, std::memory_order_relaxed);
    ::AcquireSRWLockExclusive(&m_bedCacheLock);
    m_publishedBedCache = {};
    ::ReleaseSRWLockExclusive(&m_bedCacheLock);
    if (env != nullptr && m_lwjglMouseClass != nullptr) {
        env->DeleteGlobalRef(m_lwjglMouseClass);
    }
    m_lwjglMouseClass = nullptr;
    m_lwjglSetGrabbed = nullptr;
    m_lwjglIsGrabbed = nullptr;
    if (env != nullptr && m_lwjglKeyboardClass != nullptr) {
        env->DeleteGlobalRef(m_lwjglKeyboardClass);
    }
    m_lwjglKeyboardClass = nullptr;
    m_lwjglIsKeyDown = nullptr;
    m_overlayInputSessionActive = false;
    m_inputGrabStateKnown = false;
    m_inputWasGrabbed = true;
    m_safewalkSneakForced = false;
    m_safewalkSneakKeyCode = 0;
    m_safewalkSupportMask = 0U;
    m_safewalkReleaseAt = 0U;
    m_lastScaffoldPlacementTick = 0U;
    m_lastGameplayTick = 0U;
    m_originalMouseSensitivity = 0.5F;
    m_aimSensitivityModified = false;
}

void GameBindings::abandon() noexcept
{
    // Used only when the JVM is already shutting down and no JNIEnv can be
    // obtained. The VM owns and releases its reference table at process exit.
    m_resolutionPhase.store(ResolutionPhase::Stopped, std::memory_order_release);
    m_cache.reset();
    m_lastWorld = nullptr;
    m_matchAnchorValid = false;
    m_lockedOwnBedKnown = false;
    m_lockedOwnBedSource = GameSnapshot::OwnBedSource::Unknown;
    ::AcquireSRWLockExclusive(&m_warningQueueLock);
    m_warningQueue = {};
    m_warningQueueHead = 0U;
    m_warningQueueCount = 0U;
    ::ReleaseSRWLockExclusive(&m_warningQueueLock);
    m_lwjglMouseClass = nullptr;
    m_lwjglSetGrabbed = nullptr;
    m_lwjglIsGrabbed = nullptr;
    m_lwjglKeyboardClass = nullptr;
    m_lwjglIsKeyDown = nullptr;
    m_overlayInputSessionActive = false;
    m_inputGrabStateKnown = false;
    m_inputWasGrabbed = true;
    m_safewalkSneakForced = false;
    m_safewalkSneakKeyCode = 0;
    m_safewalkSupportMask = 0U;
    m_safewalkReleaseAt = 0U;
    m_lastScaffoldPlacementTick = 0U;
    m_lastGameplayTick = 0U;
    m_originalMouseSensitivity = 0.5F;
    m_aimSensitivityModified = false;
}

} // namespace mcoverlay

