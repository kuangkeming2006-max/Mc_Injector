#include "overlay_renderer.h"

#include "src/AgentLog.h"

#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>

#include <gl/GL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "bed_png.h"
#include "block_textures.h"

// Dear ImGui intentionally keeps this declaration inside `#if 0` in the
// backend header so including it does not force windows.h on every consumer.
// This translation unit already includes Win32 types through our headers.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace mcoverlay {

struct OverlayInputState final {
    std::atomic<bool> interactive{false};
    std::atomic<bool> clickGuiToggle{false};
    std::atomic<unsigned> menuHotkey{VK_OEM_7};
    std::atomic<bool> acceptImGuiMessages{false};
    std::atomic<bool> windowProcedureAvailable{false};
    // Calling the ImGui Win32 backend is only legal when WndProc and OpenGL
    // share a thread. A Lunar split-thread window still installs the hook for
    // suppression/hotkeys, while the render thread feeds ImGui by polling.
    std::atomic<bool> directImGuiWndProc{false};
    std::atomic<bool> captureHotkey{false};
    std::atomic<unsigned> capturedHotkey{0U};
    std::atomic<HWND> window{nullptr};
    std::atomic<ImGuiContext*> imguiContext{nullptr};
    bool fallbackPrimed = false;
    bool menuKeyDown = false;
    bool escapeKeyDown = false;
    bool mouseDown[5]{};
    bool captureKeysPrimed = false;
    std::array<bool, 256U> keyDown{};
};

namespace {

struct ScreenPoint final { float x = 0.0F; float y = 0.0F; bool visible = false; };

ScreenPoint projectPoint(const WorldCameraSnapshot& camera,
                         const ImVec2 displaySize,
                         const double x, const double y, const double z) noexcept
{
    const auto multiply = [](const std::array<float, 16U>& matrix,
                             const std::array<double, 4U>& input) noexcept {
        std::array<double, 4U> output{};
        for (std::size_t row = 0U; row < 4U; ++row) {
            output[row] = matrix[row] * input[0U] +
                          matrix[4U + row] * input[1U] +
                          matrix[8U + row] * input[2U] +
                          matrix[12U + row] * input[3U];
        }
        return output;
    };
    // Minecraft 1.8.9 renders world geometry relative to RenderManager's
    // renderPos*. ActiveRenderInfo's MODELVIEW/PROJECTION buffers contain the
    // real current 3D camera transform (yaw/pitch, FOV, hurt effect and view
    // bobbing), but intentionally do not contain the large world translation.
    const auto eye = multiply(camera.modelView,
                              {x - camera.renderX, y - camera.renderY,
                               z - camera.renderZ, 1.0});
    const auto clip = multiply(camera.projection, eye);
    if (!std::isfinite(clip[3U]) || clip[3U] <= 0.001) return {};
    const double ndcX = clip[0U] / clip[3U];
    const double ndcY = clip[1U] / clip[3U];
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return {};
    const float viewportX = static_cast<float>(camera.viewport[0U]);
    const float viewportY = static_cast<float>(camera.viewport[1U]);
    const float viewportWidth = static_cast<float>(camera.viewport[2U]);
    const float viewportHeight = static_cast<float>(camera.viewport[3U]);
    ScreenPoint result;
    result.x = viewportX + static_cast<float>((ndcX + 1.0) * 0.5) * viewportWidth;
    const float openGlY = viewportY + static_cast<float>((ndcY + 1.0) * 0.5) * viewportHeight;
    result.y = displaySize.y - openGlY;
    result.visible = result.x > -viewportWidth && result.x < displaySize.x + viewportWidth &&
                     result.y > -viewportHeight && result.y < displaySize.y + viewportHeight;
    return result;
}

void drawProjectedBox(ImDrawList* const drawList,
                      const WorldCameraSnapshot& camera,
                      const ImVec2 displaySize,
                      const AxisAlignedBox& box,
                      const ImU32 color,
                      const char* const label,
                      const bool filled = false) noexcept
{
    if (drawList == nullptr || !camera.valid) return;
    const std::array<std::array<double, 3U>, 8U> corners{{
        {{box.minX, box.minY, box.minZ}}, {{box.maxX, box.minY, box.minZ}},
        {{box.maxX, box.minY, box.maxZ}}, {{box.minX, box.minY, box.maxZ}},
        {{box.minX, box.maxY, box.minZ}}, {{box.maxX, box.maxY, box.minZ}},
        {{box.maxX, box.maxY, box.maxZ}}, {{box.minX, box.maxY, box.maxZ}}}};
    std::array<ScreenPoint, 8U> projected{};
    bool anyVisible = false;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected[index] = projectPoint(camera, displaySize,
                                        corners[index][0U], corners[index][1U], corners[index][2U]);
        anyVisible = anyVisible || projected[index].visible;
    }
    if (!anyVisible) return;
    if (filled) {
        constexpr std::array<std::array<std::uint8_t, 4U>, 6U> faces{{
            {{0, 1, 2, 3}}, {{4, 5, 6, 7}}, {{0, 1, 5, 4}},
            {{1, 2, 6, 5}}, {{2, 3, 7, 6}}, {{3, 0, 4, 7}}}};
        const ImU32 fillColor = (color & ~IM_COL32_A_MASK) |
                                (42U << IM_COL32_A_SHIFT);
        for (const auto& face : faces) {
            std::array<ImVec2, 4U> points{};
            bool complete = true;
            for (std::size_t point = 0U; point < face.size(); ++point) {
                const ScreenPoint& projectedPoint = projected[face[point]];
                if (!projectedPoint.visible) {
                    complete = false;
                    break;
                }
                points[point] = ImVec2(projectedPoint.x, projectedPoint.y);
            }
            if (complete) drawList->AddConvexPolyFilled(points.data(), 4, fillColor);
        }
    }
    constexpr std::array<std::array<std::uint8_t, 2U>, 12U> edges{{
        {{0,1}}, {{1,2}}, {{2,3}}, {{3,0}}, {{4,5}}, {{5,6}},
        {{6,7}}, {{7,4}}, {{0,4}}, {{1,5}}, {{2,6}}, {{3,7}}}};
    for (const auto& edge : edges) {
        const ScreenPoint& first = projected[edge[0U]];
        const ScreenPoint& second = projected[edge[1U]];
        if (first.visible && second.visible) {
            drawList->AddLine(ImVec2(first.x, first.y), ImVec2(second.x, second.y),
                              color, 1.8F);
        }
    }
    if (label != nullptr && label[0] != '\0') {
        float left = displaySize.x;
        float top = displaySize.y;
        bool found = false;
        for (const ScreenPoint& point : projected) {
            if (!point.visible) continue;
            left = std::min(left, point.x);
            top = std::min(top, point.y);
            found = true;
        }
        if (found) {
            const ImVec2 size = ImGui::CalcTextSize(label);
            const ImVec2 start(left, std::max(2.0F, top - size.y - 5.0F));
            drawList->AddRectFilled(ImVec2(start.x - 4.0F, start.y - 2.0F),
                                    ImVec2(start.x + size.x + 4.0F, start.y + size.y + 2.0F),
                                    IM_COL32(18, 16, 22, 190), 4.0F);
            drawList->AddText(start, color, label);
        }
    }
}

ImU32 packedRgbColor(const std::uint32_t rgb, const int alpha = 255) noexcept
{
    return IM_COL32(static_cast<int>((rgb >> 16U) & 0xFFU),
                    static_cast<int>((rgb >> 8U) & 0xFFU),
                    static_cast<int>(rgb & 0xFFU), alpha);
}

std::array<float, 3U> unpackRgb(const std::uint32_t rgb) noexcept
{
    return {static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(rgb & 0xFFU) / 255.0F};
}

