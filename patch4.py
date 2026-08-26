import sys

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/overlay_renderer.cpp', 'r', encoding='utf-8') as f:
    content = f.read()

target_esp = '''                char label[64]{};
                if (m_features.labelsEnabled) {
                    std::snprintf(label, sizeof(label), "Entity #%d  %.1fm",
                                  entity.entityId, entity.distance);
                }
                drawProjectedBox(background, snapshot.camera, displaySize, interpolated,
                                 IM_COL32(108, 196, 255, 255), label);'''

rep_esp = '''                char label[64]{};
                if (m_features.labelsEnabled) {
                    std::snprintf(label, sizeof(label), "Entity #%d  %.1fm",
                                  entity.entityId, entity.distance);
                }
                
                bool isTeammate = snapshot.ownTeam != 'u' && entity.armorTeam == snapshot.ownTeam;
                if (isTeammate) {
                    // Draw a green box instead of an arrow for simplicity
                    drawProjectedBox(background, snapshot.camera, displaySize, interpolated,
                                     IM_COL32(0, 255, 0, 255), label);
                } else {
                    // Enemies or unknown threats
                    drawProjectedBox(background, snapshot.camera, displaySize, interpolated,
                                     IM_COL32(255, 0, 0, 255), label);
                }'''
content = content.replace(target_esp, rep_esp)

with open('e:/For_Minecraft/Mc_Injector-master/Mc_Injector-master/agent/overlay_renderer.cpp', 'w', encoding='utf-8') as f:
    f.write(content)
