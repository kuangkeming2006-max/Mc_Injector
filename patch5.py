import sys

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

target = '''    jobject world = env->GetObjectField(minecraft, cache->worldField);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    if (player == nullptr || world == nullptr) {
        if (m_snapshot.matchActive || m_snapshot.playerCount != 0U ||
            m_snapshot.entityMarkerCount != 0U || m_snapshot.bedMarkerCount != 0U) {
            m_snapshot.matchActive = false;
            m_snapshot.ownTeam = 'u';
            m_snapshot.playerCount = 0U;
            m_snapshot.entityMarkerCount = 0U;
            m_snapshot.bedMarkerCount = 0U;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
            m_snapshot.entitySampleGeneration = ++m_entitySampleGeneration;
        }
        if (player != nullptr) env->DeleteLocalRef(player);
        if (world != nullptr) env->DeleteLocalRef(world);
        return;
    }'''

rep = '''    jobject world = env->GetObjectField(minecraft, cache->worldField);
    if (env->ExceptionCheck() == JNI_TRUE) {
        return failJni();
    }
    
    bool worldChanged = false;
    if (world != nullptr) {
        if (m_lastWorld == nullptr || !env->IsSameObject(world, m_lastWorld)) {
            worldChanged = true;
            if (m_lastWorld != nullptr) {
                env->DeleteWeakGlobalRef(m_lastWorld);
            }
            m_lastWorld = env->NewWeakGlobalRef(world);
        }
    } else {
        if (m_lastWorld != nullptr) {
            env->DeleteWeakGlobalRef(m_lastWorld);
            m_lastWorld = nullptr;
        }
    }

    if (player == nullptr || world == nullptr || worldChanged) {
        if (m_snapshot.matchActive || m_snapshot.playerCount != 0U ||
            m_snapshot.entityMarkerCount != 0U || m_snapshot.bedMarkerCount != 0U || worldChanged) {
            m_snapshot.matchActive = false;
            m_snapshot.ownTeam = 'u';
            m_snapshot.playerCount = 0U;
            m_snapshot.entityMarkerCount = 0U;
            m_snapshot.bedMarkerCount = 0U;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
            m_snapshot.entitySampleGeneration = ++m_entitySampleGeneration;
        }
        if (player == nullptr || world == nullptr) {
            if (player != nullptr) env->DeleteLocalRef(player);
            if (world != nullptr) env->DeleteLocalRef(world);
            return;
        }
    }'''

content = content.replace(target, rep)

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'w', encoding='utf-8') as f:
    f.write(content)
