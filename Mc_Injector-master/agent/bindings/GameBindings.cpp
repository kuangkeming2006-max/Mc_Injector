#include "GameBindings.h"

#include "src/AgentLog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
    jclass chatComponentClass = nullptr;
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
    jmethodID getDisplayName = nullptr;
    jmethodID getFormattedText = nullptr;
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
    jmethodID getRenderManager = nullptr;
    std::array<jfieldID, 3U> renderPosition{};
    jfieldID activeModelView = nullptr;
    jfieldID activeProjection = nullptr;
    jfieldID activeViewport = nullptr;
    jfieldID timerField = nullptr;
    jfieldID renderPartialTicks = nullptr;
    jfieldID minX = nullptr;
    jfieldID minY = nullptr;
    jfieldID minZ = nullptr;
    jfieldID maxX = nullptr;
    jfieldID maxY = nullptr;
    jfieldID maxZ = nullptr;

    const MappingProfile* profile = nullptr;
};

namespace {

int bedWarsTeamIndex(const std::string_view formatted) noexcept
{
    constexpr std::array<std::string_view, 14U> tags{
        "R", "RED", "B", "BLUE", "G", "GREEN", "Y", "YELLOW",
        "A", "AQUA", "W", "WHITE", "P", "PINK"};
    constexpr std::array<int, 14U> indices{0,0,1,1,2,2,3,3,4,4,5,5,6,6};
    for (std::size_t offset = 0U; offset + 5U < formatted.size(); ++offset) {
        if (static_cast<unsigned char>(formatted[offset]) != 0xC2U ||
            static_cast<unsigned char>(formatted[offset + 1U]) != 0xA7U ||
            formatted[offset + 3U] != '[') continue;
        const std::size_t close = formatted.find(']', offset + 4U);
        if (close == std::string_view::npos || close - (offset + 4U) > 6U) continue;
        const std::string_view tag = formatted.substr(offset + 4U, close - (offset + 4U));
        for (std::size_t index = 0U; index < tags.size(); ++index) {
            if (tag == tags[index]) return indices[index];
        }
    }
    return -1;
}

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
    std::array<jobject, 48U> m_references{};
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
        return false;
    }

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
    jclass listClass = nullptr;
    if (!lookupRequired(env, listClass,
                        [&] { return env->FindClass("java/util/List"); })) {
        return false;
    }
    localReferences.add(listClass);
    if (!lookupRequired(env, candidate.listSize,
                        [&] { return env->GetMethodID(listClass, "size", "()I"); }) ||
        !lookupRequired(env, candidate.listGet,
                        [&] { return env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;"); }) ||
        !lookupRequired(env, candidate.listToArray,
                        [&] { return env->GetMethodID(listClass, "toArray", "()[Ljava/lang/Object;"); }) ||
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

bool GameBindings::setLwjglMouseGrabbed(JNIEnv* const env, const bool grabbed) noexcept
{
    if (env == nullptr) {
        return false;
    }
    if (m_lwjglMouseClass == nullptr || m_lwjglSetGrabbed == nullptr) {
        jclass localMouse = env->FindClass("org/lwjgl/input/Mouse");
        if (env->ExceptionCheck() == JNI_TRUE || localMouse == nullptr) {
            clearException(env);
            return false;
        }
        jmethodID setGrabbed = env->GetStaticMethodID(localMouse, "setGrabbed", "(Z)V");
        if (env->ExceptionCheck() == JNI_TRUE || setGrabbed == nullptr) {
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
    }
    env->CallStaticVoidMethod(m_lwjglMouseClass, m_lwjglSetGrabbed,
                              grabbed ? JNI_TRUE : JNI_FALSE);
    const bool succeeded = env->ExceptionCheck() != JNI_TRUE;
    clearException(env);
    return succeeded;
}

bool GameBindings::setInputCaptured(JNIEnv* const env, const bool guiOpen) noexcept
{
    if (env == nullptr) return false;

    bool minecraftFocusChanged = false;
    if (m_resolutionPhase.load(std::memory_order_acquire) == ResolutionPhase::Resolved &&
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
    const bool lwjglChanged = setLwjglMouseGrabbed(env, !guiOpen);
    if (guiOpen) {
        ::ReleaseCapture();
        ::ClipCursor(nullptr);
        ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    }
    return minecraftFocusChanged || lwjglChanged;
}

bool GameBindings::maintainInputReleased(JNIEnv* const env) noexcept
{
    if (env == nullptr) return false;
    const bool released = setLwjglMouseGrabbed(env, false);
    ::ReleaseCapture();
    ::ClipCursor(nullptr);
    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
    return released;
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

                        BedDefenseBlock* summary = nullptr;
                        for (std::size_t index = 0U; index < enriched.defenseCount; ++index) {
                            BedDefenseBlock& candidate = enriched.defense[index];
                            if (candidate.blockId == sample.blockId &&
                                candidate.metadata == sample.metadata) {
                                summary = &candidate;
                                break;
                            }
                        }
                        if (summary == nullptr &&
                            enriched.defenseCount < enriched.defense.size()) {
                            summary = &enriched.defense[enriched.defenseCount++];
                            summary->blockId = sample.blockId;
                            summary->metadata = sample.metadata;
                        }
                        if (summary != nullptr) {
                            std::uint16_t& count = summary->ringCounts[
                                static_cast<std::size_t>(ring)];
                            if (count != std::numeric_limits<std::uint16_t>::max()) ++count;
                        }
                    }
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
        if (now - lastVerification >= 1000U) {
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

        publish();
        env->PopLocalFrame(nullptr);
        if (stopRequested(500U)) break;
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

    jobject player = env->GetObjectField(minecraft, cache->playerField);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    jobject world = env->GetObjectField(minecraft, cache->worldField);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    if (player == nullptr || world == nullptr) {
        if (m_snapshot.matchActive || m_snapshot.playerCount != 0U ||
            m_snapshot.localPlayerName[0U] != '\0') {
            m_snapshot.matchActive = false;
            m_snapshot.playerCount = 0U;
            m_snapshot.players = {};
            m_snapshot.localPlayerName = {};
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
        }
        env->PopLocalFrame(nullptr);
        m_snapshot.state = GameSnapshot::State::NoPlayer;
        return m_snapshot;
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
    const jboolean singlePlayer = JNI_TRUE;
    if (env->ExceptionCheck() == JNI_TRUE) return failJni();
    if (!std::isfinite(health) || !std::isfinite(maxHealth)) return failJni();

    m_snapshot.singlePlayer = singlePlayer == JNI_TRUE;

    auto readEntityMarker = [&](jobject const entity, EntityMarker& marker) noexcept {
        marker = {};
        marker.entityId = env->CallIntMethod(entity, cache->getEntityId);
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            return false;
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
               std::isfinite(marker.bounds.minX) && std::isfinite(marker.bounds.minY) &&
               std::isfinite(marker.bounds.minZ) && std::isfinite(marker.bounds.maxX) &&
               std::isfinite(marker.bounds.maxY) && std::isfinite(marker.bounds.maxZ);
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
                if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
                if (!isLocalPlayer && isLiving) {
                    EntityMarker marker;
                    if (readEntityMarker(entity, marker)) {
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
    m_snapshot.entitySampleGeneration = ++m_entitySampleGeneration;

    // Multiplayer discovery is metadata-only and independent of ESP. Scan at
    // 1 Hz, extract the stable username and the first scoreboard color code
    // from the formatted display component, then publish only roster changes.
    if (tickMilliseconds - m_lastPlayerScan >= 1000U || m_lastPlayerScan == 0U) {
        m_lastPlayerScan = tickMilliseconds;
        std::array<PlayerIdentity, GameSnapshot::MaxDiscoveredPlayers> nextPlayers{};
        std::array<char, 17U> nextLocalName{};
        std::uint32_t nextCount = 0U;
        std::uint32_t taggedPlayers = 0U;
        unsigned teamMask = 0U;
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
        jobject playerList = env->GetObjectField(world, cache->playerEntities);
        jobjectArray playerArray = playerList == nullptr ? nullptr : static_cast<jobjectArray>(
            env->CallObjectMethod(playerList, cache->listToArray));
        if (env->ExceptionCheck() == JNI_TRUE) {
            env->ExceptionClear();
            playerArray = nullptr;
        }
        if (playerArray != nullptr) {
            const jsize count = std::min<jsize>(env->GetArrayLength(playerArray),
                                                static_cast<jsize>(nextPlayers.size()));
            for (jsize index = 0; index < count; ++index) {
                jobject remotePlayer = env->GetObjectArrayElement(playerArray, index);
                if (env->ExceptionCheck() == JNI_TRUE) {
                    env->ExceptionClear();
                    continue;
                }
                if (remotePlayer == nullptr) continue;
                jstring name = static_cast<jstring>(
                    env->CallObjectMethod(remotePlayer, cache->getName));
                jobject display = env->CallObjectMethod(remotePlayer, cache->getDisplayName);
                jstring formatted = display == nullptr ? nullptr : static_cast<jstring>(
                    env->CallObjectMethod(display, cache->getFormattedText));
                if (env->ExceptionCheck() == JNI_TRUE || name == nullptr) {
                    env->ExceptionClear();
                } else {
                    const char* const nameUtf8 = env->GetStringUTFChars(name, nullptr);
                    const char* const formattedUtf8 = formatted == nullptr ? nullptr :
                        env->GetStringUTFChars(formatted, nullptr);
                    if (env->ExceptionCheck() != JNI_TRUE && nameUtf8 != nullptr) {
                        const std::string_view nameView(nameUtf8);
                        const bool validName = !nameView.empty() && nameView.size() <= 16U &&
                            std::all_of(nameView.begin(), nameView.end(), [](const char c) noexcept {
                                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                       (c >= '0' && c <= '9') || c == '_';
                            });
                        if (validName && nextCount < nextPlayers.size()) {
                            PlayerIdentity identity{};
                            std::copy(nameView.begin(), nameView.end(), identity.name.begin());
                            identity.teamColor = 'f';
                            if (formattedUtf8 != nullptr) {
                                const std::string_view formattedView(formattedUtf8);
                                const int teamIndex = bedWarsTeamIndex(formattedView);
                                if (teamIndex >= 0) {
                                    ++taggedPlayers;
                                    teamMask |= 1U << static_cast<unsigned>(teamIndex);
                                }
                                for (std::size_t offset = 0U; offset + 2U < formattedView.size(); ++offset) {
                                    if (static_cast<unsigned char>(formattedView[offset]) == 0xC2U &&
                                        static_cast<unsigned char>(formattedView[offset + 1U]) == 0xA7U) {
                                        const char code = static_cast<char>(std::tolower(
                                            static_cast<unsigned char>(formattedView[offset + 2U])));
                                        if ((code >= '0' && code <= '9') ||
                                            (code >= 'a' && code <= 'f')) {
                                            identity.teamColor = code;
                                            break;
                                        }
                                    }
                                }
                            }
                            nextPlayers[nextCount++] = identity;
                        }
                    }
                    if (formattedUtf8 != nullptr) env->ReleaseStringUTFChars(formatted, formattedUtf8);
                    if (nameUtf8 != nullptr) env->ReleaseStringUTFChars(name, nameUtf8);
                    if (env->ExceptionCheck() == JNI_TRUE) env->ExceptionClear();
                }
                if (formatted != nullptr) env->DeleteLocalRef(formatted);
                if (display != nullptr) env->DeleteLocalRef(display);
                if (name != nullptr) env->DeleteLocalRef(name);
                env->DeleteLocalRef(remotePlayer);
            }
            env->DeleteLocalRef(playerArray);
        }
        if (playerList != nullptr) env->DeleteLocalRef(playerList);
        std::sort(nextPlayers.begin(), nextPlayers.begin() + nextCount,
                  [](const PlayerIdentity& first, const PlayerIdentity& second) noexcept {
                      return std::strcmp(first.name.data(), second.name.data()) < 0;
                  });
        bool rosterChanged = nextCount != m_snapshot.playerCount;
        for (std::uint32_t index = 0U; !rosterChanged && index < nextCount; ++index) {
            rosterChanged = std::strcmp(nextPlayers[index].name.data(),
                                        m_snapshot.players[index].name.data()) != 0 ||
                            nextPlayers[index].teamColor != m_snapshot.players[index].teamColor;
        }
        const bool nextMatchActive = taggedPlayers >= 2U &&
            teamMask != 0U && (teamMask & (teamMask - 1U)) != 0U;
        const bool statusChanged = nextMatchActive != m_snapshot.matchActive ||
            std::strcmp(nextLocalName.data(), m_snapshot.localPlayerName.data()) != 0;
        if (rosterChanged || statusChanged) {
            m_snapshot.players = nextPlayers;
            m_snapshot.playerCount = nextCount;
            m_snapshot.localPlayerName = nextLocalName;
            m_snapshot.matchActive = nextMatchActive;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
        }
    }

    // The scanner thread publishes immutable fixed storage. SwapBuffers only
    // takes a short shared SRW lock and performs no block JNI calls.
    if (singlePlayer == JNI_TRUE) {
        ::AcquireSRWLockShared(&m_bedCacheLock);
        m_snapshot.bedMarkerCount = m_publishedBedCache.markerCount;
        m_snapshot.bedCount = m_publishedBedCache.markerCount;
        std::copy_n(m_publishedBedCache.markers.begin(),
                    m_publishedBedCache.markerCount, m_snapshot.bedMarkers.begin());
        m_snapshot.bedScanProgress = m_publishedBedCache.generation == 0U ? 0.0F : 1.0F;
        ::ReleaseSRWLockShared(&m_bedCacheLock);
    } else {
        m_snapshot.bedCount = 0U;
        m_snapshot.bedMarkerCount = 0U;
        m_snapshot.bedScanProgress = 0.0F;
    }

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
    if (cache.chatComponentClass != nullptr) env->DeleteGlobalRef(cache.chatComponentClass);
    if (cache.renderManagerObject != nullptr) env->DeleteGlobalRef(cache.renderManagerObject);
    if (cache.timerObject != nullptr) env->DeleteGlobalRef(cache.timerObject);
    if (cache.modelViewBuffer != nullptr) env->DeleteGlobalRef(cache.modelViewBuffer);
    if (cache.projectionBuffer != nullptr) env->DeleteGlobalRef(cache.projectionBuffer);
    if (cache.viewportBuffer != nullptr) env->DeleteGlobalRef(cache.viewportBuffer);

    cache.minecraftClass = nullptr;
    cache.playerClass = nullptr;
    cache.livingClass = nullptr;
    cache.entityClass = nullptr;
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
    cache.chatComponentClass = nullptr;
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
    m_resolutionPhase.store(ResolutionPhase::Stopped, std::memory_order_release);
    if (env != nullptr && m_cache != nullptr) {
        deleteGlobalRefs(env, *m_cache);
    }
    m_cache.reset();
    m_snapshot = {};
    m_mappingAttempt.store(0U, std::memory_order_relaxed);
    m_retryAtMilliseconds.store(0U, std::memory_order_relaxed);
    m_lastSample = 0U;
    m_entitySampleGeneration = 0U;
    m_lastPlayerScan = 0U;
    m_playerRosterGeneration = 0U;
    m_bedRescanRequested.store(false, std::memory_order_relaxed);
    ::AcquireSRWLockExclusive(&m_bedCacheLock);
    m_publishedBedCache = {};
    ::ReleaseSRWLockExclusive(&m_bedCacheLock);
    if (env != nullptr && m_lwjglMouseClass != nullptr) {
        env->DeleteGlobalRef(m_lwjglMouseClass);
    }
    m_lwjglMouseClass = nullptr;
    m_lwjglSetGrabbed = nullptr;
}

void GameBindings::abandon() noexcept
{
    // Used only when the JVM is already shutting down and no JNIEnv can be
    // obtained. The VM owns and releases its reference table at process exit.
    m_resolutionPhase.store(ResolutionPhase::Stopped, std::memory_order_release);
    m_cache.reset();
    m_lwjglMouseClass = nullptr;
    m_lwjglSetGrabbed = nullptr;
}

} // namespace mcoverlay