std::uint32_t packRgb(const std::array<float, 3U>& color) noexcept
{
    const auto channel = [](const float value) noexcept {
        return static_cast<std::uint32_t>(std::lround(
            std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return (channel(color[0U]) << 16U) |
           (channel(color[1U]) << 8U) | channel(color[2U]);
}

bool isMouseMessage(const UINT message) noexcept
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
           message == WM_NCMOUSEMOVE || message == WM_NCLBUTTONDOWN ||
           message == WM_NCLBUTTONUP || message == WM_NCRBUTTONDOWN ||
           message == WM_NCRBUTTONUP || message == WM_INPUT;
}

bool isKeyboardMessage(const UINT message) noexcept
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
           message == WM_CHAR || message == WM_SYSCHAR;
}

bool animatedToggle(const char* const label, bool& value, float& animation,
                    const float uiScale) noexcept
{
    ImGui::PushID(label);
    const float height = 22.0F * uiScale;
    const float width = 42.0F * uiScale;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##toggle", ImVec2(width, height));
    bool changed = false;
    if (ImGui::IsItemClicked()) {
        value = !value;
        changed = true;
    }
    const float target = value ? 1.0F : 0.0F;
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0F, 0.05F);
    animation += (target - animation) * (1.0F - std::exp(-14.0F * dt));
    const float eased = animation * animation * (3.0F - 2.0F * animation);
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImVec4 offColor(0.26F, 0.25F, 0.30F, 1.0F);
    const ImVec4 onColor(0.55F, 0.40F, 0.94F, 1.0F);
    const ImVec4 mixedColor(
        offColor.x + (onColor.x - offColor.x) * eased,
        offColor.y + (onColor.y - offColor.y) * eased,
        offColor.z + (onColor.z - offColor.z) * eased, 1.0F);
    const ImU32 track = ImGui::GetColorU32(mixedColor);
    draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height),
                        track, height * 0.5F);
    const float knobX = start.x + 11.0F * uiScale +
                        eased * (width - 22.0F * uiScale);
    draw->AddCircleFilled(ImVec2(knobX, start.y + height * 0.5F), 8.0F * uiScale,
                          IM_COL32(246, 241, 255, 255));
    ImGui::SameLine(0.0F, 12.0F * uiScale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

const char* hypixelStateText(const HypixelOverlaySnapshot::State state) noexcept
{
    switch (state) {
    case HypixelOverlaySnapshot::State::Idle: return "Idle";
    case HypixelOverlaySnapshot::State::Loading: return "Loading";
    case HypixelOverlaySnapshot::State::Ready: return "Ready";
    case HypixelOverlaySnapshot::State::Error: return "Error";
    }
    return "Unknown";
}

float guiScaleForIndex(const int index) noexcept
{
    constexpr std::array<float, 4U> scales{1.0F, 1.25F, 1.5F, 1.75F};
    return scales[static_cast<std::size_t>(std::clamp(index, 0, 3))];
}

const char* hotkeyName(const unsigned virtualKey) noexcept
{
    switch (virtualKey) {
    case VK_OEM_7: return "Apostrophe";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_F8: return "F8";
    case VK_F9: return "F9";
    case VK_F10: return "F10";
    case VK_F11: return "F11";
    case VK_F12: return "F12";
    default: return "Custom key";
    }
}

ImVec4 teamColor(const char code) noexcept
{
    switch (code) {
    case '0': return ImVec4(0.12F, 0.12F, 0.14F, 1.0F);
    case '1': return ImVec4(0.20F, 0.28F, 0.75F, 1.0F);
    case '2': return ImVec4(0.18F, 0.68F, 0.32F, 1.0F);
    case '3': return ImVec4(0.16F, 0.70F, 0.72F, 1.0F);
    case '4': return ImVec4(0.72F, 0.22F, 0.25F, 1.0F);
    case '5': return ImVec4(0.58F, 0.28F, 0.76F, 1.0F);
    case '6': return ImVec4(0.95F, 0.63F, 0.18F, 1.0F);
    case '7': return ImVec4(0.68F, 0.68F, 0.72F, 1.0F);
    case '8': return ImVec4(0.34F, 0.34F, 0.38F, 1.0F);
    case '9': return ImVec4(0.38F, 0.52F, 1.0F, 1.0F);
    case 'a': return ImVec4(0.42F, 0.92F, 0.48F, 1.0F);
    case 'b': return ImVec4(0.38F, 0.90F, 0.95F, 1.0F);
    case 'c': return ImVec4(1.0F, 0.38F, 0.42F, 1.0F);
    case 'd': return ImVec4(0.92F, 0.48F, 0.95F, 1.0F);
    case 'e': return ImVec4(1.0F, 0.90F, 0.34F, 1.0F);
    default: return ImVec4(0.92F, 0.92F, 0.95F, 1.0F);
    }
}

ImU32 defenseBlockColor(const std::uint16_t blockId,
                        const std::uint8_t metadata) noexcept
{
    if (blockId == 35U || blockId == 95U || blockId == 159U) {
        constexpr std::array<ImU32, 16U> dyeColors{
            IM_COL32(225, 225, 225, 255), IM_COL32(216, 122, 45, 255),
            IM_COL32(178, 80, 188, 255), IM_COL32(102, 145, 205, 255),
            IM_COL32(198, 184, 54, 255), IM_COL32(89, 167, 53, 255),
            IM_COL32(217, 132, 153, 255), IM_COL32(66, 66, 66, 255),
            IM_COL32(152, 152, 152, 255), IM_COL32(44, 119, 146, 255),
            IM_COL32(127, 63, 178, 255), IM_COL32(46, 64, 154, 255),
            IM_COL32(105, 66, 40, 255), IM_COL32(72, 118, 42, 255),
            IM_COL32(154, 52, 48, 255), IM_COL32(28, 28, 33, 255)};
        return dyeColors[metadata & 0xFU];
    }
    switch (blockId) {
    case 1U: return IM_COL32(125, 127, 130, 255);
    case 4U: return IM_COL32(105, 107, 110, 255);
    case 5U: return IM_COL32(167, 125, 72, 255);
    case 17U: return IM_COL32(118, 86, 49, 255);
    case 20U: return IM_COL32(154, 210, 218, 220);
    case 24U: return IM_COL32(214, 199, 139, 255);
    case 45U: return IM_COL32(151, 75, 67, 255);
    case 49U: return IM_COL32(37, 25, 50, 255);
    case 121U: return IM_COL32(218, 221, 143, 255);
    default: return IM_COL32(142, 137, 151, 255);
    }
}

void drawInventoryBlockIcon(ImDrawList* const draw, const ImVec2 center,
                            const float size, const std::uint16_t blockId,
                            const std::uint8_t metadata,
                            const unsigned* textures) noexcept
{
    if (draw == nullptr) return;
    
    // Map block ID to index in textures array
    int texIndex = -1;
    switch (blockId) {
    case 35: texIndex = 0; break; // Wool
    case 5:  texIndex = 1; break; // Planks
    case 159: texIndex = 2; break; // Terracotta / Hardened clay
    case 20: 
    case 95: texIndex = 3; break; // Glass / Stained Glass
    case 121: texIndex = 4; break; // End Stone
    case 49: texIndex = 5; break; // Obsidian
    }

    if (texIndex >= 0 && textures[texIndex] != 0U) {
        // Draw 2D PNG icon
        const float half = size * 0.5F;
        draw->AddImage(
            reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(textures[texIndex])),
            ImVec2(center.x - half, center.y - half),
            ImVec2(center.x + half, center.y + half)
        );
        return;
    }

    // Fallback to 3D solid color cube if texture is missing or unknown block
    const ImU32 base = defenseBlockColor(blockId, metadata);
    const ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(base);
    const auto shaded = [&](const float multiplier) noexcept {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(
            std::clamp(rgba.x * multiplier, 0.0F, 1.0F),
            std::clamp(rgba.y * multiplier, 0.0F, 1.0F),
            std::clamp(rgba.z * multiplier, 0.0F, 1.0F), rgba.w));
    };
    const float half = size * 0.5F;
    const float quarter = size * 0.24F;
    const ImVec2 top[4]{
        {center.x, center.y - half}, {center.x + half, center.y - quarter},
        {center.x, center.y}, {center.x - half, center.y - quarter}};
    const ImVec2 left[4]{
        {center.x - half, center.y - quarter}, {center.x, center.y},
        {center.x, center.y + half}, {center.x - half, center.y + quarter}};
    const ImVec2 right[4]{
        {center.x, center.y}, {center.x + half, center.y - quarter},
        {center.x + half, center.y + quarter}, {center.x, center.y + half}};
    draw->AddConvexPolyFilled(top, 4, shaded(1.0F));
    draw->AddConvexPolyFilled(left, 4, shaded(0.7F));
    draw->AddConvexPolyFilled(right, 4, shaded(0.5F));
    draw->AddPolyline(top, 4, IM_COL32(255, 255, 255, 65), ImDrawFlags_Closed, 1.0F);
}

} // namespace

OverlayRenderer::OverlayRenderer()
    : m_inputState(new (std::nothrow) OverlayInputState)
{
}

OverlayRenderer::~OverlayRenderer()
{
    abandonAfterHookDisabled();
    delete m_inputState;
    m_inputState = nullptr;
}

bool OverlayRenderer::consumeClickGuiToggle() noexcept
{
    pollFallbackInput();
    return m_inputState != nullptr &&
           m_inputState->clickGuiToggle.exchange(false, std::memory_order_acq_rel);
}

void OverlayRenderer::setFeatureSettings(const FeatureSettings& settings) noexcept
{
    // Do not overwrite a just-clicked ImGui value before AgentRuntime has
    // consumed it and published the corresponding atomic bitset.
    if (!m_featureSettingsDirty) {
        if (m_featureSnapshotInitialized && m_features != settings) {
            enqueueFeatureToasts(m_features, settings);
        }
        m_features = settings;
        m_featureSnapshotInitialized = true;
    }
}

bool OverlayRenderer::consumeFeatureSettings(FeatureSettings& settings) noexcept
{
    if (!m_featureSettingsDirty) return false;
    settings = m_features;
    m_featureSettingsDirty = false;
    return true;
}

void OverlayRenderer::setHypixelSnapshot(const HypixelOverlaySnapshot& snapshot) noexcept
{
    m_hypixel = snapshot;
}

void OverlayRenderer::setPlayerStatsSnapshot(
    const PlayerStatsOverlaySnapshot& snapshot) noexcept
{
    m_playerStats = snapshot;
}

bool OverlayRenderer::consumeHypixelQuery(std::array<char, 17U>& playerId) noexcept
{
    if (!m_hypixelQueryPending) return false;
    playerId = m_hypixelQuery;
    m_hypixelQueryPending = false;
    return true;
}

void OverlayRenderer::setMenuHotkey(const unsigned virtualKey) noexcept
{
    if (virtualKey < 8U || virtualKey > 254U) return;
    if (m_menuHotkey == virtualKey && m_inputState != nullptr &&
        m_inputState->menuHotkey.load(std::memory_order_acquire) == virtualKey) {
        return;
    }
    m_menuHotkey = virtualKey;
    if (m_inputState != nullptr) {
        m_inputState->menuHotkey.store(virtualKey, std::memory_order_release);
        m_inputState->fallbackPrimed = false;
    }
}

bool OverlayRenderer::consumeMenuHotkeyChange(unsigned& virtualKey) noexcept
{
    if (!m_menuHotkeyDirty) return false;
    virtualKey = m_menuHotkey;
    m_menuHotkeyDirty = false;
    return true;
}

void OverlayRenderer::setGuiScaleIndex(const int index) noexcept
{
    if (m_guiScaleDirty) return;
    m_guiScaleIndex = std::clamp(index, 0, 3);
}

bool OverlayRenderer::consumeGuiScaleChange(int& index) noexcept
{
    if (!m_guiScaleDirty) return false;
    index = std::clamp(m_guiScaleIndex, 0, 3);
    m_guiScaleDirty = false;
    return true;
}

bool OverlayRenderer::consumeBedRescanRequest() noexcept
{
    return std::exchange(m_bedRescanPending, false);
}

