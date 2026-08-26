import sys

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

# patch 1: class loading
target1 = '''        !loadClass(timer, profile.timerName.c_str()) ||
        !loadClass(chatComponent, profile.chatComponentName.c_str())) {'''
replacement1 = '''        !loadClass(timer, profile.timerName.c_str()) ||
        !loadClass(chatComponent, profile.chatComponentName.c_str()) ||
        !loadClass(scoreboard, profile.scoreboardName.c_str()) ||
        !loadClass(scoreObjective, profile.scoreObjectiveName.c_str()) ||
        !loadClass(score, profile.scoreName.c_str()) ||
        !loadClass(scorePlayerTeam, profile.scorePlayerTeamName.c_str()) ||
        !loadClass(itemStack, profile.itemStackName.c_str()) ||
        !loadClass(item, profile.itemName.c_str()) ||
        !loadClass(itemArmor, profile.itemArmorName.c_str()) ||
        !loadClass(inventoryPlayer, profile.inventoryPlayerName.c_str())) {'''

content = content.replace(target1, replacement1)

target_decl = '''    jclass timer = nullptr;
    jclass chatComponent = nullptr;'''
rep_decl = '''    jclass timer = nullptr;
    jclass chatComponent = nullptr;
    jclass scoreboard = nullptr;
    jclass scoreObjective = nullptr;
    jclass score = nullptr;
    jclass scorePlayerTeam = nullptr;
    jclass itemStack = nullptr;
    jclass item = nullptr;
    jclass itemArmor = nullptr;
    jclass inventoryPlayer = nullptr;'''
content = content.replace(target_decl, rep_decl)

target_methods = '''    const std::string getRenderManagerSignature =
        std::string("()") + profile.renderManagerSignature;'''
rep_methods = '''    const std::string getRenderManagerSignature =
        std::string("()") + profile.renderManagerSignature;

    const std::string getObjectiveInDisplaySlotSignature = std::string("(I)") + profile.scoreObjectiveSignature;
    const std::string getPlayersTeamSignature = std::string("(Ljava/lang/String;)") + profile.scorePlayerTeamSignature;
    const std::string getItemSignature = std::string("()") + profile.itemSignature;'''
content = content.replace(target_methods, rep_methods)

target_resolves = '''        !lookupRequired(env, candidate.renderPartialTicks, [&] {
            return env->GetFieldID(timer, profile.renderPartialTicksField.c_str(), "F");
        })) {
        return false;
    }'''

rep_resolves = '''        !lookupRequired(env, candidate.renderPartialTicks, [&] {
            return env->GetFieldID(timer, profile.renderPartialTicksField.c_str(), "F");
        }) ||
        !lookupRequired(env, candidate.getScoreboard, [&] {
            return env->GetMethodID(world, profile.getScoreboard.c_str(), (std::string("()") + profile.scoreboardSignature).c_str());
        }) ||
        !lookupRequired(env, candidate.getObjectiveInDisplaySlot, [&] {
            return env->GetMethodID(scoreboard, profile.getObjectiveInDisplaySlot.c_str(), getObjectiveInDisplaySlotSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.getPlayersTeam, [&] {
            return env->GetMethodID(scoreboard, profile.getPlayersTeam.c_str(), getPlayersTeamSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.inventoryField, [&] {
            return env->GetFieldID(player, profile.inventoryField.c_str(), profile.inventoryPlayerSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.armorInventoryField, [&] {
            return env->GetFieldID(inventoryPlayer, profile.armorInventoryField.c_str(), (std::string("[") + profile.itemStackSignature).c_str());
        }) ||
        !lookupRequired(env, candidate.getItem, [&] {
            return env->GetMethodID(itemStack, profile.getItem.c_str(), getItemSignature.c_str());
        }) ||
        !lookupRequired(env, candidate.hasColor, [&] {
            return env->GetMethodID(itemArmor, profile.hasColor.c_str(), (std::string("(") + profile.itemStackSignature + ")Z").c_str());
        }) ||
        !lookupRequired(env, candidate.getColor, [&] {
            return env->GetMethodID(itemArmor, profile.getColor.c_str(), (std::string("(") + profile.itemStackSignature + ")I").c_str());
        })) {
        return false;
    }'''
content = content.replace(target_resolves, rep_resolves)

target_global = '''        !makeGlobal(timer, candidate.timerClass) ||
        !makeGlobal(chatComponent, candidate.chatComponentClass)) {'''
rep_global = '''        !makeGlobal(timer, candidate.timerClass) ||
        !makeGlobal(chatComponent, candidate.chatComponentClass) ||
        !makeGlobal(scoreboard, candidate.scoreboardClass) ||
        !makeGlobal(scoreObjective, candidate.scoreObjectiveClass) ||
        !makeGlobal(score, candidate.scoreClass) ||
        !makeGlobal(scorePlayerTeam, candidate.scorePlayerTeamClass) ||
        !makeGlobal(itemStack, candidate.itemStackClass) ||
        !makeGlobal(item, candidate.itemClass) ||
        !makeGlobal(itemArmor, candidate.itemArmorClass) ||
        !makeGlobal(inventoryPlayer, candidate.inventoryPlayerClass)) {'''
content = content.replace(target_global, rep_global)

target_delglobal1 = '''    if (cache.timerClass != nullptr) env->DeleteGlobalRef(cache.timerClass);
    if (cache.chatComponentClass != nullptr) env->DeleteGlobalRef(cache.chatComponentClass);'''
rep_delglobal1 = '''    if (cache.timerClass != nullptr) env->DeleteGlobalRef(cache.timerClass);
    if (cache.chatComponentClass != nullptr) env->DeleteGlobalRef(cache.chatComponentClass);
    if (cache.scoreboardClass != nullptr) env->DeleteGlobalRef(cache.scoreboardClass);
    if (cache.scoreObjectiveClass != nullptr) env->DeleteGlobalRef(cache.scoreObjectiveClass);
    if (cache.scoreClass != nullptr) env->DeleteGlobalRef(cache.scoreClass);
    if (cache.scorePlayerTeamClass != nullptr) env->DeleteGlobalRef(cache.scorePlayerTeamClass);
    if (cache.itemStackClass != nullptr) env->DeleteGlobalRef(cache.itemStackClass);
    if (cache.itemClass != nullptr) env->DeleteGlobalRef(cache.itemClass);
    if (cache.itemArmorClass != nullptr) env->DeleteGlobalRef(cache.itemArmorClass);
    if (cache.inventoryPlayerClass != nullptr) env->DeleteGlobalRef(cache.inventoryPlayerClass);'''
content = content.replace(target_delglobal1, rep_delglobal1)

target_delglobal2 = '''    cache.timerClass = nullptr;
    cache.chatComponentClass = nullptr;'''
rep_delglobal2 = '''    cache.timerClass = nullptr;
    cache.chatComponentClass = nullptr;
    cache.scoreboardClass = nullptr;
    cache.scoreObjectiveClass = nullptr;
    cache.scoreClass = nullptr;
    cache.scorePlayerTeamClass = nullptr;
    cache.itemStackClass = nullptr;
    cache.itemClass = nullptr;
    cache.itemArmorClass = nullptr;
    cache.inventoryPlayerClass = nullptr;'''
content = content.replace(target_delglobal2, rep_delglobal2)

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/bindings/GameBindings.cpp', 'w', encoding='utf-8') as f:
    f.write(content)
