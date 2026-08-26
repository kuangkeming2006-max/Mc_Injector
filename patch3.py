import sys

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

target = '''        const bool nextMatchActive = taggedPlayers >= 2U &&
            teamMask != 0U && (teamMask & (teamMask - 1U)) != 0U;
        const bool statusChanged = nextMatchActive != m_snapshot.matchActive ||
            std::strcmp(nextLocalName.data(), m_snapshot.localPlayerName.data()) != 0;
        if (rosterChanged || statusChanged) {
            m_snapshot.players = nextPlayers;
            m_snapshot.playerCount = nextCount;
            m_snapshot.localPlayerName = nextLocalName;
            m_snapshot.matchActive = nextMatchActive;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
        }'''

rep = '''        bool nextMatchActive = taggedPlayers >= 2U &&
            teamMask != 0U && (teamMask & (teamMask - 1U)) != 0U;
        
        char nextOwnTeam = 'u';
        jobject scoreboard = cache->getScoreboard != nullptr ? env->CallObjectMethod(world, cache->getScoreboard) : nullptr;
        if (env->ExceptionCheck() != JNI_TRUE && scoreboard != nullptr) {
            jobject objective = env->CallObjectMethod(scoreboard, cache->getObjectiveInDisplaySlot, 1);
            if (env->ExceptionCheck() != JNI_TRUE && objective != nullptr) {
                jobject scores = env->CallObjectMethod(scoreboard, cache->getSortedScores, objective);
                if (env->ExceptionCheck() != JNI_TRUE && scores != nullptr) {
                    jobjectArray scoresArray = static_cast<jobjectArray>(env->CallObjectMethod(scores, cache->listToArray));
                    if (env->ExceptionCheck() != JNI_TRUE && scoresArray != nullptr) {
                        jsize len = env->GetArrayLength(scoresArray);
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
                                                std::string_view fv(formattedUtf8);
                                                if (fv.find(" YOU") != std::string_view::npos) {
                                                    const int ti = bedWarsTeamIndex(fv);
                                                    if (ti >= 0) {
                                                        const char teams[] = "RBGYAWP7";
                                                        if (ti < 8) nextOwnTeam = teams[ti];
                                                    }
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

        const bool statusChanged = nextMatchActive != m_snapshot.matchActive || nextOwnTeam != m_snapshot.ownTeam ||
            std::strcmp(nextLocalName.data(), m_snapshot.localPlayerName.data()) != 0;
        if (rosterChanged || statusChanged) {
            m_snapshot.players = nextPlayers;
            m_snapshot.playerCount = nextCount;
            m_snapshot.localPlayerName = nextLocalName;
            m_snapshot.matchActive = nextMatchActive;
            m_snapshot.ownTeam = nextOwnTeam;
            m_snapshot.playerRosterGeneration = ++m_playerRosterGeneration;
        }'''
content = content.replace(target, rep)
with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'w', encoding='utf-8') as f:
    f.write(content)