void OverlayRenderer::pollFallbackInput() noexcept
{
    OverlayInputState* const input = m_inputState;
    if (input == nullptr ||
        input->directImGuiWndProc.load(std::memory_order_acquire) ||
        !input->acceptImGuiMessages.load(std::memory_order_acquire)) {
        return;
    }

    const HWND window = input->window.load(std::memory_order_acquire);
    if (window == nullptr || ::GetForegroundWindow() != window) {
        ImGuiContext* const context = input->imguiContext.load(std::memory_order_acquire);
        if (context != nullptr) {
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            for (int button = 0; button < 5; ++button) {
                if (input->mouseDown[button]) {
                    io.AddMouseButtonEvent(button, false);
                    input->mouseDown[button] = false;
                }
            }
        }
        input->fallbackPrimed = false;
        input->captureKeysPrimed = false;
        return;
    }

    const unsigned menuHotkey = input->menuHotkey.load(std::memory_order_acquire);
    const bool menuKeyDown = (::GetAsyncKeyState(static_cast<int>(menuHotkey)) & 0x8000) != 0;
    const bool escapeKeyDown = (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool mouseDown[5] = {
        (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
        (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
        (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0,
        (::GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0,
        (::GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0,
    };

    if (!input->fallbackPrimed) {
        // Prime from the current physical state so attaching while a hotkey is
        // already held does not synthesize a toggle edge.
        input->fallbackPrimed = true;
        input->menuKeyDown = menuKeyDown;
        input->escapeKeyDown = escapeKeyDown;
        ImGuiContext* const context = input->imguiContext.load(std::memory_order_acquire);
        if (context != nullptr) {
            ImGui::SetCurrentContext(context);
            ImGuiIO& io = ImGui::GetIO();
            POINT cursor{};
            if (::GetCursorPos(&cursor) != FALSE &&
                ::ScreenToClient(window, &cursor) != FALSE) {
                io.AddMousePosEvent(static_cast<float>(cursor.x),
                                    static_cast<float>(cursor.y));
            }
            for (int button = 0; button < 5; ++button) {
                io.AddMouseButtonEvent(button, mouseDown[button]);
            }
        }
        for (int button = 0; button < 5; ++button) {
            input->mouseDown[button] = mouseDown[button];
        }
        return;
    }

    bool capturedThisFrame = false;
    if (input->captureHotkey.load(std::memory_order_acquire)) {
        if (!input->captureKeysPrimed) {
            for (unsigned key = 8U; key <= 254U; ++key) {
                input->keyDown[key] =
                    (::GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
            }
            input->captureKeysPrimed = true;
        } else {
            for (unsigned key = 8U; key <= 254U; ++key) {
                const bool down =
                    (::GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
                const bool rising = down && !input->keyDown[key];
                input->keyDown[key] = down;
                if (!rising || key == VK_LBUTTON || key == VK_RBUTTON ||
                    key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2) {
                    continue;
                }
                input->capturedHotkey.store(key, std::memory_order_release);
                input->captureHotkey.store(false, std::memory_order_release);
                input->captureKeysPrimed = false;
                capturedThisFrame = true;
                break;
            }
        }
    } else {
        input->captureKeysPrimed = false;
    }
    if (!capturedThisFrame) {
        if (menuKeyDown && !input->menuKeyDown) {
            input->clickGuiToggle.store(true, std::memory_order_release);
        }
        if (escapeKeyDown && !input->escapeKeyDown &&
            input->interactive.load(std::memory_order_acquire)) {
            input->clickGuiToggle.store(true, std::memory_order_release);
        }
    }
    input->menuKeyDown = menuKeyDown;
    input->escapeKeyDown = escapeKeyDown;

    ImGuiContext* const context = input->imguiContext.load(std::memory_order_acquire);
    if (context != nullptr) {
        ImGui::SetCurrentContext(context);
        ImGuiIO& io = ImGui::GetIO();
        POINT cursor{};
        if (::GetCursorPos(&cursor) != FALSE && ::ScreenToClient(window, &cursor) != FALSE) {
            io.AddMousePosEvent(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        }
        for (int button = 0; button < 5; ++button) {
            if (input->mouseDown[button] != mouseDown[button]) {
                io.AddMouseButtonEvent(button, mouseDown[button]);
            }
        }
    }
    for (int button = 0; button < 5; ++button) {
        input->mouseDown[button] = mouseDown[button];
    }
}

bool OverlayRenderer::ownsCurrentContext() const noexcept
{
    return m_initialized && m_glContext != nullptr &&
           m_glContext == ::wglGetCurrentContext();
}

void OverlayRenderer::applyGuiScaleStyle(const float scale, const int fontIndex) noexcept
{
    ImGuiStyle fresh{};
    ImGui::StyleColorsDark(&fresh);
    fresh.WindowRounding = 16.0F * scale;
    fresh.ChildRounding = 12.0F * scale;
    fresh.FrameRounding = 9.0F * scale;
    fresh.PopupRounding = 12.0F * scale;
    fresh.WindowPadding = ImVec2(17.0F * scale, 15.0F * scale);
    fresh.ItemSpacing = ImVec2(10.0F * scale, 8.0F * scale);
    fresh.ItemInnerSpacing = ImVec2(7.0F * scale, 5.0F * scale);
    fresh.WindowBorderSize = 0.0F;
    fresh.ChildBorderSize = 0.0F;
    fresh.PopupBorderSize = 0.0F;
    fresh.FrameBorderSize = 0.0F;
    fresh.ScrollbarRounding = 12.0F * scale;
    fresh.GrabRounding = 9.0F * scale;
    fresh.Colors[ImGuiCol_WindowBg] = ImVec4(0.055F, 0.050F, 0.072F, 0.88F);
    fresh.Colors[ImGuiCol_ChildBg] = ImVec4(0.090F, 0.082F, 0.112F, 0.82F);
    fresh.Colors[ImGuiCol_PopupBg] = ImVec4(0.075F, 0.068F, 0.095F, 0.96F);
    fresh.Colors[ImGuiCol_Header] = ImVec4(0.39F, 0.31F, 0.68F, 0.85F);
    fresh.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.50F, 0.39F, 0.82F, 0.90F);
    fresh.Colors[ImGuiCol_Button] = ImVec4(0.42F, 0.33F, 0.75F, 0.92F);
    fresh.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.52F, 0.41F, 0.88F, 1.0F);
    fresh.Colors[ImGuiCol_ButtonActive] = ImVec4(0.61F, 0.48F, 0.96F, 1.0F);
    fresh.Colors[ImGuiCol_FrameBg] = ImVec4(0.15F, 0.14F, 0.19F, 0.90F);
    fresh.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21F, 0.19F, 0.28F, 0.96F);
    fresh.Colors[ImGuiCol_Separator] = ImVec4(0.40F, 0.36F, 0.48F, 0.36F);
    ImGui::GetStyle() = fresh;
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.0F;
    const int selectedFont = std::clamp(fontIndex, 0, 3);
    if (m_fonts[static_cast<std::size_t>(selectedFont)] != nullptr) {
        io.FontDefault = m_fonts[static_cast<std::size_t>(selectedFont)];
    }
    m_appliedGuiScaleIndex = selectedFont;
}

void OverlayRenderer::enqueueToast(const char* const label, const bool enabled) noexcept
{
    if (label == nullptr || label[0] == '\0') return;
    char message[52]{};
    std::snprintf(message, sizeof(message), "%s %s", label,
                  enabled ? "enabled" : "disabled");
    enqueueMessage(message, enabled);
}

void OverlayRenderer::enqueueMessage(const char* const message,
                                     const bool positive) noexcept
{
    if (message == nullptr || message[0] == '\0') return;
    Toast* selected = nullptr;
    for (Toast& toast : m_toasts) {
        if (!toast.active) { selected = &toast; break; }
        if (selected == nullptr || toast.sequence < selected->sequence) selected = &toast;
    }
    if (selected == nullptr) return;
    *selected = {};
    std::snprintf(selected->label.data(), selected->label.size(), "%s", message);
    selected->enabled = positive;
    selected->active = true;
    selected->sequence = ++m_toastSequence;
}

void OverlayRenderer::enqueueFeatureToasts(const FeatureSettings& before,
                                           const FeatureSettings& after) noexcept
{
    if (before.espEnabled != after.espEnabled)
        enqueueToast("ESP master", after.espEnabled);
    if (before.entityEspEnabled != after.entityEspEnabled)
        enqueueToast("Living hitboxes", after.entityEspEnabled);
    if (before.bedEspEnabled != after.bedEspEnabled)
        enqueueToast("Bed ESP", after.bedEspEnabled);
    if (before.labelsEnabled != after.labelsEnabled)
        enqueueToast("World labels", after.labelsEnabled);
    if (before.hypixelPanelEnabled != after.hypixelPanelEnabled)
        enqueueToast("Player statistics", after.hypixelPanelEnabled);
    if (before.bedThreatAlertsEnabled != after.bedThreatAlertsEnabled)
        enqueueToast("Bed threat alerts", after.bedThreatAlertsEnabled);
    if (before.bedDefensePanelEnabled != after.bedDefensePanelEnabled)
        enqueueToast("Bed defense panel", after.bedDefensePanelEnabled);
}

void OverlayRenderer::updateBedThreatAlerts(const GameSnapshot& snapshot) noexcept
{
    const std::uint64_t now = static_cast<std::uint64_t>(::GetTickCount64());
    if (!m_features.bedThreatAlertsEnabled ||
        snapshot.state != GameSnapshot::State::Ready ||
        !snapshot.matchActive || snapshot.ownTeam == 'u' ||
        !snapshot.ownBedKnown) {
        for (ThreatContact& contact : m_threatContacts) contact.inside = false;
        return;
    }

    const double enterDistance = static_cast<double>(
        std::clamp(m_features.bedThreatRadius, 3, 32));
    const double leaveDistance = enterDistance + 1.25;

    auto checkEntity = [&](const EntityMarker& entity) {
        if (!entity.player) return;
        // Only a chestplate which positively resolves to our own colour is
        // safe. Missing/unrecognized armour remains a possible invisibility
        // threat even when stale TAB metadata says the name is a teammate.
        if (entity.hasArmor && entity.armorTeam == snapshot.ownTeam) return;
        if (entity.entityId == snapshot.entityId) return; // Ignore local player
        
        for (std::uint32_t bedIndex = 0U; bedIndex < snapshot.bedMarkerCount; ++bedIndex) {
            const BedMarker& bed = snapshot.bedMarkers[bedIndex];
            
            if (bed.x != snapshot.ownBedX || bed.y != snapshot.ownBedY ||
                bed.z != snapshot.ownBedZ) continue;
            const double dx = std::min(std::abs(entity.currentX - (bed.x + 0.5)),
                                       std::abs(entity.currentX - (bed.footX + 0.5)));
            const double dz = std::min(std::abs(entity.currentZ - (bed.z + 0.5)),
                                       std::abs(entity.currentZ - (bed.footZ + 0.5)));
            const double dy = std::abs(entity.currentY - static_cast<double>(bed.y));
            const double distance = std::sqrt(dx * dx + dz * dz + dy * dy);

            ThreatContact* contact = nullptr;
            ThreatContact* oldest = &m_threatContacts.front();
            for (ThreatContact& candidate : m_threatContacts) {
                if (candidate.entityId == entity.entityId && candidate.bedX == bed.x &&
                    candidate.bedY == bed.y && candidate.bedZ == bed.z) {
                    contact = &candidate;
                    break;
                }
                if (candidate.entityId < 0 ||
                    candidate.lastSeenTick < oldest->lastSeenTick) oldest = &candidate;
            }
            if (contact == nullptr) {
                contact = oldest;
                *contact = {};
                contact->entityId = entity.entityId;
                contact->bedX = bed.x;
                contact->bedY = bed.y;
                contact->bedZ = bed.z;
            }
            contact->lastSeenTick = now;
            contact->distance = distance;
            contact->teamColor = entity.teamColor != 'u'
                ? entity.teamColor : entity.armorTeam;
            contact->playerName = entity.playerName;
            if (distance > leaveDistance) contact->inside = false;
            else if (distance <= enterDistance) contact->inside = true;
        }
    };

    for (std::uint32_t entityIndex = 0U;
         entityIndex < snapshot.entityMarkerCount; ++entityIndex) {
        const EntityMarker& entity = snapshot.entityMarkers[entityIndex];
        if (!entity.player) continue;
        checkEntity(entity);
    }
    for (ThreatContact& contact : m_threatContacts) {
        if (contact.entityId >= 0 && now - contact.lastSeenTick > 500U) contact.inside = false;
    }
}

void OverlayRenderer::renderToasts(const float deltaSeconds, const float uiScale) noexcept
{
    constexpr float lifetime = 3.4F;
    constexpr float transition = 0.32F;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = 300.0F * uiScale;
    const float height = 58.0F * uiScale;
    const float gap = 10.0F * uiScale;
    std::array<const ThreatContact*, 64U> threats{};
    std::size_t threatCount = 0U;
    for (const ThreatContact& contact : m_threatContacts) {
        if (contact.inside && contact.entityId >= 0) threats[threatCount++] = &contact;
    }
    std::sort(threats.begin(), threats.begin() + threatCount,
              [](const ThreatContact* first, const ThreatContact* second) {
                  return first->distance < second->distance;
              });
    std::array<Toast*, 6U> ordered{};
    std::size_t count = 0U;
    for (Toast& toast : m_toasts) {
        if (!toast.active) continue;
        toast.age += std::clamp(deltaSeconds, 0.0F, 0.05F);
        if (toast.age >= lifetime) { toast.active = false; continue; }
        ordered[count++] = &toast;
    }
    std::sort(ordered.begin(), ordered.begin() + count,
              [](const Toast* a, const Toast* b) { return a->sequence > b->sequence; });
    ImDrawList* const draw = ImGui::GetForegroundDrawList();
    for (std::size_t index = 0U; index < threatCount; ++index) {
        const ThreatContact& contact = *threats[index];
        const float targetX = display.x - width - 22.0F * uiScale;
        const float y = display.y - 24.0F * uiScale - height -
                        static_cast<float>(index) * (height + gap);
        const ImVec2 min(targetX, y), max(targetX + width, y + height);
        draw->AddRectFilled(ImVec2(min.x + 4.0F * uiScale, min.y + 7.0F * uiScale),
                            ImVec2(max.x + 4.0F * uiScale, max.y + 7.0F * uiScale),
                            IM_COL32(0, 0, 0, 75), 15.0F * uiScale);
        draw->AddRectFilled(min, max, IM_COL32(36, 24, 30, 246), 15.0F * uiScale);
        const ImU32 accent = IM_COL32(255, 92, 104, 255);
        draw->AddRectFilled(min, ImVec2(min.x + 5.0F * uiScale, max.y), accent,
                            15.0F * uiScale, ImDrawFlags_RoundCornersLeft);
        draw->AddCircleFilled(ImVec2(min.x + 29.0F * uiScale, min.y + height * 0.5F),
                              11.0F * uiScale, accent);
        const ImVec2 exclamationSize = ImGui::CalcTextSize("!");
        draw->AddText(ImVec2(min.x + 29.0F * uiScale - exclamationSize.x * 0.5F,
                             min.y + height * 0.5F - exclamationSize.y * 0.5F),
                      IM_COL32(255, 255, 255, 255), "!");
        const char* const name = contact.playerName[0U] == '\0'
            ? "Unknown player" : contact.playerName.data();
        const ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(teamColor(contact.teamColor));
        const ImVec2 textPos(min.x + 50.0F * uiScale, min.y + 11.0F * uiScale);
        draw->AddText(textPos, nameColor, name);
        char distanceLabel[64]{};
        std::snprintf(distanceLabel, sizeof(distanceLabel),
                      "Enemy near bed  %.1fm / %dm", contact.distance,
                      std::clamp(m_features.bedThreatRadius, 3, 32));
        draw->AddText(ImVec2(textPos.x, textPos.y + 22.0F * uiScale),
                      IM_COL32(235, 224, 232, 255), distanceLabel);
    }
    for (std::size_t index = 0U; index < count; ++index) {
        Toast& toast = *ordered[index];
        const float enter = std::clamp(toast.age / transition, 0.0F, 1.0F);
        const float exit = std::clamp((lifetime - toast.age) / transition, 0.0F, 1.0F);
        const float progress = std::min(enter, exit);
        const float eased = 1.0F - std::pow(1.0F - progress, 3.0F);
        const float targetX = display.x - width - 22.0F * uiScale;
        const float x = display.x + 12.0F * uiScale +
                        (targetX - display.x - 12.0F * uiScale) * eased;
        const float y = display.y - 24.0F * uiScale - height -
                        static_cast<float>(index + threatCount) * (height + gap);
        const ImVec2 min(x, y), max(x + width, y + height);
        draw->AddRectFilled(ImVec2(min.x + 4.0F * uiScale, min.y + 7.0F * uiScale),
                            ImVec2(max.x + 4.0F * uiScale, max.y + 7.0F * uiScale),
                            IM_COL32(0, 0, 0, static_cast<int>(75.0F * eased)), 15.0F * uiScale);
        draw->AddRectFilled(min, max, IM_COL32(28, 25, 36, static_cast<int>(242.0F * eased)),
                            15.0F * uiScale);
        const ImU32 accent = toast.enabled ? IM_COL32(87, 220, 126, 255)
                                           : IM_COL32(255, 105, 115, 255);
        draw->AddRectFilled(min, ImVec2(min.x + 5.0F * uiScale, max.y), accent,
                            15.0F * uiScale, ImDrawFlags_RoundCornersLeft);
        draw->AddCircleFilled(ImVec2(min.x + 29.0F * uiScale, min.y + height * 0.5F),
                              10.0F * uiScale, accent);
        draw->AddText(ImVec2(min.x + 50.0F * uiScale, min.y + 19.0F * uiScale),
                      IM_COL32(245, 240, 252, static_cast<int>(255.0F * eased)),
                      toast.label.data());
    }
}

bool OverlayRenderer::initialize(HWND const window, HGLRC const context) noexcept
{
    if (m_permanentlyDisabled) {
        return false;
    }
    if (m_inputState == nullptr) {
        m_inputState = new (std::nothrow) OverlayInputState;
        if (m_inputState == nullptr) {
            return false;
        }
    }

    IMGUI_CHECKVERSION();
    m_imguiContext = ImGui::CreateContext();
    if (m_imguiContext == nullptr) {
        return false;
    }
    ImGui::SetCurrentContext(m_imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // FontGlobalScale enlarges a single bitmap and becomes visibly blurry.
    // Build four independently rasterized Segoe UI sizes into one atlas and
    // switch the default font only at the safe pre-NewFrame boundary.
    char windowsDirectory[MAX_PATH]{};
    const UINT windowsLength = ::GetWindowsDirectoryA(windowsDirectory, MAX_PATH);
    std::array<char, MAX_PATH> fontPath{};
    if (windowsLength > 0U && windowsLength + 20U < fontPath.size()) {
        std::snprintf(fontPath.data(), fontPath.size(), "%s\\Fonts\\segoeui.ttf",
                      windowsDirectory);
    }
    constexpr std::array<float, 4U> fontSizes{15.0F, 19.0F, 23.0F, 27.0F};
    for (std::size_t index = 0U; index < m_fonts.size(); ++index) {
        ImFontConfig fontConfig{};
        fontConfig.OversampleH = 3;
        fontConfig.OversampleV = 2;
        fontConfig.PixelSnapH = false;
        if (fontPath[0U] != '\0') {
            m_fonts[index] = io.Fonts->AddFontFromFileTTF(
                fontPath.data(), fontSizes[index], &fontConfig,
                io.Fonts->GetGlyphRangesDefault());
        }
        if (m_fonts[index] == nullptr) {
            fontConfig.SizePixels = fontSizes[index];
            m_fonts[index] = io.Fonts->AddFontDefault(&fontConfig);
        }
    }

    m_appliedGuiScaleIndex = -1;
    m_animatedGuiScale = guiScaleForIndex(m_guiScaleIndex);
    applyGuiScaleStyle(m_animatedGuiScale, m_guiScaleIndex);

    if (!ImGui_ImplWin32_Init(window)) {
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
        m_fonts = {};
        return false;
    }
    if (!ImGui_ImplOpenGL2_Init()) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
        m_fonts = {};
        return false;
    }

    m_inputState->imguiContext.store(m_imguiContext, std::memory_order_release);
    m_inputState->window.store(window, std::memory_order_release);
    m_inputState->acceptImGuiMessages.store(true, std::memory_order_release);
    m_inputState->fallbackPrimed = false;
    DWORD windowProcessId = 0U;
    const DWORD windowThreadId = ::GetWindowThreadProcessId(window, &windowProcessId);
    m_inputState->directImGuiWndProc.store(
        windowThreadId != 0U && windowThreadId == ::GetCurrentThreadId(),
        std::memory_order_release);
    const bool windowProcedureInstalled = m_windowProcedure.install(
        window, &OverlayRenderer::handleWindowMessage, m_inputState);
    m_inputState->windowProcedureAvailable.store(
        windowProcedureInstalled, std::memory_order_release);
    if (!windowProcedureInstalled && !m_wndProcFallbackLogged) {
        log::info("WndProc input hook unavailable; using non-blocking async input fallback.");
        m_wndProcFallbackLogged = true;
    }

    int bedWidth = 0, bedHeight = 0, bedChannels = 0;
    unsigned char* bedPixels = stbi_load_from_memory(BED_PNG_DATA, sizeof(BED_PNG_DATA), &bedWidth, &bedHeight, &bedChannels, 4);
    if (bedPixels) {
        GLint lastTexture = 0;
        ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
        ::glGenTextures(1, &m_bedTexture);
        ::glBindTexture(GL_TEXTURE_2D, m_bedTexture);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, bedWidth, bedHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, bedPixels);
        stbi_image_free(bedPixels);

        auto loadBlockTexture = [](const unsigned char* data, std::size_t size) -> unsigned {
            int w = 0, h = 0, c = 0;
            unsigned char* pixels = stbi_load_from_memory(data, size, &w, &h, &c, 4);
            if (!pixels) return 0U;
            unsigned tex = 0U;
            ::glGenTextures(1, &tex);
            ::glBindTexture(GL_TEXTURE_2D, tex);
            ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
            return tex;
        };
        m_blockTextures[0] = loadBlockTexture(BLOCK_WOOL_PNG, sizeof(BLOCK_WOOL_PNG));
        m_blockTextures[1] = loadBlockTexture(BLOCK_PLANKS_OAK_PNG, sizeof(BLOCK_PLANKS_OAK_PNG));
        m_blockTextures[2] = loadBlockTexture(BLOCK_HARDENED_CLAY_PNG, sizeof(BLOCK_HARDENED_CLAY_PNG));
        m_blockTextures[3] = loadBlockTexture(BLOCK_GLASS_PNG, sizeof(BLOCK_GLASS_PNG));
        m_blockTextures[4] = loadBlockTexture(BLOCK_END_STONE_PNG, sizeof(BLOCK_END_STONE_PNG));
        m_blockTextures[5] = loadBlockTexture(BLOCK_OBSIDIAN_PNG, sizeof(BLOCK_OBSIDIAN_PNG));

        ::glBindTexture(GL_TEXTURE_2D, lastTexture);
    }

    m_window = window;
    m_glContext = context;
    m_initialized = true;
    return true;
}

void OverlayRenderer::shutdownWithCurrentContext() noexcept
{
    if (!m_initialized || m_imguiContext == nullptr) {
        return;
    }
    if (m_inputState != nullptr) {
        m_inputState->acceptImGuiMessages.store(false, std::memory_order_release);
        m_inputState->interactive.store(false, std::memory_order_release);
        m_inputState->windowProcedureAvailable.store(false, std::memory_order_release);
        m_inputState->directImGuiWndProc.store(false, std::memory_order_release);
        m_inputState->captureHotkey.store(false, std::memory_order_release);
        m_inputState->window.store(nullptr, std::memory_order_release);
    }
    if (!m_windowProcedure.restore()) {
        abandonAfterWndProcDrainTimeout();
        return;
    }
    if (m_inputState != nullptr) {
        m_inputState->imguiContext.store(nullptr, std::memory_order_release);
    }
    ImGui::SetCurrentContext(m_imguiContext);
    if (m_blurTexture != 0U) {
        const GLuint texture = static_cast<GLuint>(m_blurTexture);
        ::glDeleteTextures(1, &texture);
        m_blurTexture = 0U;
        m_blurWidth = 0;
        m_blurHeight = 0;
    }
    if (m_bedTexture != 0U) {
        const GLuint texture = static_cast<GLuint>(m_bedTexture);
        ::glDeleteTextures(1, &texture);
        m_bedTexture = 0U;
    }
    for (unsigned& tex : m_blockTextures) {
        if (tex != 0U) {
            const GLuint t = static_cast<GLuint>(tex);
            ::glDeleteTextures(1, &t);
            tex = 0U;
        }
    }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(m_imguiContext);
    m_imguiContext = nullptr;
    m_fonts = {};
    m_blurTexture = 0U;
    m_bedTexture = 0U;
    m_blurWidth = 0;
    m_blurHeight = 0;
    m_window = nullptr;
    m_glContext = nullptr;
    m_initialized = false;
}

void OverlayRenderer::abandonForContextChange() noexcept
{
    if (m_imguiContext != nullptr) {
        if (m_inputState != nullptr) {
            m_inputState->acceptImGuiMessages.store(false, std::memory_order_release);
            m_inputState->interactive.store(false, std::memory_order_release);
            m_inputState->windowProcedureAvailable.store(false, std::memory_order_release);
            m_inputState->directImGuiWndProc.store(false, std::memory_order_release);
            m_inputState->captureHotkey.store(false, std::memory_order_release);
            m_inputState->window.store(nullptr, std::memory_order_release);
        }
        if (!m_windowProcedure.restore()) {
            abandonAfterWndProcDrainTimeout();
            return;
        }
        if (m_inputState != nullptr) {
            m_inputState->imguiContext.store(nullptr, std::memory_order_release);
        }
        // The old HGLRC is not current. Neither ImGui_ImplOpenGL2_Shutdown nor
        // DestroyContext is legal here: the former would delete driver objects
        // in the new context, while the latter asserts because renderer backend
        // data is still attached. Retain this small, unreachable generation;
        // initialize() creates a fresh context for the new HGLRC.
        log::error("Retaining an ImGui generation whose OpenGL context is no longer current.");
    }
    m_imguiContext = nullptr;
    m_fonts = {};
    // The old HGLRC is unavailable, so its texture cannot be deleted here.
    // Drop the name to prevent a later context generation from deleting an
    // unrelated object which happens to reuse the same GLuint value.
    m_blurTexture = 0U;
    m_bedTexture = 0U;
    m_blurWidth = 0;
    m_blurHeight = 0;
    m_window = nullptr;
    m_glContext = nullptr;
    m_initialized = false;
}

void OverlayRenderer::abandonAfterWndProcDrainTimeout() noexcept
{
    // A callback selected the old handler before restore() unpublished it and
    // did not finish within the bounded drain. Destroying the ImGui context or
    // input bridge would race that callback. Both are intentionally retained;
    // no new handler can acquire them because WndProcHook is already inert.
    log::error("Retaining ImGui state after a WndProc drain timeout.");
    m_inputState = nullptr;
    m_imguiContext = nullptr;
    m_fonts = {};
    m_window = nullptr;
    m_glContext = nullptr;
    m_initialized = false;
    m_permanentlyDisabled = true;
}

void OverlayRenderer::abandonAfterHookDisabled() noexcept
{
    abandonForContextChange();
}

void OverlayRenderer::renderInventoryBlur(const float strength) noexcept
{
    if (strength <= 0.01F) return;
    const ImGuiIO& io = ImGui::GetIO();
    const int width = static_cast<int>(io.DisplaySize.x);
    const int height = static_cast<int>(io.DisplaySize.y);
    if (width < 2 || height < 2) return;

    ::glPushAttrib(GL_ALL_ATTRIB_BITS);
    if (m_blurTexture == 0U) {
        GLuint texture = 0U;
        ::glGenTextures(1, &texture);
        m_blurTexture = texture;
    }
    ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_blurTexture));
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    if (width != m_blurWidth || height != m_blurHeight) {
        ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0,
                       GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        m_blurWidth = width;
        m_blurHeight = height;
    }
    // Copy once, then blend five linearly filtered offset taps. This is a
    // fixed-pipeline Gaussian approximation compatible with Minecraft 1.8.9's
    // OpenGL2 context and avoids shader/FBO setup in third-party clients.
    ::glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    ::glDisable(GL_DEPTH_TEST);
    ::glDisable(GL_CULL_FACE);
    ::glDisable(GL_ALPHA_TEST);
    ::glDisable(GL_LIGHTING);
    ::glEnable(GL_TEXTURE_2D);
    ::glEnable(GL_BLEND);
    ::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    ::glMatrixMode(GL_PROJECTION);
    ::glPushMatrix();
    ::glLoadIdentity();
    ::glOrtho(0.0, static_cast<double>(width), static_cast<double>(height), 0.0, -1.0, 1.0);
    ::glMatrixMode(GL_MODELVIEW);
    ::glPushMatrix();
    ::glLoadIdentity();
    constexpr std::array<std::array<float, 2U>, 5U> offsets{{
        {{-2.2F, 0.0F}}, {{2.2F, 0.0F}}, {{0.0F, -2.2F}},
        {{0.0F, 2.2F}}, {{0.0F, 0.0F}}}};
    for (const auto& offset : offsets) {
        ::glColor4f(1.0F, 1.0F, 1.0F, 0.135F * strength);
        const float x0 = offset[0U];
        const float y0 = offset[1U];
        const float x1 = static_cast<float>(width) + offset[0U];
        const float y1 = static_cast<float>(height) + offset[1U];
        ::glBegin(GL_QUADS);
        ::glTexCoord2f(0.0F, 1.0F); ::glVertex2f(x0, y0);
        ::glTexCoord2f(1.0F, 1.0F); ::glVertex2f(x1, y0);
        ::glTexCoord2f(1.0F, 0.0F); ::glVertex2f(x1, y1);
        ::glTexCoord2f(0.0F, 0.0F); ::glVertex2f(x0, y1);
        ::glEnd();
    }
    ::glDisable(GL_TEXTURE_2D);
    ::glColor4f(0.035F, 0.028F, 0.055F, 0.24F * strength);
    ::glBegin(GL_QUADS);
    ::glVertex2f(0.0F, 0.0F);
    ::glVertex2f(static_cast<float>(width), 0.0F);
    ::glVertex2f(static_cast<float>(width), static_cast<float>(height));
    ::glVertex2f(0.0F, static_cast<float>(height));
    ::glEnd();
    ::glMatrixMode(GL_MODELVIEW);
    ::glPopMatrix();
    ::glMatrixMode(GL_PROJECTION);
    ::glPopMatrix();
    ::glPopAttrib();
}

bool OverlayRenderer::render(HDC const deviceContext,
                             const GameSnapshot& snapshot,
                             const bool interactive) noexcept
{
    if (m_permanentlyDisabled) {
        return false;
    }
    const HGLRC context = ::wglGetCurrentContext();
    const HWND window = ::WindowFromDC(deviceContext);
    if (context == nullptr || window == nullptr || ::IsWindowVisible(window) == FALSE) {
        return false;
    }
    RECT client{};
    if (::GetClientRect(window, &client) == FALSE ||
        client.right - client.left < 480 || client.bottom - client.top < 270) {
        return false;
    }

    bool newlyInitialized = false;
    if (m_initialized && (m_window != window || m_glContext != context)) {
        if (m_glContext == context) {
            shutdownWithCurrentContext();
        } else {
            abandonForContextChange();
        }
    }
    if (!m_initialized) {
        if (!initialize(window, context)) {
            return false;
        }
        newlyInitialized = true;
    }

    if (m_inputState == nullptr) {
        return false;
    }
    m_inputState->interactive.store(interactive, std::memory_order_release);
    if (!interactive) {
        m_inputState->captureHotkey.store(false, std::memory_order_release);
        m_waitingForHotkey = false;
    }
    ImGui::SetCurrentContext(m_imguiContext);
    ImGuiIO& io = ImGui::GetIO();
    m_guiScaleIndex = std::clamp(m_guiScaleIndex, 0, 3);
    const float targetScale = guiScaleForIndex(m_guiScaleIndex);
    const float scaleDelta = std::clamp(io.DeltaTime, 0.0F, 0.05F);
    m_animatedGuiScale += (targetScale - m_animatedGuiScale) *
                          (1.0F - std::exp(-11.0F * scaleDelta));
    if (std::abs(targetScale - m_animatedGuiScale) < 0.001F)
        m_animatedGuiScale = targetScale;
    // Rebuild from immutable constants before NewFrame. This produces a
    // smooth size transition without ever compounding ScaleAllSizes values.
    applyGuiScaleStyle(m_animatedGuiScale, m_guiScaleIndex);
    const float uiScale = m_animatedGuiScale;
    if (interactive) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    const float targetGui = interactive ? 1.0F : 0.0F;
    const float delta = std::clamp(io.DeltaTime, 0.0F, 0.05F);
    m_clickGuiProgress += (targetGui - m_clickGuiProgress) *
                          (1.0F - std::exp(-12.0F * delta));
    const float guiEase = m_clickGuiProgress * m_clickGuiProgress *
                          (3.0F - 2.0F * m_clickGuiProgress);
    renderInventoryBlur(guiEase);

    updateBedThreatAlerts(snapshot);

    if (snapshot.state == GameSnapshot::State::Ready &&
        m_features.espEnabled && snapshot.camera.valid) {
        ImDrawList* const background = ImGui::GetBackgroundDrawList();
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        if (m_features.bedEspEnabled) {
            if (m_features.bedAutoRefreshEnabled) {
                const double now = ImGui::GetTime();
                if (now - m_lastBedRefreshTime >= 2.0) {
                    m_bedRescanPending = true;
                    m_lastBedRefreshTime = now;
                }
            }
            for (std::uint32_t index = 0U; index < snapshot.bedMarkerCount; ++index) {
                const BedMarker& bed = snapshot.bedMarkers[index];
                const AxisAlignedBox bedBox{
                    static_cast<double>(std::min(bed.x, bed.footX)),
                    static_cast<double>(bed.y),
                    static_cast<double>(std::min(bed.z, bed.footZ)),
                    static_cast<double>(std::max(bed.x, bed.footX) + 1),
                    static_cast<double>(bed.y) + 0.5625,
                    static_cast<double>(std::max(bed.z, bed.footZ) + 1)};
                char label[64]{};
                if (m_features.labelsEnabled) {
                    std::snprintf(label, sizeof(label), "Bed  %d %d %d", bed.x, bed.y, bed.z);
                }
                
                const ImU32 boxColor = packedRgbColor(m_features.bedEspColor);
                const bool ownBed = snapshot.ownBedKnown &&
                    bed.x == snapshot.ownBedX && bed.y == snapshot.ownBedY &&
                    bed.z == snapshot.ownBedZ;
                
                drawProjectedBox(background, snapshot.camera, displaySize, bedBox,
                                 boxColor, label, m_features.bedEspFilled);

                if (m_features.bedDefensePanelEnabled && bed.defenseCount > 0U &&
                    (m_features.showOwnBedDefenseInfo || !ownBed)) {
                    const int radius = std::clamp(m_features.bedDefenseRadius, 3, 10);
                    std::array<std::uint16_t, BedMarker::MaxDefenseBlocks> totals{};
                    std::uint32_t visibleBlocks = 0U;
                    for (std::size_t material = 0U; material < bed.defenseCount; ++material) {
                        unsigned total = 0U;
                        for (int ring = 1; ring <= radius; ++ring) {
                            total += bed.defense[material].ringCounts[
                                static_cast<std::size_t>(ring)];
                        }
                        totals[material] = static_cast<std::uint16_t>(
                            std::min<unsigned>(total, UINT16_MAX));
                        if (total != 0U) ++visibleBlocks;
                    }
                    const ScreenPoint anchor = projectPoint(
                        snapshot.camera, displaySize,
                        (static_cast<double>(bed.x + bed.footX) + 1.0) * 0.5,
                        static_cast<double>(bed.y) + 1.65,
                        (static_cast<double>(bed.z + bed.footZ) + 1.0) * 0.5);
                    if (anchor.visible && visibleBlocks > 0U) {
                        const float panelScale = uiScale;
                        const unsigned columns = std::min<std::uint32_t>(visibleBlocks, 6U);
                        const unsigned rows = (visibleBlocks + 5U) / 6U;
                        const float cell = 45.0F * panelScale;
                        const float panelWidth = 20.0F * panelScale +
                            static_cast<float>(columns) * cell;
                        const float panelHeight = 24.0F * panelScale +
                            static_cast<float>(rows) * 49.0F * panelScale;
                        float left = anchor.x - panelWidth * 0.5F;
                        left = std::clamp(left, 8.0F, std::max(8.0F,
                            displaySize.x - panelWidth - 8.0F));
                        const float top = std::clamp(anchor.y - panelHeight - 10.0F * panelScale,
                                                     8.0F, displaySize.y - panelHeight - 8.0F);
                        const ImVec2 panelMin(left, top);
                        const ImVec2 panelMax(left + panelWidth, top + panelHeight);
                        background->AddRectFilled(
                            ImVec2(panelMin.x + 3.0F * panelScale,
                                   panelMin.y + 5.0F * panelScale),
                            ImVec2(panelMax.x + 3.0F * panelScale,
                                   panelMax.y + 5.0F * panelScale),
                            IM_COL32(0, 0, 0, 85), 12.0F * panelScale);
                        background->AddRectFilled(panelMin, panelMax,
                                                  IM_COL32(25, 22, 33, 228),
                                                  12.0F * panelScale);
                        
                        // Draw 2D vanilla bed item icon
                        const float bedIconSize = 18.0F * panelScale;
                        const ImVec2 bPos(left + 8.0F * panelScale, top + 4.0F * panelScale);
                        if (m_bedTexture != 0U) {
                            background->AddImage(reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(m_bedTexture)),
                                                 bPos, ImVec2(bPos.x + bedIconSize, bPos.y + bedIconSize));
                        }

                        char radiusLabel[24]{};
                        std::snprintf(radiusLabel, sizeof(radiusLabel),
                                      "Bed");
                        background->AddText(
                            ImVec2(left + 30.0F * panelScale,
                                   top + 6.0F * panelScale),
                            IM_COL32(221, 210, 241, 235), radiusLabel);
                        unsigned visibleIndex = 0U;
                        for (std::size_t material = 0U; material < bed.defenseCount;
                             ++material) {
                            if (totals[material] == 0U) continue;
                            const unsigned column = visibleIndex % 6U;
                            const unsigned row = visibleIndex / 6U;
                            const float cellX = left + 10.0F * panelScale +
                                (static_cast<float>(column) + 0.5F) * cell;
                            const float cellY = top + 37.0F * panelScale +
                                static_cast<float>(row) * 49.0F * panelScale;
                            drawInventoryBlockIcon(background, ImVec2(cellX, cellY),
                                                   23.0F * panelScale,
                                                   bed.defense[material].blockId,
                                                   bed.defense[material].metadata,
                                                   m_blockTextures);
                            char countLabel[16]{};
                            std::snprintf(countLabel, sizeof(countLabel), "x%u",
                                          static_cast<unsigned>(totals[material]));
                            background->AddText(
                                ImVec2(cellX - 12.0F * panelScale,
                                       cellY + 14.0F * panelScale),
                                IM_COL32(248, 244, 252, 245), countLabel);
                            ++visibleIndex;
                        }
                    }
                }
            }
        }
        if (m_features.entityEspEnabled) {
            const float partial = std::clamp(snapshot.camera.partialTicks, 0.0F, 1.0F);
            if (snapshot.entitySampleGeneration != m_lastEntitySampleGeneration) {
                m_lastEntitySampleGeneration = snapshot.entitySampleGeneration;
                m_missedEntityTicks = 0U;
            } else if (partial + 0.20F < m_lastEntityPartialTicks) {
                // sample() is deliberately capped at 20 Hz. If partialTicks
                // wraps before the next JNI snapshot, the game advanced one
                // tick while the renderer still owns the previous positions.
                // Extrapolate that single missing tick from the already-read
                // velocity; this removes visible lag without extra JNI calls.
                m_missedEntityTicks = std::min(2U, m_missedEntityTicks + 1U);
            }
            m_lastEntityPartialTicks = partial;
            const double renderTick = static_cast<double>(partial) +
                                      static_cast<double>(m_missedEntityTicks);
            for (std::uint32_t index = 0U; index < snapshot.entityMarkerCount; ++index) {
                const EntityMarker& entity = snapshot.entityMarkers[index];
                if (m_features.entityEspPlayersOnly && !entity.player) continue;
                const double renderX = entity.previousX +
                    (entity.currentX - entity.previousX) * renderTick;
                const double renderY = entity.previousY +
                    (entity.currentY - entity.previousY) * renderTick;
                const double renderZ = entity.previousZ +
                    (entity.currentZ - entity.previousZ) * renderTick;
                const double offsetX = renderX - entity.currentX;
                const double offsetY = renderY - entity.currentY;
                const double offsetZ = renderZ - entity.currentZ;
                const AxisAlignedBox interpolated{
                    entity.bounds.minX + offsetX, entity.bounds.minY + offsetY,
                    entity.bounds.minZ + offsetZ, entity.bounds.maxX + offsetX,
                    entity.bounds.maxY + offsetY, entity.bounds.maxZ + offsetZ};
                char label[64]{};
                if (m_features.labelsEnabled) {
                    if (entity.player && entity.playerName[0U] != '\0') {
                        std::snprintf(label, sizeof(label), "%s  %.1fm",
                                      entity.playerName.data(), entity.distance);
                    } else {
                        std::snprintf(label, sizeof(label), "Entity #%d  %.1fm",
                                      entity.entityId, entity.distance);
                    }
                }
                
                const ImU32 entityColor = entity.player
                    ? packedRgbColor(m_features.playerEspColor)
                    : IM_COL32(255, 168, 74, 255);
                drawProjectedBox(background, snapshot.camera, displaySize, interpolated,
                                 entityColor, label);
                const bool isTeammate = entity.player && snapshot.ownTeam != 'u' &&
                                        entity.teamColor == snapshot.ownTeam;
                if (isTeammate) {
                    const double centerX = (interpolated.minX + interpolated.maxX) * 0.5;
                    const double centerZ = (interpolated.minZ + interpolated.maxZ) * 0.5;
                    const double topY = interpolated.maxY + 0.6;
                    const ScreenPoint pt = projectPoint(snapshot.camera, displaySize, centerX, topY, centerZ);
                    if (pt.visible) {
                        const float baseWidth = 14.0F * uiScale;
                        const float arrowHeight = 16.0F * uiScale;
                        background->AddTriangleFilled(
                            ImVec2(pt.x, pt.y),
                            ImVec2(pt.x - baseWidth * 0.5F, pt.y - arrowHeight),
                            ImVec2(pt.x + baseWidth * 0.5F, pt.y - arrowHeight),
                            IM_COL32(40, 255, 40, 230)
                        );
                        if (m_features.labelsEnabled && label[0] != '\0') {
                            const ImVec2 textSize = ImGui::CalcTextSize(label);
                            background->AddText(
                                ImVec2(pt.x - textSize.x * 0.5F, pt.y - arrowHeight - textSize.y - 2.0F * uiScale),
                                IM_COL32(40, 255, 40, 255), label);
                        }
                    }
                }
            }
        }
    }

    // The configurable menu key opens the interactive state used here. The window
    // eases down from above the viewport and uses a custom drag surface so it
    // can remain borderless while still being repositionable.
    if (m_clickGuiX < -9000.0F) {
        m_clickGuiX = std::max(18.0F * uiScale,
                              (io.DisplaySize.x - 570.0F * uiScale) * 0.5F);
    }
    if (m_clickGuiProgress > 0.01F) {
        const float hiddenY = -230.0F * uiScale;
        const float animatedY = hiddenY + (m_clickGuiY - hiddenY) * guiEase;
        ImGui::SetNextWindowPos(ImVec2(m_clickGuiX, animatedY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(570.0F * uiScale, 0.0F), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.88F * guiEase);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, guiEase);
        const ImGuiWindowFlags clickFlags = ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##McOverlayClickGui", nullptr, clickFlags)) {
            const ImVec2 windowPosition = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            ImDrawList* const backgroundDraw = ImGui::GetBackgroundDrawList();
            for (int shadow = 4; shadow >= 1; --shadow) {
                const float spread = static_cast<float>(shadow) * 4.0F * uiScale;
                backgroundDraw->AddRectFilled(
                    ImVec2(windowPosition.x - spread, windowPosition.y - spread + 5.0F * uiScale),
                    ImVec2(windowPosition.x + windowSize.x + spread,
                           windowPosition.y + windowSize.y + spread + 5.0F * uiScale),
                    IM_COL32(7, 5, 12, static_cast<int>(12 * guiEase)),
                    18.0F * uiScale + spread);
            }
            ImGui::InvisibleButton("##dragSurface", ImVec2(-1.0F, 30.0F * uiScale));
            const ImVec2 headerMin = ImGui::GetItemRectMin();
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                m_clickGuiX += io.MouseDelta.x;
                m_clickGuiY += io.MouseDelta.y;
            }
            ImDrawList* const windowDraw = ImGui::GetWindowDrawList();
            windowDraw->AddText(ImVec2(headerMin.x, headerMin.y + 4.0F * uiScale),
                                IM_COL32(240, 232, 255, 255), "MC OVERLAY  /  CLICK GUI");
            ImGui::Separator();
            ImGui::TextDisabled("Interface size");
            constexpr std::array<const char*, 4U> sizeLabels{"S", "M", "L", "XL"};
            for (int sizeIndex = 0; sizeIndex < 4; ++sizeIndex) {
                if (sizeIndex != 0) ImGui::SameLine();
                if (ImGui::RadioButton(sizeLabels[static_cast<std::size_t>(sizeIndex)],
                                       m_guiScaleIndex == sizeIndex)) {
                    m_guiScaleIndex = sizeIndex;
                    m_guiScaleDirty = true;
                }
            }
            ImGui::SameLine(0.0F, 22.0F * uiScale);
            if (ImGui::Button(m_waitingForHotkey ? "Press a key..." : hotkeyName(m_menuHotkey))) {
                m_waitingForHotkey = true;
                m_hotkeyCaptureCooldownFrames = 2;
                m_hotkeyCaptureArmed = false;
                m_inputState->captureHotkey.store(false, std::memory_order_release);
                m_inputState->capturedHotkey.store(0U, std::memory_order_release);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Click GUI bind");
            if (m_waitingForHotkey) {
                const unsigned captured = m_inputState->capturedHotkey.exchange(
                    0U, std::memory_order_acq_rel);
                if (captured != 0U) {
                    if (captured != VK_ESCAPE) {
                        setMenuHotkey(captured);
                        m_menuHotkeyDirty = true;
                    }
                    m_waitingForHotkey = false;
                    m_hotkeyCaptureArmed = false;
                    m_inputState->captureHotkey.store(false, std::memory_order_release);
                }
                if (captured == 0U) {
                    const bool mouseHeld = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
                    if (mouseHeld) {
                        // The click which opened capture must be fully released;
                        // it can never become a bind or retrigger this button.
                        m_hotkeyCaptureArmed = false;
                        m_hotkeyCaptureCooldownFrames = 2;
                    } else if (m_hotkeyCaptureCooldownFrames > 0) {
                        --m_hotkeyCaptureCooldownFrames;
                    } else if (!m_hotkeyCaptureArmed) {
                        m_hotkeyCaptureArmed = true;
                        m_inputState->captureHotkey.store(true, std::memory_order_release);
                    }
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Single-player diagnostics");
            const FeatureSettings featuresBefore = m_features;
            bool changed = false;
            changed |= animatedToggle("ESP master", m_features.espEnabled,
                                      m_toggleAnimation[0], uiScale);
            changed |= animatedToggle("3D living hitboxes", m_features.entityEspEnabled,
                                      m_toggleAnimation[1], uiScale);
            if (m_features.entityEspEnabled) {
                ImGui::SameLine(0.0F, 18.0F * uiScale);
                changed |= animatedToggle("Players only", m_features.entityEspPlayersOnly,
                                          m_toggleAnimation[8], uiScale);
            }
            std::array<float, 3U> playerColor = unpackRgb(m_features.playerEspColor);
            ImGui::SetNextItemWidth(180.0F * uiScale);
            if (ImGui::ColorEdit3("Player box color", playerColor.data(),
                                  ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_DisplayRGB)) {
                m_features.playerEspColor = packRgb(playerColor);
                changed = true;
            }
            changed |= animatedToggle("Bed ESP", m_features.bedEspEnabled,
                                      m_toggleAnimation[2], uiScale);
            ImGui::SameLine(0.0F, 18.0F * uiScale);
            changed |= animatedToggle("Auto-refresh", m_features.bedAutoRefreshEnabled,
                                      m_toggleAnimation[7], uiScale);
            ImGui::SameLine(0.0F, 18.0F * uiScale);
            if (ImGui::Button("Refresh beds now")) {
                m_bedRescanPending = true;
            }
            std::array<float, 3U> bedColor = unpackRgb(m_features.bedEspColor);
            ImGui::SetNextItemWidth(180.0F * uiScale);
            if (ImGui::ColorEdit3("Bed box color", bedColor.data(),
                                  ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_DisplayRGB)) {
                m_features.bedEspColor = packRgb(bedColor);
                changed = true;
            }
            changed |= animatedToggle("Semi-transparent bed fill",
                                      m_features.bedEspFilled,
                                      m_toggleAnimation[9], uiScale);
            changed |= animatedToggle("World labels", m_features.labelsEnabled,
                                      m_toggleAnimation[3], uiScale);
            changed |= animatedToggle("Bed proximity alerts",
                                      m_features.bedThreatAlertsEnabled,
                                      m_toggleAnimation[5], uiScale);
            ImGui::SetNextItemWidth(270.0F * uiScale);
            changed |= ImGui::SliderInt("Bed warning range",
                                        &m_features.bedThreatRadius, 3, 32,
                                        "%d blocks", ImGuiSliderFlags_AlwaysClamp);
            changed |= animatedToggle("Bed defense material panel",
                                      m_features.bedDefensePanelEnabled,
                                      m_toggleAnimation[6], uiScale);
            changed |= animatedToggle("Show own bed defense info",
                                      m_features.showOwnBedDefenseInfo,
                                      m_toggleAnimation[10], uiScale);
            changed |= animatedToggle("Local Debug chat",
                                      m_features.debugChatEnabled,
                                      m_toggleAnimation[11], uiScale);
            ImGui::TextDisabled("Defense material radius (bed level and above)");
            for (int radius = 3; radius <= 10; ++radius) {
                if (radius != 3) ImGui::SameLine();
                ImGui::PushID(radius);
                const bool isCurrentRadius = (radius == m_features.bedDefenseRadius);
                if (isCurrentRadius) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.58F, 0.44F, 0.92F, 1.0F));
                }
                char radiusLabel[4]{};
                std::snprintf(radiusLabel, sizeof(radiusLabel), "%d", radius);
                if (ImGui::Button(radiusLabel, ImVec2(42.0F * uiScale, 0.0F)) &&
                    !isCurrentRadius) {
                    m_features.bedDefenseRadius = radius;
                    changed = true;
                }
                if (isCurrentRadius) ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::SeparatorText("Hypixel player panel");
            changed |= animatedToggle("Show statistics panel",
                                      m_features.hypixelPanelEnabled,
                                      m_toggleAnimation[4], uiScale);
            ImGui::SetNextItemWidth(370.0F * uiScale);
            ImGui::InputTextWithHint("##hypixelPlayer", "Minecraft player ID / name",
                                     m_hypixelInput.data(), m_hypixelInput.size());
            ImGui::SameLine();
            if (ImGui::Button("Query", ImVec2(110.0F * uiScale, 0.0F))) {
                const std::string_view playerId(m_hypixelInput.data());
                const bool valid = !playerId.empty() && playerId.size() <= 16U &&
                    std::all_of(playerId.begin(), playerId.end(), [](const char c) noexcept {
                        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '_';
                    });
                if (valid) {
                    m_hypixelQuery.fill('\0');
                    std::copy(playerId.begin(), playerId.end(), m_hypixelQuery.begin());
                    m_hypixelQueryPending = true;
                }
            }
            ImGui::TextDisabled("Press the configured bind again to restore Minecraft mouse grab");
            m_featureSettingsDirty = m_featureSettingsDirty || changed;
            if (changed) enqueueFeatureToasts(featuresBefore, m_features);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    if (m_features.hypixelPanelEnabled) {
        ImGui::SetNextWindowPos(ImVec2(22.0F * uiScale, 22.0F * uiScale),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0F * uiScale, 0.0F), ImGuiCond_FirstUseEver);
        ImGuiWindowFlags hypixelFlags = ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_AlwaysAutoResize;
        if (!interactive) hypixelFlags |= ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("Hypixel Bed Wars", nullptr, hypixelFlags)) {
            ImGui::Text("State: %s", hypixelStateText(m_hypixel.state));
            if (m_hypixel.displayName[0] != '\0') {
                ImGui::Text("Player: %s", m_hypixel.displayName.data());
            }
            if (m_hypixel.state == HypixelOverlaySnapshot::State::Ready) {
                ImGui::Text("W/L  %lld / %lld    ratio %.2f",
                            static_cast<long long>(m_hypixel.wins),
                            static_cast<long long>(m_hypixel.losses), m_hypixel.winRate);
                ImGui::Text("Final K/D  %lld / %lld    FKDR %.2f",
                            static_cast<long long>(m_hypixel.finalKills),
                            static_cast<long long>(m_hypixel.finalDeaths), m_hypixel.fkdr);
                ImGui::Text("Beds broken/lost  %lld / %lld",
                            static_cast<long long>(m_hypixel.bedsBroken),
                            static_cast<long long>(m_hypixel.bedsLost));
            }
            if (m_hypixel.status[0] != '\0') ImGui::TextWrapped("%s", m_hypixel.status.data());
        }
        ImGui::End();

        if (m_playerStats.count > 0U) {
            ImGui::SetNextWindowPos(
                ImVec2(std::max(18.0F, io.DisplaySize.x - 390.0F * uiScale),
                       22.0F * uiScale), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(430.0F * uiScale, 0.0F), ImGuiCond_FirstUseEver);
            ImGuiWindowFlags boardFlags = ImGuiWindowFlags_NoCollapse |
                                          ImGuiWindowFlags_AlwaysAutoResize;
            if (!interactive) boardFlags |= ImGuiWindowFlags_NoInputs;
            if (ImGui::Begin("Live Bed Wars Players", nullptr, boardFlags)) {
                ImGui::TextDisabled("Automatic roster lookup  /  active matches only");
                ImGui::Spacing();
                std::array<std::uint32_t, PlayerStatsOverlaySnapshot::Capacity> order{};
                for (std::uint32_t index = 0U; index < m_playerStats.count; ++index) {
                    order[index] = index;
                }
                const auto colorCode = [&](const PlayerStatsEntry& entry) noexcept {
                    return entry.teamPrefix[0U] == static_cast<char>(0xC2) &&
                           entry.teamPrefix[1U] == static_cast<char>(0xA7)
                        ? entry.teamPrefix[2U] : 'f';
                };
                std::sort(order.begin(), order.begin() + m_playerStats.count,
                          [&](const std::uint32_t first, const std::uint32_t second) noexcept {
                              const PlayerStatsEntry& a = m_playerStats.entries[first];
                              const PlayerStatsEntry& b = m_playerStats.entries[second];
                              const char ac = colorCode(a);
                              const char bc = colorCode(b);
                              return ac != bc ? ac < bc : std::strcmp(a.name.data(), b.name.data()) < 0;
                          });
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0F * uiScale, 8.0F * uiScale));
                ImGui::BeginChild("##liveRosterCards", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysAutoResize);
                char currentTeam = '\0';
                for (std::uint32_t ordered = 0U; ordered < m_playerStats.count; ++ordered) {
                    const PlayerStatsEntry& player = m_playerStats.entries[order[ordered]];
                    const char code = colorCode(player);
                    if (code != currentTeam) {
                        if (ordered > 0) ImGui::EndGroup();
                        if (ordered > 0) ImGui::SameLine(0.0F, 16.0F * uiScale);
                        currentTeam = code;
                        ImGui::BeginGroup();
                        ImGui::TextColored(teamColor(code), "TEAM %c", static_cast<char>(std::toupper(static_cast<unsigned char>(code))));
                        ImGui::Spacing();
                    }
                    
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImVec2 size(140.0F * uiScale, 64.0F * uiScale);
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    
                    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(35, 35, 45, 255), 8.0F * uiScale);
                    drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(60, 60, 75, 255), 8.0F * uiScale, 0, 1.5F * uiScale);
                    
                    ImGui::PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
                    drawList->AddText(m_fonts[m_appliedGuiScaleIndex], 16.0F * uiScale, ImVec2(pos.x + 8.0F * uiScale, pos.y + 6.0F * uiScale), ImGui::ColorConvertFloat4ToU32(teamColor(code)), player.name.data());
                    
                    const ImU32 fkdrColor = player.fkdr >= 3.0 ? IM_COL32(89, 235, 122, 255) : (player.fkdr >= 1.0 ? IM_COL32(255, 214, 82, 255) : IM_COL32(255, 97, 107, 255));
                    char fkdrText[32];
                    std::snprintf(fkdrText, sizeof(fkdrText), "FKDR: %.2f", player.fkdr);
                    drawList->AddText(m_fonts[m_appliedGuiScaleIndex], 14.0F * uiScale, ImVec2(pos.x + 8.0F * uiScale, pos.y + 26.0F * uiScale), fkdrColor, fkdrText);
                    
                    char starText[32];
                    std::snprintf(starText, sizeof(starText), "%d Stars | Lvl %d", player.stars, player.level);
                    drawList->AddText(m_fonts[m_appliedGuiScaleIndex], 14.0F * uiScale, ImVec2(pos.x + 8.0F * uiScale, pos.y + 42.0F * uiScale), IM_COL32(180, 180, 180, 255), starText);
                    
                    ImGui::PopClipRect();
                    ImGui::Dummy(size);
                }
                if (m_playerStats.count > 0) ImGui::EndGroup();
                ImGui::EndChild();
                ImGui::PopStyleVar();
            }
            ImGui::End();
        }
    }

    renderToasts(delta, uiScale);

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    return newlyInitialized;
}

