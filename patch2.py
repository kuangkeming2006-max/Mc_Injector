import sys

# Update MappingProvider.h
with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/MappingProvider.h', 'r', encoding='utf-8') as f:
    content = f.read()

target_h = '''    std::string getScoreboard;
    std::string getObjectiveInDisplaySlot;
    std::string getPlayersTeam;'''
rep_h = '''    std::string getScoreboard;
    std::string getObjectiveInDisplaySlot;
    std::string getPlayersTeam;
    std::string getSortedScores;
    std::string getPlayerName;
    std::string formatPlayerName;'''
content = content.replace(target_h, rep_h)
with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/MappingProvider.h', 'w', encoding='utf-8') as f:
    f.write(content)

# Update MappingProvider.cpp
with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/MappingProvider.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

target_cpp1 = '''    mapping.getObjectiveInDisplaySlot = "func_96539_a";
    mapping.getPlayersTeam = "func_96509_i";'''
rep_cpp1 = '''    mapping.getObjectiveInDisplaySlot = "func_96539_a";
    mapping.getPlayersTeam = "func_96509_i";
    mapping.getSortedScores = "func_96534_i";
    mapping.getPlayerName = "func_96653_e";
    mapping.formatPlayerName = "func_96667_a";'''
content = content.replace(target_cpp1, rep_cpp1)

target_cpp2 = '''    mapping.getObjectiveInDisplaySlot = "getObjectiveInDisplaySlot";
    mapping.getPlayersTeam = "getPlayersTeam";'''
rep_cpp2 = '''    mapping.getObjectiveInDisplaySlot = "getObjectiveInDisplaySlot";
    mapping.getPlayersTeam = "getPlayersTeam";
    mapping.getSortedScores = "getSortedScores";
    mapping.getPlayerName = "getPlayerName";
    mapping.formatPlayerName = "formatPlayerName";'''
content = content.replace(target_cpp2, rep_cpp2)

target_cpp3 = '''        loadedEntitiesField, getScoreboard, getObjectiveInDisplaySlot,
        getPlayersTeam, inventoryField, armorInventoryField,'''
rep_cpp3 = '''        loadedEntitiesField, getScoreboard, getObjectiveInDisplaySlot,
        getPlayersTeam, getSortedScores, getPlayerName, formatPlayerName,
        inventoryField, armorInventoryField,'''
content = content.replace(target_cpp3, rep_cpp3)

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/MappingProvider.cpp', 'w', encoding='utf-8') as f:
    f.write(content)

# Update GameBindings.cpp (BindingCache & resolveProfile)
with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

target_cache = '''    jmethodID getScoreboard = nullptr;
    jmethodID getObjectiveInDisplaySlot = nullptr;
    jmethodID getPlayersTeam = nullptr;'''
rep_cache = '''    jmethodID getScoreboard = nullptr;
    jmethodID getObjectiveInDisplaySlot = nullptr;
    jmethodID getPlayersTeam = nullptr;
    jmethodID getSortedScores = nullptr;
    jmethodID getPlayerName = nullptr;
    jmethodID formatPlayerName = nullptr;'''
content = content.replace(target_cache, rep_cache)

target_resolve = '''    const std::string getPlayersTeamSignature = std::string("(Ljava/lang/String;)") + profile.scorePlayerTeamSignature;
    const std::string getItemSignature = std::string("()") + profile.itemSignature;'''
rep_resolve = '''    const std::string getPlayersTeamSignature = std::string("(Ljava/lang/String;)") + profile.scorePlayerTeamSignature;
    const std::string getSortedScoresSignature = std::string("(") + profile.scoreObjectiveSignature + ")Ljava/util/Collection;";
    const std::string formatPlayerNameSignature = std::string("(") + profile.scorePlayerTeamSignature + "Ljava/lang/String;)Ljava/lang/String;";
    const std::string getItemSignature = std::string("()") + profile.itemSignature;'''
content = content.replace(target_resolve, rep_resolve)

target_lookup = '''        !lookupRequired(env, candidate.getPlayersTeam, [&] {
            return env->GetMethodID(scoreboard, profile.getPlayersTeam.c_str(), getPlayersTeamSignature.c_str());
        }) ||'''
rep_lookup = '''        !lookupRequired(env, candidate.getPlayersTeam, [&] {
            return env->GetMethodID(scoreboard, profile.getPlayersTeam.c_str(), getPlayersTeamSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getSortedScores, [&] {
            return env->GetMethodID(scoreboard, profile.getSortedScores.c_str(), getSortedScoresSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getPlayerName, [&] {
            return env->GetMethodID(score, profile.getPlayerName.c_str(), "()Ljava/lang/String;");
        }) ||
        !lookupRequired(env, candidate.formatPlayerName, [&] {
            return env->GetStaticMethodID(scorePlayerTeam, profile.formatPlayerName.c_str(), formatPlayerNameSignature.c_str());
        }) ||'''
content = content.replace(target_lookup, rep_lookup)

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'w', encoding='utf-8') as f:
    f.write(content)