LRESULT OverlayRenderer::handleWindowMessage(void* const context,
                                             HWND const window,
                                             const UINT message,
                                             const WPARAM wParam,
                                             const LPARAM lParam,
                                             bool& handled) noexcept
{
    return onWindowMessage(*static_cast<OverlayInputState*>(context),
                           window, message, wParam, lParam, handled);
}

LRESULT OverlayRenderer::onWindowMessage(OverlayInputState& input,
                                         HWND const window,
                                         const UINT message,
                                         const WPARAM wParam,
                                         const LPARAM lParam,
                                         bool& handled) noexcept
{
    const bool firstKeyDown = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
                              (lParam & (1LL << 30)) == 0;
    if (firstKeyDown) {
        const UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
        const UINT extendedScan = scanCode | ((lParam & (1LL << 24)) != 0 ? 0xE000U : 0U);
        const UINT resolvedKey = ::MapVirtualKeyW(extendedScan, MAPVK_VSC_TO_VK_EX);
        const unsigned eventKey = resolvedKey != 0U
            ? resolvedKey : static_cast<unsigned>(wParam);
        if (input.captureHotkey.exchange(false, std::memory_order_acq_rel)) {
            if (eventKey >= 8U && eventKey <= 254U &&
                eventKey != VK_LBUTTON && eventKey != VK_RBUTTON &&
                eventKey != VK_MBUTTON && eventKey != VK_XBUTTON1 &&
                eventKey != VK_XBUTTON2) {
                input.capturedHotkey.store(eventKey, std::memory_order_release);
            }
            handled = true;
            return 1;
        }
        if (wParam == VK_ESCAPE && input.interactive.load(std::memory_order_acquire)) {
            input.clickGuiToggle.store(true, std::memory_order_release);
            handled = true;
            return 1;
        }
        const unsigned configured = input.menuHotkey.load(std::memory_order_acquire);
        if (static_cast<unsigned>(wParam) == configured || resolvedKey == configured) {
            input.clickGuiToggle.store(true, std::memory_order_release);
            handled = true;
            return 1;
        }
    }
    ImGuiContext* const context = input.imguiContext.load(std::memory_order_acquire);
    if (input.acceptImGuiMessages.load(std::memory_order_acquire) &&
        input.interactive.load(std::memory_order_acquire) && context != nullptr &&
        input.directImGuiWndProc.load(std::memory_order_acquire)) {
        ImGui::SetCurrentContext(context);
        ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
    }
    // When the Click GUI is open Minecraft must not receive any relative/raw
    // movement or gameplay key, even if ImGui currently has no hovered item.
    // WM_INPUT is cleaned up through DefWindowProc but never forwarded into the
    // client WndProc. This is what prevents Lunar's camera from rotating.
    if (input.interactive.load(std::memory_order_acquire) &&
        (isMouseMessage(message) || isKeyboardMessage(message))) {
        handled = true;
        return message == WM_INPUT
            ? ::DefWindowProcW(window, message, wParam, lParam) : 1;
    }
    handled = false;
    return 0;
}

} // namespace mcoverlay
