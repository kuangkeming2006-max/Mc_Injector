#include "overlay_renderer.h"

#include "src/AgentLog.h"

#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>

#include <gl/GL.h>
#include <imm.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <functional>
#include <new>
#include <string>
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
    // One immutable native cursor is retained for the whole overlay session.
    // Changing HCURSOR shapes every render frame is both visually wrong for
    // custom/Lunar cursors and creates a WM_SETCURSOR race while dragging.
    std::atomic<HCURSOR> sessionCursor{nullptr};
    std::atomic<HWND> window{nullptr};
    std::atomic<ImGuiContext*> imguiContext{nullptr};
    bool fallbackPrimed = false;
    bool menuKeyDown = false;
    bool escapeKeyDown = false;
    bool mouseDown[5]{};
    bool captureKeysPrimed = false;
    std::array<bool, 256U> keyDown{};
    // Legacy IMM messages are delivered on the game window thread, which can
    // differ from Lunar's OpenGL presentation thread. Fixed buffers plus this
    // lock make the handoff allocation-free and race-free.
    SRWLOCK imeLock = SRWLOCK_INIT;
    std::array<wchar_t, 80U> imeName{};
    std::array<wchar_t, 128U> imeComposition{};
    std::array<std::array<wchar_t, 64U>, 9U> imeCandidates{};
    std::uint32_t imeCandidateCount = 0U;
    std::uint32_t imeCandidateSelection = 0U;
    bool imeComposing = false;
    std::atomic<std::uint64_t> imeRevision{0U};
};

namespace {

void updateImeState(OverlayInputState& input, const HWND window,
                    const UINT message, const LPARAM lParam) noexcept
{
    std::array<wchar_t, 80U> name{};
    std::array<wchar_t, 128U> composition{};
    std::array<std::array<wchar_t, 64U>, 9U> candidates{};
    std::uint32_t candidateCount = 0U;
    std::uint32_t candidateSelection = 0U;
    bool composing = false;

    DWORD processId = 0U;
    const DWORD threadId = ::GetWindowThreadProcessId(window, &processId);
    const HKL layout = message == WM_INPUTLANGCHANGE
        ? reinterpret_cast<HKL>(lParam) : ::GetKeyboardLayout(threadId);
    if (layout != nullptr) {
        const UINT described = ::ImmGetDescriptionW(
            layout, name.data(), static_cast<UINT>(name.size()));
        if (described == 0U) {
            const LANGID language = LOWORD(reinterpret_cast<ULONG_PTR>(layout));
            (void)::GetLocaleInfoW(MAKELCID(language, SORT_DEFAULT),
                LOCALE_SLOCALIZEDDISPLAYNAME, name.data(),
                static_cast<int>(name.size()));
        }
    }

    const bool clearComposition = message == WM_IME_ENDCOMPOSITION;
    HIMC const ime = ::ImmGetContext(window);
    if (ime != nullptr && !clearComposition) {
        const LONG bytes = ::ImmGetCompositionStringW(
            ime, GCS_COMPSTR, composition.data(),
            static_cast<DWORD>((composition.size() - 1U) * sizeof(wchar_t)));
        if (bytes > 0) {
            composition[std::min<std::size_t>(
                static_cast<std::size_t>(bytes) / sizeof(wchar_t),
                composition.size() - 1U)] = L'\0';
            composing = true;
        }

        std::array<unsigned char, 8192U> candidateBytes{};
        const DWORD required = ::ImmGetCandidateListW(ime, 0U, nullptr, 0U);
        if (required >= sizeof(CANDIDATELIST) &&
            required <= candidateBytes.size()) {
            auto* const list = reinterpret_cast<CANDIDATELIST*>(
                candidateBytes.data());
            if (::ImmGetCandidateListW(ime, 0U, list,
                    static_cast<DWORD>(candidateBytes.size())) > 0U) {
                const DWORD pageStart = std::min(list->dwPageStart, list->dwCount);
                const DWORD pageCount = std::min<DWORD>(
                    std::min(list->dwPageSize, list->dwCount - pageStart),
                    static_cast<DWORD>(candidates.size()));
                for (DWORD index = 0U; index < pageCount; ++index) {
                    const DWORD sourceIndex = pageStart + index;
                    if (offsetof(CANDIDATELIST, dwOffset) +
                        (static_cast<std::size_t>(sourceIndex) + 1U) *
                            sizeof(DWORD) > required) continue;
                    const DWORD offset = list->dwOffset[sourceIndex];
                    if (offset >= required) continue;
                    const auto* const source = reinterpret_cast<const wchar_t*>(
                        candidateBytes.data() + offset);
                    std::size_t length = 0U;
                    const std::size_t availableCharacters =
                        (required - offset) / sizeof(wchar_t);
                    while (length + 1U < candidates[index].size() &&
                           length < availableCharacters &&
                           source[length] != L'\0') {
                        candidates[index][length] = source[length];
                        ++length;
                    }
                    ++candidateCount;
                }
                if (list->dwSelection >= pageStart &&
                    list->dwSelection < pageStart + pageCount) {
                    candidateSelection = list->dwSelection - pageStart;
                }
                composing = composing || candidateCount != 0U;
            }
        }
        ::ImmReleaseContext(window, ime);
    }

    ::AcquireSRWLockExclusive(&input.imeLock);
    input.imeName = name;
    input.imeComposition = composition;
    input.imeCandidates = candidates;
    input.imeCandidateCount = candidateCount;
    input.imeCandidateSelection = candidateSelection;
    input.imeComposing = composing;
    ::ReleaseSRWLockExclusive(&input.imeLock);
    input.imeRevision.fetch_add(1U, std::memory_order_release);
}

void advancePresentationSpring(float& value, float& velocity,
                               const float target, const float delta) noexcept
{
    constexpr float stiffness = 70.0F;
    constexpr float damping = 12.5F;
    constexpr float halfDamping = damping * 0.5F;
    constexpr float dampedFrequency = std::sqrt(
        stiffness - halfDamping * halfDamping);
    const float displacement = value - target;
    const float secondary = (velocity + halfDamping * displacement) /
                            dampedFrequency;
    const float decay = std::exp(-halfDamping * delta);
    const float cosine = std::cos(dampedFrequency * delta);
    const float sine = std::sin(dampedFrequency * delta);
    const float evolved = displacement * cosine + secondary * sine;
    value = target + decay * evolved;
    velocity = decay *
        (-halfDamping * evolved - displacement * dampedFrequency * sine +
         secondary * dampedFrequency * cosine);
    value = std::clamp(value, -0.045F, 1.055F);
    if (std::abs(target - value) < 0.0005F && std::abs(velocity) < 0.005F) {
        value = target;
        velocity = 0.0F;
    }
}

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

ImU32 contrastingTextColor(const std::uint32_t rgb) noexcept
{
    const int red = static_cast<int>((rgb >> 16U) & 0xFFU);
    const int green = static_cast<int>((rgb >> 8U) & 0xFFU);
    const int blue = static_cast<int>(rgb & 0xFFU);
    const int luminance = red * 299 + green * 587 + blue * 114;
    return luminance >= 150000 ? IM_COL32(18, 18, 22, 255)
                               : IM_COL32(255, 255, 255, 255);
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

struct PrestigeStyle final {
    ImU32 color = IM_COL32(170, 170, 170, 255);
    bool master = false;
};

PrestigeStyle bedWarsPrestigeStyle(const int stars) noexcept
{
    // Hypixel's classic 100-star prestige cadence. At 1000+ the icon changes
    // to the master-prestige star; later tiers continue rotating the familiar
    // palette so the compact row remains readable without reproducing chat's
    // multi-glyph rainbow formatter.
    const int prestige = std::max(0, stars) / 100;
    constexpr std::array<ImU32, 10U> colors{{
        IM_COL32(170, 170, 170, 255), IM_COL32(245, 245, 245, 255),
        IM_COL32(255, 190, 58, 255),  IM_COL32(83, 220, 238, 255),
        IM_COL32(72, 204, 112, 255),  IM_COL32(48, 190, 190, 255),
        IM_COL32(236, 76, 86, 255),   IM_COL32(241, 110, 196, 255),
        IM_COL32(91, 137, 255, 255),  IM_COL32(177, 106, 235, 255)}};
    PrestigeStyle result;
    result.color = colors[static_cast<std::size_t>(prestige % 10)];
    result.master = prestige >= 10;
    return result;
}

bool projectedBoxBounds(const WorldCameraSnapshot& camera,
                        const ImVec2 displaySize,
                        const AxisAlignedBox& box,
                        ImVec2& minimum,
                        ImVec2& maximum) noexcept
{
    const std::array<std::array<double, 3U>, 8U> corners{{
        {{box.minX, box.minY, box.minZ}}, {{box.maxX, box.minY, box.minZ}},
        {{box.maxX, box.minY, box.maxZ}}, {{box.minX, box.minY, box.maxZ}},
        {{box.minX, box.maxY, box.minZ}}, {{box.maxX, box.maxY, box.minZ}},
        {{box.maxX, box.maxY, box.maxZ}}, {{box.minX, box.maxY, box.maxZ}}}};
    minimum = displaySize;
    maximum = ImVec2(0.0F, 0.0F);
    bool found = false;
    for (const auto& corner : corners) {
        const ScreenPoint point = projectPoint(
            camera, displaySize, corner[0U], corner[1U], corner[2U]);
        if (!point.visible) continue;
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        found = true;
    }
    return found;
}

const char* protectionRoman(const std::uint8_t level) noexcept
{
    constexpr std::array<const char*, 6U> labels{{"", "I", "II", "III", "IV", "V"}};
    return level < labels.size() ? labels[level] : "V+";
}

void drawPrestigeStar(ImDrawList* const drawList, const ImVec2 center,
                      const float radius, const ImU32 color,
                      const bool master) noexcept
{
    if (drawList == nullptr || radius <= 0.0F) return;
    std::array<ImVec2, 10U> points{};
    constexpr float pi = 3.14159265358979323846F;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const float angle = -pi * 0.5F + static_cast<float>(index) * pi / 5.0F;
        const float pointRadius = (index % 2U) == 0U ? radius : radius * 0.44F;
        points[index] = ImVec2(center.x + std::cos(angle) * pointRadius,
                               center.y + std::sin(angle) * pointRadius);
    }
    drawList->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), color);
    if (master) {
        const ImU32 ring = IM_COL32(255, 255, 255, 220);
        drawList->AddPolyline(points.data(), static_cast<int>(points.size()), ring,
                              ImDrawFlags_Closed, std::max(1.0F, radius * 0.18F));
        drawList->AddCircle(center, radius * 1.20F, color, 20,
                            std::max(1.0F, radius * 0.14F));
    }
}

void formatCompactCount(char* const output, const std::size_t capacity,
                        const std::int64_t value) noexcept
{
    if (output == nullptr || capacity == 0U) return;
    if (value >= 1'000'000)
        std::snprintf(output, capacity, "%.1fM", static_cast<double>(value) / 1'000'000.0);
    else if (value >= 1'000)
        std::snprintf(output, capacity, "%.1fk", static_cast<double>(value) / 1'000.0);
    else
        std::snprintf(output, capacity, "%lld", static_cast<long long>(value));
}

void drawRoundedTriangle(ImDrawList* const drawList,
                         const ImVec2 a, const ImVec2 b, const ImVec2 c,
                         const float radius, const ImU32 color) noexcept
{
    if (drawList == nullptr) return;
    const auto toward = [](const ImVec2 from, const ImVec2 to,
                           const float distance) noexcept {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.001F) return from;
        const float amount = std::min(distance / length, 0.45F);
        return ImVec2(from.x + dx * amount, from.y + dy * amount);
    };
    const ImVec2 aToB = toward(a, b, radius);
    const ImVec2 bFromA = toward(b, a, radius);
    const ImVec2 bToC = toward(b, c, radius);
    const ImVec2 cFromB = toward(c, b, radius);
    const ImVec2 cToA = toward(c, a, radius);
    const ImVec2 aFromC = toward(a, c, radius);
    drawList->PathClear();
    drawList->PathLineTo(aToB);
    drawList->PathLineTo(bFromA);
    drawList->PathBezierQuadraticCurveTo(b, bToC);
    drawList->PathLineTo(cFromB);
    drawList->PathBezierQuadraticCurveTo(c, cToA);
    drawList->PathLineTo(aFromC);
    drawList->PathBezierQuadraticCurveTo(a, aToB);
    drawList->PathFillConvex(color);
}

bool isMouseMessage(const UINT message) noexcept
{
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
           message == WM_NCMOUSEMOVE || message == WM_NCLBUTTONDOWN ||
           message == WM_NCLBUTTONUP || message == WM_NCRBUTTONDOWN ||
           message == WM_NCRBUTTONUP || message == WM_SETCURSOR ||
           message == WM_INPUT;
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
    const ImVec4 offColor = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    const ImVec4 onColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    const ImVec4 textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const bool lightSurface = textColor.x + textColor.y + textColor.z < 1.5F;
    const ImVec4 mixedColor(
        offColor.x + (onColor.x - offColor.x) * eased,
        offColor.y + (onColor.y - offColor.y) * eased,
        offColor.z + (onColor.z - offColor.z) * eased, 1.0F);
    const ImU32 track = ImGui::GetColorU32(mixedColor);
    draw->AddRectFilled(start, ImVec2(start.x + width, start.y + height),
                        track, height * 0.5F);
    if (!value && animation < 0.5F) {
        const ImVec4 outline = lightSurface
            ? ImVec4(0.47F, 0.44F, 0.50F, 0.72F)
            : ImVec4(0.70F, 0.66F, 0.76F, 0.55F);
        draw->AddRect(start, ImVec2(start.x + width, start.y + height),
                      ImGui::GetColorU32(outline), height * 0.5F, 0,
                      1.0F * uiScale);
    }
    const float knobX = start.x + 11.0F * uiScale +
                        eased * (width - 22.0F * uiScale);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        draw->AddCircleFilled(ImVec2(knobX, start.y + height * 0.5F),
                              10.0F * uiScale,
                              ImGui::GetColorU32(ImVec4(
                                  onColor.x, onColor.y, onColor.z, 0.15F)));
    }
    const ImVec4 offKnob = lightSurface
        ? ImVec4(0.36F, 0.34F, 0.39F, 1.0F)
        : ImVec4(0.76F, 0.72F, 0.80F, 1.0F);
    const ImVec4 onKnob = ImVec4(0.99F, 0.985F, 1.0F, 1.0F);
    const ImVec4 knobColor(
        offKnob.x + (onKnob.x - offKnob.x) * eased,
        offKnob.y + (onKnob.y - offKnob.y) * eased,
        offKnob.z + (onKnob.z - offKnob.z) * eased, 1.0F);
    draw->AddCircleFilled(ImVec2(knobX, start.y + height * 0.5F),
                          (hovered ? 8.4F : 8.0F) * uiScale,
                          ImGui::GetColorU32(knobColor));
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
    static thread_local std::array<char, 64U> name{};
    switch (virtualKey) {
    case 0U: return "Unbound";
    case VK_OEM_7: return "Apostrophe";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_F8: return "F8";
    case VK_F9: return "F9";
    case VK_F10: return "F10";
    case VK_F11: return "F11";
    case VK_F12: return "F12";
    default: break;
    }
    name.fill('\0');
    UINT scan = ::MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT || virtualKey == VK_UP ||
        virtualKey == VK_DOWN || virtualKey == VK_PRIOR || virtualKey == VK_NEXT ||
        virtualKey == VK_END || virtualKey == VK_HOME || virtualKey == VK_INSERT ||
        virtualKey == VK_DELETE || virtualKey == VK_DIVIDE || virtualKey == VK_NUMLOCK) {
        scan |= 0x100U;
    }
    const LONG parameter = static_cast<LONG>(scan << 16U);
    if (::GetKeyNameTextA(parameter, name.data(),
                          static_cast<int>(name.size())) > 0) return name.data();
    std::snprintf(name.data(), name.size(), "VK 0x%02X", virtualKey);
    return name.data();
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
    if (!m_featureSettingsDirty &&
        !m_statsPanelDragging && !m_statsPanelResizing) {
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

void OverlayRenderer::setBlacklistSnapshot(
    const BlacklistOverlaySnapshot& snapshot) noexcept
{
    // Layout changes created by an active drag are first returned to the
    // Controller. Do not let an older pipe snapshot pull the panel backward
    // before that acknowledgement arrives.
    if (!m_blacklistPanelDragging && !m_blacklistPanelResizing)
        m_blacklist = snapshot;
}

bool OverlayRenderer::consumeBlacklistAction(BlacklistAction& action) noexcept
{
    if (!m_blacklistActionDirty) return false;
    action = m_blacklistAction;
    m_blacklistAction = {};
    m_blacklistActionDirty = false;
    return true;
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
    char message[96]{};
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
    const auto movementWarning = [&](const char* feature, const bool wasEnabled,
                                     const bool enabled) noexcept {
        if (wasEnabled == enabled) return;
        if (enabled) {
            char warning[96]{};
            std::snprintf(warning, sizeof(warning),
                "WARNING: %s can cause a server ban. Use only offline.", feature);
            enqueueMessage(warning, false);
        } else enqueueToast(feature, false);
    };
    movementWarning("Scaffold", before.scaffoldEnabled, after.scaffoldEnabled);
    movementWarning("Fly", before.flyEnabled, after.flyEnabled);
    movementWarning("BHop", before.bhopEnabled, after.bhopEnabled);
    if (before.safewalkEnabled != after.safewalkEnabled)
        enqueueToast("Safewalk", after.safewalkEnabled);
    if (before.aimAssistEnabled != after.aimAssistEnabled)
        enqueueToast("Aim Assist", after.aimAssistEnabled);
    if (before.fireballEspEnabled != after.fireballEspEnabled)
        enqueueToast("Fireball ESP", after.fireballEspEnabled);
    if (before.longJumpEnabled != after.longJumpEnabled)
        enqueueToast("LongJump", after.longJumpEnabled);
    if (before.textGuiEnabled != after.textGuiEnabled)
        enqueueToast("Text GUI", after.textGuiEnabled);
    if (before.knockbackPredictionEnabled != after.knockbackPredictionEnabled)
        enqueueToast("Knockback Prediction", after.knockbackPredictionEnabled);
    if (before.bowPredictionEnabled != after.bowPredictionEnabled)
        enqueueToast("Bow Prediction", after.bowPredictionEnabled);
    if (before.localMobAuraEnabled != after.localMobAuraEnabled)
        enqueueToast("Local Mob Aura", after.localMobAuraEnabled);
    if (before.localVelocityEnabled != after.localVelocityEnabled)
        enqueueToast("Local Velocity", after.localVelocityEnabled);
    if (before.fullscreenImeFixEnabled != after.fullscreenImeFixEnabled)
        enqueueToast("Fullscreen IME", after.fullscreenImeFixEnabled);
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

    // Ownership is intentionally persistent for the lifetime of the world,
    // whereas the high-performance bed scanner cache may be briefly empty
    // until a chunk refresh completes. Threat distance must therefore use the
    // locked own-bed coordinates directly and only borrow the paired foot
    // coordinate when the current marker happens to be available.
    int ownBedFootX = snapshot.ownBedX;
    int ownBedFootZ = snapshot.ownBedZ;
    for (std::uint32_t bedIndex = 0U; bedIndex < snapshot.bedMarkerCount; ++bedIndex) {
        const BedMarker& marker = snapshot.bedMarkers[bedIndex];
        if (marker.x == snapshot.ownBedX && marker.y == snapshot.ownBedY &&
            marker.z == snapshot.ownBedZ) {
            ownBedFootX = marker.footX;
            ownBedFootZ = marker.footZ;
            break;
        }
    }

    auto checkEntity = [&](const EntityMarker& entity) {
        if (!entity.player) return;

        // The live entity name/team metadata may disappear for a few frames
        // while an enemy becomes invisible (some transformed clients rebuild
        // their NetworkPlayerInfo wrapper at that point).  The ESP box does
        // not depend on that metadata, so requiring a fresh TAB-name match
        // here made the box remain visible while the bed alert silently
        // dropped the same entity.  Reuse an identity only when it belongs to
        // this exact entity id and this world's locked own bed.  A non-empty,
        // different live name rejects the cache to guard against entity-id
        // reuse.
        ThreatContact* identityContact = nullptr;
        for (ThreatContact& candidate : m_threatContacts) {
            if (candidate.entityId != entity.entityId ||
                candidate.bedX != snapshot.ownBedX ||
                candidate.bedY != snapshot.ownBedY ||
                candidate.bedZ != snapshot.ownBedZ ||
                candidate.teamColor == 'u') {
                continue;
            }
            const bool uuidMatch = entity.uuid[0U] != '\0' &&
                candidate.uuid[0U] != '\0' &&
                std::strcmp(entity.uuid.data(), candidate.uuid.data()) == 0;
            const bool liveNameMissing = entity.playerName[0U] == '\0';
            const bool cachedNameMissing = candidate.playerName[0U] == '\0';
            const bool sameName = !liveNameMissing && !cachedNameMissing &&
                ::_stricmp(entity.playerName.data(),
                           candidate.playerName.data()) == 0;
            if (uuidMatch || liveNameMissing || cachedNameMissing || sameName) {
                identityContact = &candidate;
                break;
            }
        }

        bool persistentRosterKnown = false;
        bool persistentRosterTeammate = false;
        char persistentRosterTeam = 'u';
        std::array<char, 17U> persistentRosterName{};
        if (entity.playerName[0U] != '\0' || entity.uuid[0U] != '\0') {
            for (std::uint32_t index = 0U; index < snapshot.playerCount; ++index) {
                const PlayerIdentity& identity = snapshot.players[index];
                const bool uuidMatch = entity.uuid[0U] != '\0' &&
                    identity.uuid[0U] != '\0' &&
                    std::strcmp(identity.uuid.data(), entity.uuid.data()) == 0;
                const bool nameMatch = entity.playerName[0U] != '\0' &&
                    ::_stricmp(identity.name.data(), entity.playerName.data()) == 0;
                if (uuidMatch || nameMatch) {
                    persistentRosterKnown = true;
                    persistentRosterTeammate = identity.teamColor == snapshot.ownTeam;
                    persistentRosterTeam = identity.teamColor;
                    persistentRosterName = identity.name;
                    break;
                }
            }
        }
        if (!persistentRosterKnown && identityContact != nullptr) {
            persistentRosterKnown = true;
            persistentRosterTeam = identityContact->teamColor;
            persistentRosterTeammate =
                identityContact->teamColor == snapshot.ownTeam;
            persistentRosterName = identityContact->playerName;
        }
        // Threats and API lookups share the same monotonic, colour-validated
        // TAB roster. Uncoloured lobby/start NPCs are never admitted, while a
        // real player remains known through death/respawn TAB gaps.
        if (!persistentRosterKnown) return;
        // The monotonic TAB roster is authoritative once a player has been
        // admitted.  Invisibility removes armour, so live armour colour must
        // never be required to keep a confirmed enemy classified as a threat.
        // This also keeps respawning teammates excluded while their armour is
        // temporarily absent.
        if (persistentRosterTeammate) return;
        if (entity.entityId == snapshot.entityId) return; // Ignore local player

        {
            const double dx = std::min(
                std::abs(entity.currentX - (snapshot.ownBedX + 0.5)),
                std::abs(entity.currentX - (ownBedFootX + 0.5)));
            const double dz = std::min(
                std::abs(entity.currentZ - (snapshot.ownBedZ + 0.5)),
                std::abs(entity.currentZ - (ownBedFootZ + 0.5)));
            const double dy = std::abs(
                entity.currentY - static_cast<double>(snapshot.ownBedY));
            const double distance = std::sqrt(dx * dx + dz * dz + dy * dy);

            ThreatContact* contact = identityContact;
            ThreatContact* oldest = &m_threatContacts.front();
            if (contact == nullptr) {
                for (ThreatContact& candidate : m_threatContacts) {
                    if (candidate.entityId == entity.entityId &&
                        candidate.bedX == snapshot.ownBedX &&
                        candidate.bedY == snapshot.ownBedY &&
                        candidate.bedZ == snapshot.ownBedZ) {
                        contact = &candidate;
                        break;
                    }
                    if (candidate.entityId < 0 ||
                        candidate.lastSeenTick < oldest->lastSeenTick) oldest = &candidate;
                }
            }
            if (contact == nullptr) {
                contact = oldest;
                *contact = {};
                contact->entityId = entity.entityId;
                contact->bedX = snapshot.ownBedX;
                contact->bedY = snapshot.ownBedY;
                contact->bedZ = snapshot.ownBedZ;
            }
            contact->lastSeenTick = now;
            contact->distance = distance;
            contact->teamColor = persistentRosterTeam;
            contact->uuid = entity.uuid;
            if (persistentRosterName[0U] != '\0')
                contact->playerName = persistentRosterName;
            else if (entity.playerName[0U] != '\0')
                contact->playerName = entity.playerName;
            contact->invisible = entity.invisible;
            if (entity.skinTextureId != 0U)
                contact->skinTextureId = entity.skinTextureId;
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
    const float width = 326.0F * uiScale;
    const float height = 68.0F * uiScale;
    const float gap = 10.0F * uiScale;
    std::array<const ThreatContact*, 64U> threats{};
    std::size_t threatCount = 0U;
    for (ThreatContact& contact : m_threatContacts) {
        const float target = contact.inside && contact.entityId >= 0 ? 1.0F : 0.0F;
        contact.presentation += (target - contact.presentation) *
            (1.0F - std::exp(-13.0F * std::clamp(deltaSeconds, 0.0F, 0.05F)));
        if (contact.presentation > 0.005F && contact.entityId >= 0)
            threats[threatCount++] = &contact;
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
        const float slideLinear = std::clamp(contact.presentation, 0.0F, 1.0F);
        const float slideEase = 1.0F - std::pow(1.0F - slideLinear, 3.0F);
        const float outsideX = display.x + 12.0F * uiScale;
        const float x = outsideX + (targetX - outsideX) * slideEase;
        const float y = display.y - 24.0F * uiScale - height -
                        static_cast<float>(index) * (height + gap);
        const ImVec2 min(x, y), max(x + width, y + height);
        draw->AddRectFilled(ImVec2(min.x + 4.0F * uiScale, min.y + 7.0F * uiScale),
                            ImVec2(max.x + 4.0F * uiScale, max.y + 7.0F * uiScale),
                            IM_COL32(0, 0, 0, 75), 15.0F * uiScale);
        draw->AddRectFilled(min, max, IM_COL32(36, 24, 30, 246), 15.0F * uiScale);
        const ImU32 accent = IM_COL32(255, 92, 104, 255);
        const float warningPulse = 0.5F + 0.5F * static_cast<float>(
            std::sin(ImGui::GetTime() * 6.2));
        const int warningAlpha = static_cast<int>(120.0F + warningPulse * 125.0F);
        const float warningThickness = (1.4F + warningPulse * 1.4F) * uiScale;
        draw->AddRect(ImVec2(min.x - 2.0F * uiScale, min.y - 2.0F * uiScale),
                      ImVec2(max.x + 2.0F * uiScale, max.y + 2.0F * uiScale),
                      IM_COL32(255, 58, 76, warningAlpha), 17.0F * uiScale, 0,
                      warningThickness);
        draw->AddRect(ImVec2(min.x - 4.0F * uiScale, min.y - 4.0F * uiScale),
                      ImVec2(max.x + 4.0F * uiScale, max.y + 4.0F * uiScale),
                      IM_COL32(255, 58, 76,
                          static_cast<int>(warningAlpha * 0.28F)),
                      19.0F * uiScale, 0, 1.0F * uiScale);
        draw->AddCircleFilled(ImVec2(min.x + 20.0F * uiScale, min.y + 18.0F * uiScale),
                              10.0F * uiScale, accent);
        const ImVec2 exclamationSize = ImGui::CalcTextSize("!");
        draw->AddText(ImVec2(min.x + 20.0F * uiScale - exclamationSize.x * 0.5F,
                             min.y + 18.0F * uiScale - exclamationSize.y * 0.5F),
                      IM_COL32(255, 255, 255, 255), "!");
        const char* const name = contact.playerName[0U] == '\0'
            ? "Unknown player" : contact.playerName.data();
        const ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(teamColor(contact.teamColor));
        // Minecraft's TextureManager owns this texture in the exact OpenGL
        // context used by the hook. Draw the 8x8 face and hat UV regions from
        // the already-loaded 64x64 skin; no controller download or per-frame
        // upload is necessary.
        const ImVec2 avatarMin(min.x + 39.0F * uiScale, min.y + 14.0F * uiScale);
        const ImVec2 avatarMax(avatarMin.x + 38.0F * uiScale,
                              avatarMin.y + 38.0F * uiScale);
        draw->AddRectFilled(avatarMin, avatarMax, IM_COL32(20, 18, 24, 255),
                            8.0F * uiScale);
        if (contact.skinTextureId != 0U) {
            const ImTextureID skin = reinterpret_cast<ImTextureID>(
                static_cast<std::uintptr_t>(contact.skinTextureId));
            draw->AddImage(skin, avatarMin, avatarMax,
                           ImVec2(8.0F / 64.0F, 8.0F / 64.0F),
                           ImVec2(16.0F / 64.0F, 16.0F / 64.0F));
            draw->AddImage(skin, avatarMin, avatarMax,
                           ImVec2(40.0F / 64.0F, 8.0F / 64.0F),
                           ImVec2(48.0F / 64.0F, 16.0F / 64.0F));
        } else {
            ImVec4 avatarTint = teamColor(contact.teamColor);
            avatarTint.w = 0.34F;
            draw->AddRectFilled(avatarMin, avatarMax,
                                ImGui::ColorConvertFloat4ToU32(avatarTint),
                                8.0F * uiScale);
        }
        draw->AddRect(avatarMin, avatarMax, nameColor, 10.0F * uiScale, 0,
                      1.5F * uiScale);
        const ImVec2 textPos(min.x + 88.0F * uiScale, min.y + 10.0F * uiScale);
        ImFont* const warningFont = m_fonts[static_cast<std::size_t>(
            std::clamp(m_appliedGuiScaleIndex, 0, 3))];
        if (warningFont != nullptr) {
            draw->AddText(warningFont, warningFont->LegacySize * 1.18F,
                          textPos, nameColor, name);
        } else {
            draw->AddText(textPos, nameColor, name);
        }
        char distanceLabel[64]{};
        std::snprintf(distanceLabel, sizeof(distanceLabel),
                      contact.invisible
                          ? "INVIS  |  Enemy near bed  %.1fm / %dm"
                          : "Enemy near bed  %.1fm / %dm",
                      contact.distance,
                      std::clamp(m_features.bedThreatRadius, 3, 32));
        draw->AddText(ImVec2(textPos.x, textPos.y + 28.0F * uiScale),
                      contact.invisible ? IM_COL32(255, 91, 108, 255)
                                        : IM_COL32(235, 224, 232, 255),
                      distanceLabel);
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

    // FontGlobalScale enlarges a single bitmap and becomes visibly blurry.
    // Build four independently rasterized Segoe UI sizes into one atlas and
    // switch the default font only at the safe pre-NewFrame boundary.
    char windowsDirectory[MAX_PATH]{};
    const UINT windowsLength = ::GetWindowsDirectoryA(windowsDirectory, MAX_PATH);
    std::array<char, MAX_PATH> fontPath{};
    std::array<char, MAX_PATH> boldFontPath{};
    if (windowsLength > 0U && windowsLength + 20U < fontPath.size()) {
        std::snprintf(fontPath.data(), fontPath.size(), "%s\\Fonts\\segoeui.ttf",
                      windowsDirectory);
        std::snprintf(boldFontPath.data(), boldFontPath.size(),
                      "%s\\Fonts\\segoeuib.ttf", windowsDirectory);
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
        ImFontConfig boldConfig{};
        boldConfig.OversampleH = 3;
        boldConfig.OversampleV = 2;
        boldConfig.PixelSnapH = false;
        if (boldFontPath[0U] != '\0') {
            m_boldFonts[index] = io.Fonts->AddFontFromFileTTF(
                boldFontPath.data(), fontSizes[index], &boldConfig,
                io.Fonts->GetGlyphRangesDefault());
        }
        if (m_boldFonts[index] == nullptr) m_boldFonts[index] = m_fonts[index];
    }
    // One dedicated CJK font is enough for the transient IME card. Building
    // four full CJK atlases would waste substantial memory inside the game.
    std::array<char, MAX_PATH> imeFontPath{};
    if (windowsLength > 0U && windowsLength + 18U < imeFontPath.size()) {
        std::snprintf(imeFontPath.data(), imeFontPath.size(),
                      "%s\\Fonts\\msyh.ttc", windowsDirectory);
        if (::GetFileAttributesA(imeFontPath.data()) == INVALID_FILE_ATTRIBUTES) {
            std::snprintf(imeFontPath.data(), imeFontPath.size(),
                          "%s\\Fonts\\msjh.ttc", windowsDirectory);
        }
    }
    if (imeFontPath[0U] != '\0' &&
        ::GetFileAttributesA(imeFontPath.data()) != INVALID_FILE_ATTRIBUTES) {
        ImFontConfig imeConfig{};
        imeConfig.OversampleH = 2;
        imeConfig.OversampleV = 2;
        imeConfig.PixelSnapH = false;
        m_imeFont = io.Fonts->AddFontFromFileTTF(
            imeFontPath.data(), 22.0F, &imeConfig,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }

    m_appliedGuiScaleIndex = -1;
    m_animatedGuiScale = guiScaleForIndex(m_guiScaleIndex);
    applyGuiScaleStyle(m_animatedGuiScale, m_guiScaleIndex);

    if (!ImGui_ImplWin32_Init(window)) {
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
        m_fonts = {};
        m_boldFonts = {};
        m_imeFont = nullptr;
        return false;
    }
    if (!ImGui_ImplOpenGL2_Init()) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
        m_fonts = {};
        m_boldFonts = {};
        m_imeFont = nullptr;
        return false;
    }

    m_inputState->imguiContext.store(m_imguiContext, std::memory_order_release);
    m_inputState->window.store(window, std::memory_order_release);
    updateImeState(*m_inputState, window, WM_INPUTLANGCHANGE,
        reinterpret_cast<LPARAM>(::GetKeyboardLayout(
            ::GetWindowThreadProcessId(window, nullptr))));
    m_inputState->acceptImGuiMessages.store(true, std::memory_order_release);
    m_inputState->fallbackPrimed = false;
    DWORD windowProcessId = 0U;
    const DWORD windowThreadId = ::GetWindowThreadProcessId(window, &windowProcessId);
    m_inputState->directImGuiWndProc.store(
        windowThreadId != 0U && windowThreadId == ::GetCurrentThreadId(),
        std::memory_order_release);
    // Minecraft/LWJGL remains the cursor style owner. The overlay only keeps
    // the cursor visible while interactive; it never swaps the game's native
    // pointer for ImGui's smaller default arrow or resize sprites.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
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
    for (BlacklistTexture& cached : m_blacklistTextures) {
        if (cached.texture != 0U) {
            const GLuint texture = static_cast<GLuint>(cached.texture);
            ::glDeleteTextures(1, &texture);
        }
        cached = {};
    }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(m_imguiContext);
    m_imguiContext = nullptr;
    m_fonts = {};
    m_boldFonts = {};
    m_imeFont = nullptr;
    m_blurTexture = 0U;
    m_bedTexture = 0U;
    m_blurWidth = 0;
    m_blurHeight = 0;
    m_blacklistTextures = {};
    m_cursorSessionActive = false;
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
    m_boldFonts = {};
    m_imeFont = nullptr;
    // The old HGLRC is unavailable, so its texture cannot be deleted here.
    // Drop the name to prevent a later context generation from deleting an
    // unrelated object which happens to reuse the same GLuint value.
    m_blurTexture = 0U;
    m_bedTexture = 0U;
    m_blurWidth = 0;
    m_blurHeight = 0;
    m_blacklistTextures = {};
    m_cursorSessionActive = false;
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
    m_boldFonts = {};
    m_imeFont = nullptr;
    m_blacklistTextures = {};
    m_cursorSessionActive = false;
    m_window = nullptr;
    m_glContext = nullptr;
    m_initialized = false;
    m_permanentlyDisabled = true;
}

void OverlayRenderer::abandonAfterHookDisabled() noexcept
{
    abandonForContextChange();
}

void OverlayRenderer::captureBackdropTexture() noexcept
{
    const ImGuiIO& io = ImGui::GetIO();
    const int width = static_cast<int>(io.DisplaySize.x);
    const int height = static_cast<int>(io.DisplaySize.y);
    if (width < 2 || height < 2 || m_backdropCapturedThisFrame) return;

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
    ::glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    ::glPopAttrib();
    m_backdropCapturedThisFrame = true;
}

void OverlayRenderer::renderInventoryBlur(const float strength) noexcept
{
    if (strength <= 0.01F) return;
    const ImGuiIO& io = ImGui::GetIO();
    const int width = static_cast<int>(io.DisplaySize.x);
    const int height = static_cast<int>(io.DisplaySize.y);
    if (width < 2 || height < 2) return;

    captureBackdropTexture();
    if (m_blurTexture == 0U) return;
    ::glPushAttrib(GL_ALL_ATTRIB_BITS);
    ::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_blurTexture));
    // Copy once, then blend weighted centre/near/far samples. The symmetric
    // nine-tap kernel remains compatible with Minecraft 1.8.9's fixed OpenGL2
    // pipeline and avoids an FBO or client-specific GLSL.
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
    struct BlurSample final { float x; float y; float weight; };
    // Preserve the previous kernel's aggregate opacity, but widen its sampling
    // radius. Bilinear filtering turns these wider taps into a stronger frosted
    // glass separation without adding full-screen passes or changing animation
    // timing.
    // but fold the diagonal ring into the bilinear near/far weights. Nine full
    // screen samples instead of thirteen reduce fill bandwidth by about 31%
    // without changing the GUI animation or its perceived blur strength.
    constexpr std::array<BlurSample, 9U> samples{{
        {0.0F, 0.0F, 0.145F},
        {-13.0F, 0.0F, 0.120F}, {13.0F, 0.0F, 0.120F},
        {0.0F, -13.0F, 0.120F}, {0.0F, 13.0F, 0.120F},
        {-29.0F, 0.0F, 0.074F}, {29.0F, 0.0F, 0.074F},
        {0.0F, -29.0F, 0.074F}, {0.0F, 29.0F, 0.074F}}};
    for (const BlurSample& sample : samples) {
        ::glColor4f(1.0F, 1.0F, 1.0F, sample.weight * strength);
        const float x0 = sample.x;
        const float y0 = sample.y;
        const float x1 = static_cast<float>(width) + sample.x;
        const float y1 = static_cast<float>(height) + sample.y;
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

void OverlayRenderer::renderImeOverlay(const float deltaSeconds,
                                       const float uiScale) noexcept
{
    if (m_inputState == nullptr) return;
    std::array<wchar_t, 80U> name{};
    std::array<wchar_t, 128U> composition{};
    std::array<std::array<wchar_t, 64U>, 9U> candidates{};
    std::uint32_t candidateCount = 0U;
    std::uint32_t candidateSelection = 0U;
    bool composing = false;
    ::AcquireSRWLockShared(&m_inputState->imeLock);
    name = m_inputState->imeName;
    composition = m_inputState->imeComposition;
    candidates = m_inputState->imeCandidates;
    candidateCount = m_inputState->imeCandidateCount;
    candidateSelection = m_inputState->imeCandidateSelection;
    composing = m_inputState->imeComposing;
    ::ReleaseSRWLockShared(&m_inputState->imeLock);

    const std::uint64_t revision = m_inputState->imeRevision.load(
        std::memory_order_acquire);
    const std::uint64_t now = ::GetTickCount64();
    if (revision != m_lastImeRevision) {
        m_lastImeRevision = revision;
        m_lastImeActivityTick = now;
    }
    const bool recentlyChanged = m_lastImeActivityTick != 0U &&
        now - m_lastImeActivityTick < 2200U;
    const bool visible = m_features.fullscreenImeFixEnabled &&
        (composing || recentlyChanged);
    const float target = visible ? 1.0F : 0.0F;
    m_imePanelProgress += (target - m_imePanelProgress) *
        (1.0F - std::exp(-12.0F * std::clamp(deltaSeconds, 0.0F, 0.05F)));
    if (m_imePanelProgress < 0.002F) return;

    const auto toUtf8 = [](const wchar_t* const source,
                           char* const destination,
                           const int capacity) noexcept {
        destination[0] = '\0';
        if (source == nullptr || source[0] == L'\0') return;
        (void)::WideCharToMultiByte(CP_UTF8, 0, source, -1,
            destination, capacity, nullptr, nullptr);
        destination[capacity - 1] = '\0';
    };
    std::array<char, 240U> nameUtf8{};
    std::array<char, 384U> compositionUtf8{};
    std::array<std::array<char, 192U>, 9U> candidateUtf8{};
    toUtf8(name.data(), nameUtf8.data(), static_cast<int>(nameUtf8.size()));
    toUtf8(composition.data(), compositionUtf8.data(),
           static_cast<int>(compositionUtf8.size()));
    for (std::size_t index = 0U; index < candidateUtf8.size(); ++index) {
        toUtf8(candidates[index].data(), candidateUtf8[index].data(),
               static_cast<int>(candidateUtf8[index].size()));
    }

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* const draw = ImGui::GetForegroundDrawList();
    const float eased = m_imePanelProgress * m_imePanelProgress *
        (3.0F - 2.0F * m_imePanelProgress);
    const float width = std::min(660.0F * uiScale,
                                 io.DisplaySize.x - 24.0F * uiScale);
    const float rowHeight = candidateCount == 0U ? 0.0F : 35.0F * uiScale;
    const float height = (compositionUtf8[0U] != '\0' ? 92.0F : 66.0F) *
        uiScale + rowHeight;
    const ImVec2 minimum((io.DisplaySize.x - width) * 0.5F,
        (-height - 12.0F * uiScale) * (1.0F - eased) + 18.0F * uiScale);
    const ImVec2 maximum(minimum.x + width, minimum.y + height);
    const int alpha = static_cast<int>(238.0F * eased);
    draw->AddRectFilled(ImVec2(minimum.x - 5.0F * uiScale,
                              minimum.y + 8.0F * uiScale),
                        ImVec2(maximum.x + 5.0F * uiScale,
                              maximum.y + 12.0F * uiScale),
                        IM_COL32(0, 0, 0, static_cast<int>(54.0F * eased)),
                        20.0F * uiScale);
    draw->AddRectFilled(minimum, maximum,
        IM_COL32(20, 21, 28, alpha), 18.0F * uiScale);
    draw->AddRectFilled(minimum,
        ImVec2(minimum.x + 5.0F * uiScale, maximum.y),
        IM_COL32(114, 224, 210, static_cast<int>(255.0F * eased)),
        18.0F * uiScale, ImDrawFlags_RoundCornersLeft);
    ImFont* const font = m_imeFont != nullptr ? m_imeFont : ImGui::GetFont();
    const float fontSize = (m_imeFont != nullptr ? 22.0F : ImGui::GetFontSize()) *
        uiScale;
    draw->AddText(font, fontSize * 0.70F,
        ImVec2(minimum.x + 22.0F * uiScale, minimum.y + 12.0F * uiScale),
        IM_COL32(174, 180, 194, static_cast<int>(255.0F * eased)),
        nameUtf8[0U] != '\0' ? nameUtf8.data() : "Input method");
    if (compositionUtf8[0U] != '\0') {
        draw->AddText(font, fontSize,
            ImVec2(minimum.x + 22.0F * uiScale,
                   minimum.y + 34.0F * uiScale),
            IM_COL32(248, 249, 252, static_cast<int>(255.0F * eased)),
            compositionUtf8.data());
    }
    if (candidateCount != 0U) {
        const float top = maximum.y - rowHeight;
        const float cellWidth = (width - 28.0F * uiScale) /
            static_cast<float>(candidateCount);
        for (std::uint32_t index = 0U; index < candidateCount; ++index) {
            const float left = minimum.x + 14.0F * uiScale +
                cellWidth * static_cast<float>(index);
            if (index == candidateSelection) {
                draw->AddRectFilled(ImVec2(left, top + 2.0F * uiScale),
                    ImVec2(left + cellWidth - 4.0F * uiScale,
                           maximum.y - 5.0F * uiScale),
                    IM_COL32(55, 184, 170, static_cast<int>(105.0F * eased)),
                    9.0F * uiScale);
            }
            char numbered[224]{};
            std::snprintf(numbered, sizeof(numbered), "%u %s",
                index + 1U, candidateUtf8[index].data());
            draw->AddText(font, fontSize * 0.76F,
                ImVec2(left + 7.0F * uiScale, top + 8.0F * uiScale),
                IM_COL32(236, 239, 244, static_cast<int>(255.0F * eased)),
                numbered);
        }
    }
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
        m_inputState->sessionCursor.store(nullptr, std::memory_order_release);
        m_waitingForHotkey = false;
        m_hotkeyCaptureTarget = 0;
        m_statsPanelDragging = false;
        m_statsPanelResizing = false;
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
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (interactive && !m_cursorSessionActive) {
        CURSORINFO cursorInfo{};
        cursorInfo.cbSize = sizeof(cursorInfo);
        HCURSOR cursor = nullptr;
        if (::GetCursorInfo(&cursorInfo) != FALSE &&
            (cursorInfo.flags & CURSOR_SHOWING) != 0U) {
            cursor = cursorInfo.hCursor;
        }
        if (cursor == nullptr) {
            cursor = reinterpret_cast<HCURSOR>(::GetClassLongPtrW(
                window, GCLP_HCURSOR));
        }
        if (cursor == nullptr) cursor = ::LoadCursorW(nullptr, IDC_ARROW);
        m_inputState->sessionCursor.store(cursor, std::memory_order_release);
        m_cursorSessionActive = true;
    } else if (!interactive && m_cursorSessionActive) {
        m_cursorSessionActive = false;
        m_inputState->sessionCursor.store(nullptr, std::memory_order_release);
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    m_backdropCapturedThisFrame = false;

    // Feature hotkeys are sampled only while Minecraft owns foreground focus.
    // While unfocused we mirror physical state into the edge latch, so a key
    // used in another application cannot fire immediately on refocus.
    const bool gameForeground = ::GetForegroundWindow() == window;
    const FeatureSettings featuresBeforeHotkeys = m_features;
    bool hotkeyFeatureChanged = false;
    for (std::size_t index = 0U; index < m_features.featureHotkeys.size(); ++index) {
        const int key = m_features.featureHotkeys[index];
        const bool down = key >= 8 && key <= 254 &&
            (::GetAsyncKeyState(key) & 0x8000) != 0;
        if (gameForeground && !interactive && down && !m_featureHotkeyWasDown[index]) {
            switch (index) {
            case 0: m_features.entityEspEnabled = !m_features.entityEspEnabled; break;
            case 1: m_features.bedEspEnabled = !m_features.bedEspEnabled; break;
            case 2: m_features.nametagEnabled = !m_features.nametagEnabled; break;
            case 3: m_features.bedThreatAlertsEnabled =
                        !m_features.bedThreatAlertsEnabled; break;
            case 4: m_features.safewalkEnabled = !m_features.safewalkEnabled; break;
            case 5: m_features.scaffoldEnabled = !m_features.scaffoldEnabled; break;
            case 6: m_features.flyEnabled = !m_features.flyEnabled; break;
            case 7: m_features.bhopEnabled = !m_features.bhopEnabled; break;
            case 8: m_features.aimAssistEnabled = !m_features.aimAssistEnabled; break;
            case 9: m_features.hypixelPanelEnabled =
                        !m_features.hypixelPanelEnabled; break;
            case 10: m_features.debugChatEnabled = !m_features.debugChatEnabled; break;
            case 11:
                m_blacklist.panelEnabled = !m_blacklist.panelEnabled;
                m_blacklistAction = {};
                m_blacklistAction.type = BlacklistAction::Type::Settings;
                m_blacklistAction.panelEnabled = m_blacklist.panelEnabled;
                m_blacklistAction.matchAlertsEnabled = m_blacklist.matchAlertsEnabled;
                m_blacklistAction.allowIdOnlyNicks = m_blacklist.allowIdOnlyNicks;
                m_blacklistAction.showWithClickGui = m_blacklist.showWithClickGui;
                m_blacklistAction.collapsed = m_blacklist.collapsed;
                m_blacklistAction.panelOpacity = m_blacklist.panelOpacity;
                m_blacklistAction.panelColor = m_blacklist.panelColor;
                m_blacklistActionDirty = true;
                enqueueToast("Blacklist", m_blacklist.panelEnabled);
                break;
            case 12: m_features.textGuiEnabled = !m_features.textGuiEnabled; break;
            case 13: m_features.fireballEspEnabled =
                         !m_features.fireballEspEnabled; break;
            case 14: m_features.longJumpEnabled = !m_features.longJumpEnabled; break;
            default: break;
            }
            if (index != 11U) hotkeyFeatureChanged = true;
        }
        m_featureHotkeyWasDown[index] = down;
    }
    if (hotkeyFeatureChanged) {
        m_features.safewalkHotkey = m_features.featureHotkeys[4U];
        m_featureSettingsDirty = true;
        enqueueFeatureToasts(featuresBeforeHotkeys, m_features);
    }

    const float targetGui = interactive ? 1.0F : 0.0F;
    const float delta = std::clamp(io.DeltaTime, 0.0F, 0.10F);
    // A lightly under-damped spring takes roughly half a second to settle. It
    // is slower than the previous exponential lerp but still responsive, and
    // its single small overshoot provides the requested lightweight rebound.
    // Exact under-damped oscillator integration. At normal frame rates this is
    // visually equivalent to the previous spring, while uneven/slow title-menu
    // frames no longer quantize the motion into Euler steps.
    advancePresentationSpring(m_clickGuiProgress, m_clickGuiVelocity,
                              targetGui, delta);
    const float guiLinear = std::clamp(m_clickGuiProgress, 0.0F, 1.0F);
    const float guiEase = guiLinear * guiLinear * (3.0F - 2.0F * guiLinear);
    renderInventoryBlur(std::clamp(guiEase * 1.28F, 0.0F, 1.0F));
    renderImeOverlay(delta, uiScale);

    updateBedThreatAlerts(snapshot);

    if (snapshot.state == GameSnapshot::State::Ready && snapshot.camera.valid) {
        ImDrawList* const background = ImGui::GetBackgroundDrawList();
        const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        if (m_features.bedEspEnabled) {
            const bool defenseHotkeyDown =
                (::GetAsyncKeyState(std::clamp(m_features.bedDefenseHotkey, 8, 254)) &
                 0x8000) != 0;
            const bool defensePanelsVisible = m_features.bedDefensePanelEnabled &&
                (!m_features.bedDefenseHoldToShow || defenseHotkeyDown);
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

                const bool defenseOwnershipVisible =
                    m_features.showOwnBedDefenseInfo || !snapshot.matchActive ||
                    !snapshot.ownBedKnown || !ownBed;
                if (defensePanelsVisible && bed.defenseCount > 0U &&
                    defenseOwnershipVisible) {
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
                        const double dx = (static_cast<double>(bed.x + bed.footX) + 1.0) *
                            0.5 - snapshot.x;
                        const double dy = static_cast<double>(bed.y) - snapshot.y;
                        const double dz = (static_cast<double>(bed.z + bed.footZ) + 1.0) *
                            0.5 - snapshot.z;
                        const double bedDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
                        const float distanceScale = m_features.bedDefensePerspectiveScale
                            ? std::clamp(static_cast<float>(14.0 / (bedDistance + 4.0)),
                                         0.55F, 1.40F)
                            : 1.0F;
                        const float panelScale = uiScale * distanceScale;
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
                        const int panelAlpha = static_cast<int>(std::lround(
                            std::clamp(m_features.bedDefensePanelOpacity, 0, 100) * 2.55));
                        if (panelAlpha < 250) {
                            captureBackdropTexture();
                            if (m_blurTexture != 0U) {
                                constexpr std::array<ImVec2, 5U> blurOffsets{{
                                    ImVec2(-3.0F, 0.0F), ImVec2(3.0F, 0.0F),
                                    ImVec2(0.0F, -3.0F), ImVec2(0.0F, 3.0F),
                                    ImVec2(0.0F, 0.0F)}};
                                for (const ImVec2 offset : blurOffsets) {
                                    const float sourceLeft = std::clamp(
                                        panelMin.x + offset.x * panelScale,
                                        0.0F, displaySize.x);
                                    const float sourceTop = std::clamp(
                                        panelMin.y + offset.y * panelScale,
                                        0.0F, displaySize.y);
                                    const float sourceRight = std::clamp(
                                        panelMax.x + offset.x * panelScale,
                                        0.0F, displaySize.x);
                                    const float sourceBottom = std::clamp(
                                        panelMax.y + offset.y * panelScale,
                                        0.0F, displaySize.y);
                                    background->AddImageRounded(
                                        reinterpret_cast<ImTextureID>(
                                            static_cast<std::uintptr_t>(m_blurTexture)),
                                        panelMin, panelMax,
                                        ImVec2(sourceLeft / displaySize.x,
                                               1.0F - sourceTop / displaySize.y),
                                        ImVec2(sourceRight / displaySize.x,
                                               1.0F - sourceBottom / displaySize.y),
                                        IM_COL32(255, 255, 255, 48),
                                        12.0F * panelScale);
                                }
                            }
                        }
                        background->AddRectFilled(
                            panelMin, panelMax,
                            packedRgbColor(m_features.bedDefensePanelColor, panelAlpha),
                            12.0F * panelScale);
                        background->AddRect(
                            panelMin, panelMax, IM_COL32(255, 255, 255, 38),
                            12.0F * panelScale, 0, 1.0F * panelScale);
                        
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
        if (m_features.entityEspEnabled || m_features.nametagEnabled ||
            (m_features.fireballEspEnabled && snapshot.integratedSinglePlayer)) {
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
                if (entity.fireball) {
                    if (m_features.fireballEspEnabled &&
                        snapshot.integratedSinglePlayer) {
                        const double renderX = entity.previousX +
                            (entity.currentX - entity.previousX) * renderTick;
                        const double renderY = entity.previousY +
                            (entity.currentY - entity.previousY) * renderTick;
                        const double renderZ = entity.previousZ +
                            (entity.currentZ - entity.previousZ) * renderTick;
                        // EntityLargeFireball's collision box is visually much
                        // larger than its core. A compact fixed cube protects
                        // visibility while still tracking the projectile.
                        constexpr double halfExtent = 0.24;
                        const AxisAlignedBox fireballBox{
                            renderX - halfExtent, renderY - halfExtent,
                            renderZ - halfExtent, renderX + halfExtent,
                            renderY + halfExtent, renderZ + halfExtent};
                        drawProjectedBox(background, snapshot.camera, displaySize,
                            fireballBox, packedRgbColor(m_features.fireballEspColor),
                            m_features.labelsEnabled ? "Fireball" : "",
                            m_features.fireballEspFilled);
                    }
                    continue;
                }
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
                if (m_features.entityEspEnabled && m_features.labelsEnabled &&
                    !(entity.player && m_features.nametagEnabled)) {
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
                // Restore the original live armour-colour classification as
                // the primary signal. Roster metadata is retained only as a
                // fallback for a frame where armour JNI data is unavailable.
                const bool armorTeammate = entity.armorTeam != 'u' &&
                    entity.armorTeam == snapshot.ownTeam;
                const bool rosterTeammate = entity.teamColor != 'u' &&
                    entity.teamColor == snapshot.ownTeam;
                const bool isTeammate = snapshot.matchActive && entity.player &&
                    snapshot.ownTeam != 'u' && (armorTeammate || rosterTeammate);
                if (m_features.entityEspEnabled &&
                    (!isTeammate || m_features.showTeammateBoxes)) {
                    drawProjectedBox(background, snapshot.camera, displaySize, interpolated,
                                     entityColor, label);
                }
                if (m_features.entityEspEnabled && isTeammate &&
                    m_features.showTeammateArrows) {
                    const double centerX = (interpolated.minX + interpolated.maxX) * 0.5;
                    const double centerZ = (interpolated.minZ + interpolated.maxZ) * 0.5;
                    const double topY = interpolated.maxY + 0.95;
                    const ScreenPoint pt = projectPoint(snapshot.camera, displaySize, centerX, topY, centerZ);
                    if (pt.visible) {
                        const char visualTeam = entity.armorTeam != 'u'
                            ? entity.armorTeam : entity.teamColor;
                        const ImVec4 teamAccentVector = teamColor(visualTeam);
                        const ImU32 teamAccent = ImGui::ColorConvertFloat4ToU32(
                            teamAccentVector);
                        const float halfWidth = 11.0F * uiScale;
                        const float height = halfWidth * 1.7320508F;
                        const ImVec2 tip(pt.x, pt.y);
                        const ImVec2 left(pt.x - halfWidth, pt.y - height);
                        const ImVec2 right(pt.x + halfWidth, pt.y - height);
                        drawRoundedTriangle(background,
                            ImVec2(tip.x + 1.5F * uiScale, tip.y + 3.0F * uiScale),
                            ImVec2(left.x + 1.5F * uiScale, left.y + 3.0F * uiScale),
                            ImVec2(right.x + 1.5F * uiScale, right.y + 3.0F * uiScale),
                            4.6F * uiScale, IM_COL32(0, 0, 0, 92));
                        // The inner triangle is a similarity transform around
                        // the equilateral triangle's centroid. Unlike manually
                        // moving only its tip, this is exactly equivalent to
                        // offsetting all three edges inward by the same amount,
                        // so the white shell has uniform thickness everywhere.
                        drawRoundedTriangle(background, tip, left, right,
                                            4.6F * uiScale,
                                            IM_COL32(255, 255, 255, 245));
                        const float borderThickness = 2.8F * uiScale;
                        const float inradius = height / 3.0F;
                        const float innerScale = std::clamp(
                            1.0F - borderThickness / inradius, 0.25F, 0.90F);
                        const ImVec2 centroid(pt.x, pt.y - height * (2.0F / 3.0F));
                        const auto insetVertex = [&](const ImVec2 vertex) noexcept {
                            return ImVec2(
                                centroid.x + (vertex.x - centroid.x) * innerScale,
                                centroid.y + (vertex.y - centroid.y) * innerScale);
                        };
                        drawRoundedTriangle(
                            background, insetVertex(tip), insetVertex(left),
                            insetVertex(right), 3.2F * uiScale, teamAccent);
                    }
                }
                const bool nametagAllowed = entity.player && entity.confirmedPlayer &&
                    m_features.nametagEnabled && entity.playerName[0U] != '\0' &&
                    (!isTeammate || m_features.showTeammateNametags) &&
                    (isTeammate || !m_features.nametagNearbyEnemiesOnly ||
                     entity.distance <= static_cast<double>(m_features.nametagRange));
                if (nametagAllowed &&
                    entity.playerName[0U] != '\0') {
                    ImVec2 bodyMin{}, bodyMax{};
                    if (projectedBoxBounds(snapshot.camera, displaySize, interpolated,
                                           bodyMin, bodyMax)) {
                        constexpr std::array<float, 4U> sizePresets{{
                            0.86F, 1.0F, 1.16F, 1.34F}};
                        const float tagScale = std::clamp(uiScale, 0.88F, 1.34F) *
                            sizePresets[static_cast<std::size_t>(
                                std::clamp(m_features.nametagSizeIndex, 0, 3))];
                        const int potion = entity.heldItemDamage & 0x3FFF;
                        const bool knownSpecial = entity.heldItemId == 388 ||
                            entity.heldItemId == 264 || entity.heldItemId == 46 ||
                            entity.heldItemId == 385 ||
                            (entity.heldItemId == 373 &&
                             (potion == 8206 || potion == 8270));
                        const bool enemy = snapshot.ownTeam != 'u' && !isTeammate;
                        const bool showSpecial = m_features.enemyItemIndicatorsEnabled &&
                            enemy && knownSpecial;
                        const float width = 184.0F * tagScale;
                        const float height = (showSpecial ? 62.0F : 48.0F) * tagScale;
                        float x = (bodyMin.x + bodyMax.x - width) * 0.5F;
                        float y = bodyMin.y - height - 8.0F * tagScale;
                        if (m_features.nametagSidePlacement) {
                            const bool placeRight = (bodyMin.x + bodyMax.x) * 0.5F <
                                                    displaySize.x * 0.5F;
                            x = placeRight ? bodyMax.x + 9.0F * tagScale
                                           : bodyMin.x - width - 9.0F * tagScale;
                            y = (bodyMin.y + bodyMax.y - height) * 0.5F;
                        }
                        x = std::clamp(x, 4.0F,
                            std::max(4.0F, displaySize.x - width - 4.0F));
                        y = std::clamp(y, 4.0F,
                            std::max(4.0F, displaySize.y - height - 4.0F));
                        const ImVec2 minimum(x, y), maximum(x + width, y + height);
                        background->AddRectFilled(
                            ImVec2(x + 2.0F * tagScale, y + 4.0F * tagScale),
                            ImVec2(maximum.x + 2.0F * tagScale,
                                   maximum.y + 4.0F * tagScale),
                            IM_COL32(0, 0, 0, 74), 11.0F * tagScale);
                        background->AddRectFilled(minimum, maximum,
                            packedRgbColor(m_features.nametagPanelColor,
                                static_cast<int>(std::lround(static_cast<float>(
                                    std::clamp(m_features.nametagPanelOpacity,
                                               10, 100)) * 2.55F))),
                            11.0F * tagScale);
                        background->AddRect(minimum, maximum,
                            IM_COL32(255, 255, 255, 34), 11.0F * tagScale);
                        if (enemy && m_features.nametagTeamPulse) {
                            const float pulse = 0.5F + 0.5F * static_cast<float>(
                                std::sin(ImGui::GetTime() * 5.8));
                            ImVec4 pulseColor = teamColor(entity.teamColor != 'u'
                                ? entity.teamColor : entity.armorTeam);
                            pulseColor.w = 0.42F + pulse * 0.50F;
                            background->AddRect(
                                ImVec2(minimum.x - (1.0F + pulse) * tagScale,
                                       minimum.y - (1.0F + pulse) * tagScale),
                                ImVec2(maximum.x + (1.0F + pulse) * tagScale,
                                       maximum.y + (1.0F + pulse) * tagScale),
                                ImGui::ColorConvertFloat4ToU32(pulseColor),
                                12.0F * tagScale, 0,
                                (1.2F + 1.3F * pulse) * tagScale);
                        }

                        const ImVec2 faceMin(x + 8.0F * tagScale,
                                             y + 7.0F * tagScale);
                        const ImVec2 faceMax(faceMin.x + 34.0F * tagScale,
                                             faceMin.y + 34.0F * tagScale);
                        background->AddRectFilled(faceMin, faceMax,
                            IM_COL32(24, 23, 29, 255), 7.0F * tagScale);
                        if (entity.skinTextureId != 0U) {
                            const ImTextureID skin = reinterpret_cast<ImTextureID>(
                                static_cast<std::uintptr_t>(entity.skinTextureId));
                            background->AddImage(skin, faceMin, faceMax,
                                ImVec2(8.0F / 64.0F, 8.0F / 64.0F),
                                ImVec2(16.0F / 64.0F, 16.0F / 64.0F));
                            background->AddImage(skin, faceMin, faceMax,
                                ImVec2(40.0F / 64.0F, 8.0F / 64.0F),
                                ImVec2(48.0F / 64.0F, 16.0F / 64.0F));
                        }
                        background->AddRect(faceMin, faceMax,
                            IM_COL32(255, 255, 255, 42), 7.0F * tagScale);

                        NametagAnimation* animation = nullptr;
                        NametagAnimation* oldest = &m_nametagAnimations.front();
                        const std::uint64_t now = static_cast<std::uint64_t>(::GetTickCount64());
                        for (NametagAnimation& candidate : m_nametagAnimations) {
                            if (candidate.entityId == entity.entityId) {
                                animation = &candidate;
                                break;
                            }
                            if (candidate.entityId < 0 ||
                                candidate.lastSeenTick < oldest->lastSeenTick) oldest = &candidate;
                        }
                        if (animation == nullptr) {
                            animation = oldest;
                            *animation = {};
                            animation->entityId = entity.entityId;
                            animation->displayedHealth = entity.health;
                        }
                        animation->lastSeenTick = now;
                        animation->displayedHealth +=
                            (entity.health - animation->displayedHealth) *
                            (1.0F - std::exp(-11.0F * delta));
                        const float ratio = std::clamp(animation->displayedHealth /
                            std::max(1.0F, entity.maxHealth), 0.0F, 1.0F);
                        const std::size_t nametagFontIndex = static_cast<std::size_t>(
                            std::clamp(m_features.nametagSizeIndex, 0, 3));
                        ImFont* const font = m_boldFonts[nametagFontIndex] != nullptr
                            ? m_boldFonts[nametagFontIndex] : ImGui::GetFont();
                        const float fontSize = font->LegacySize;
                        const ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(
                            teamColor(entity.teamColor != 'u'
                                ? entity.teamColor : entity.armorTeam));
                        background->AddText(font, fontSize,
                            ImVec2(faceMax.x + 8.0F * tagScale,
                                   y + 7.0F * tagScale), nameColor,
                            entity.playerName.data());
                        if (entity.protectionLevel > 0U) {
                            char protection[16]{};
                            std::snprintf(protection, sizeof(protection), "Prot %s",
                                          protectionRoman(entity.protectionLevel));
                            const ImVec2 size = font->CalcTextSizeA(
                                fontSize * 0.76F, FLT_MAX, 0.0F, protection);
                            background->AddText(font, fontSize * 0.76F,
                                ImVec2(maximum.x - size.x - 7.0F * tagScale,
                                       y + 8.0F * tagScale),
                                IM_COL32(196, 176, 255, 245), protection);
                        }
                        char healthText[16]{};
                        std::snprintf(healthText, sizeof(healthText), "%.1f",
                                      animation->displayedHealth);
                        const float healthFontSize = fontSize * 0.76F;
                        const ImVec2 healthTextSize = font->CalcTextSizeA(
                            healthFontSize, FLT_MAX, 0.0F, healthText);
                        const ImVec2 barMin(faceMax.x + 8.0F * tagScale,
                                            y + 29.0F * tagScale);
                        const ImVec2 barMax(maximum.x - 13.0F * tagScale -
                                                healthTextSize.x,
                                            barMin.y + 7.0F * tagScale);
                        background->AddRectFilled(barMin, barMax,
                            IM_COL32(255, 255, 255, 28), 3.5F * tagScale);
                        const ImU32 hpColor = ratio > 0.60F ? IM_COL32(78, 220, 121, 255)
                            : (ratio > 0.30F ? IM_COL32(255, 190, 62, 255)
                                             : IM_COL32(255, 76, 92, 255));
                        background->AddRectFilled(barMin,
                            ImVec2(barMin.x + (barMax.x - barMin.x) * ratio, barMax.y),
                            hpColor, 3.5F * tagScale);
                        background->AddText(font, healthFontSize,
                            ImVec2(maximum.x - 7.0F * tagScale - healthTextSize.x,
                                   barMin.y - (healthTextSize.y -
                                               (barMax.y - barMin.y)) * 0.5F),
                            hpColor, healthText);
                        if (showSpecial) {
                            const char* item = entity.heldItemId == 388 ? "EMERALD"
                                : entity.heldItemId == 264 ? "DIAMOND"
                                : entity.heldItemId == 46 ? "TNT"
                                : entity.heldItemId == 385 ? "FIREBALL" : "INVIS";
                            char itemText[32]{};
                            std::snprintf(itemText, sizeof(itemText), "%s x%u", item,
                                static_cast<unsigned>(std::max<std::uint8_t>(
                                    1U, entity.heldItemCount)));
                            background->AddText(font, fontSize * 0.76F,
                                ImVec2(faceMax.x + 8.0F * tagScale,
                                       y + 44.0F * tagScale),
                                entity.heldItemId == 373
                                    ? IM_COL32(255, 93, 113, 255)
                                    : IM_COL32(105, 218, 240, 255), itemText);
                        }
                    }
                }
            }
        }

        // Knockback prediction is visual-only and is fed by the bounded 20 Hz
        // snapshot. A detected impulse starts one animation; subsequent samples
        // refine its path without restarting it, then the landing box lingers.
        const double trajectoryNow = ImGui::GetTime();
        if (m_features.knockbackPredictionEnabled &&
            snapshot.entitySampleGeneration != m_lastKnockbackGeneration) {
            m_lastKnockbackGeneration = snapshot.entitySampleGeneration;
            for (std::uint8_t predictionIndex = 0U;
                 predictionIndex < snapshot.knockbackTrajectoryCount;
                 ++predictionIndex) {
                const KnockbackTrajectory& prediction =
                    snapshot.knockbackTrajectories[predictionIndex];
                KnockbackVisual* visual = nullptr;
                KnockbackVisual* oldest = &m_knockbackVisuals.front();
                for (KnockbackVisual& candidate : m_knockbackVisuals) {
                    if (candidate.active &&
                        candidate.trajectory.entityId == prediction.entityId) {
                        visual = &candidate;
                        break;
                    }
                    if (!candidate.active) oldest = &candidate;
                    else if (candidate.updatedAt < oldest->updatedAt) oldest = &candidate;
                }
                if (visual == nullptr) {
                    visual = oldest;
                    *visual = {};
                    visual->startedAt = trajectoryNow;
                    visual->active = true;
                    visual->trajectory = prediction;
                } else if (trajectoryNow - visual->updatedAt > 0.42) {
                    // A later impulse on the same entity is a new event.
                    visual->startedAt = trajectoryNow;
                    visual->trajectory = prediction;
                }
                // Keep the first trajectory immutable throughout one impulse.
                // Re-basing it to the entity's newer mid-flight position while
                // preserving animation time would make the box jump forward.
                visual->updatedAt = trajectoryNow;
            }
        }
        for (KnockbackVisual& visual : m_knockbackVisuals) {
            if (!m_features.knockbackPredictionEnabled) {
                visual.active = false;
                continue;
            }
            if (!visual.active || visual.trajectory.pointCount < 2U) continue;
            const KnockbackTrajectory& prediction = visual.trajectory;
            const float duration = std::max(0.28F,
                static_cast<float>(prediction.pointCount - 1U) * 0.045F);
            const float age = static_cast<float>(trajectoryNow - visual.startedAt);
            if (age > duration + 2.7F) {
                visual.active = false;
                continue;
            }
            const ImU32 trajectoryColor = IM_COL32(255, 184, 72, 220);
            const std::size_t visiblePoint = std::min<std::size_t>(
                prediction.pointCount - 1U,
                static_cast<std::size_t>(std::floor(std::clamp(
                    age / duration, 0.0F, 1.0F) *
                    static_cast<float>(prediction.pointCount - 1U))));
            for (std::size_t point = 1U; point <= visiblePoint; ++point) {
                const ScreenPoint first = projectPoint(snapshot.camera, displaySize,
                    prediction.points[point - 1U].x,
                    prediction.points[point - 1U].y + 0.9,
                    prediction.points[point - 1U].z);
                const ScreenPoint second = projectPoint(snapshot.camera, displaySize,
                    prediction.points[point].x,
                    prediction.points[point].y + 0.9,
                    prediction.points[point].z);
                if (first.visible && second.visible)
                    background->AddLine(ImVec2(first.x, first.y),
                        ImVec2(second.x, second.y), trajectoryColor, 2.0F);
            }
            const auto translatedBox = [&](const WorldPoint& point) noexcept {
                const WorldPoint& origin = prediction.points[0U];
                const double offsetX = point.x - origin.x;
                const double offsetY = point.y - origin.y;
                const double offsetZ = point.z - origin.z;
                return AxisAlignedBox{
                    prediction.startBounds.minX + offsetX,
                    prediction.startBounds.minY + offsetY,
                    prediction.startBounds.minZ + offsetZ,
                    prediction.startBounds.maxX + offsetX,
                    prediction.startBounds.maxY + offsetY,
                    prediction.startBounds.maxZ + offsetZ};
            };
            if (age < duration) {
                const float exactIndex = std::clamp(age / duration, 0.0F, 1.0F) *
                    static_cast<float>(prediction.pointCount - 1U);
                const std::size_t lower = std::min<std::size_t>(
                    static_cast<std::size_t>(std::floor(exactIndex)),
                    prediction.pointCount - 1U);
                const std::size_t upper = std::min<std::size_t>(
                    lower + 1U, prediction.pointCount - 1U);
                const double blend = static_cast<double>(exactIndex -
                    static_cast<float>(lower));
                const WorldPoint animated{
                    prediction.points[lower].x +
                        (prediction.points[upper].x - prediction.points[lower].x) * blend,
                    prediction.points[lower].y +
                        (prediction.points[upper].y - prediction.points[lower].y) * blend,
                    prediction.points[lower].z +
                        (prediction.points[upper].z - prediction.points[lower].z) * blend};
                drawProjectedBox(background, snapshot.camera, displaySize,
                    translatedBox(animated), trajectoryColor, "", true);
            } else if (prediction.landed) {
                const float fade = std::clamp(
                    1.0F - (age - duration) / 2.7F, 0.0F, 1.0F);
                drawProjectedBox(background, snapshot.camera, displaySize,
                    translatedBox(prediction.points[
                        prediction.pointCount - 1U]),
                    IM_COL32(255, 184, 72,
                        static_cast<int>(220.0F * fade)), "", true);
            }
        }

        // The bow path is recomputed at a bounded cadence from the exact 1.8.9
        // charge/drag/gravity constants. The terminal marker turns red only
        // when the swept segment intersects a player AABB.
        if (m_features.bowPredictionEnabled && snapshot.bowTrajectory.active &&
            snapshot.bowTrajectory.pointCount >= 2U) {
            const BowTrajectory& targetBow = snapshot.bowTrajectory;
            if (!m_bowVisualInitialized || !m_bowVisualTrajectory.active) {
                m_bowVisualTrajectory = targetBow;
                m_bowVisualInitialized = true;
            } else {
                // JNI physics remains bounded to Minecraft's 20 TPS, while
                // every OpenGL frame eases the already computed POD path
                // toward the newest result. This preserves block collision
                // accuracy without performing world JNI calls at 240 Hz.
                const float blend = 1.0F - std::exp(-24.0F * delta);
                const std::uint8_t common = std::min(
                    m_bowVisualTrajectory.pointCount, targetBow.pointCount);
                for (std::uint8_t point = 0U; point < common; ++point) {
                    WorldPoint& visualPoint = m_bowVisualTrajectory.points[point];
                    const WorldPoint& targetPoint = targetBow.points[point];
                    visualPoint.x += (targetPoint.x - visualPoint.x) * blend;
                    visualPoint.y += (targetPoint.y - visualPoint.y) * blend;
                    visualPoint.z += (targetPoint.z - visualPoint.z) * blend;
                }
                for (std::uint8_t point = common; point < targetBow.pointCount; ++point)
                    m_bowVisualTrajectory.points[point] = targetBow.points[point];
                m_bowVisualTrajectory.pointCount = targetBow.pointCount;
                m_bowVisualTrajectory.impact = targetBow.impact;
                m_bowVisualTrajectory.impactEntityId = targetBow.impactEntityId;
                m_bowVisualTrajectory.hasImpact = targetBow.hasImpact;
                m_bowVisualTrajectory.impactPlayer = targetBow.impactPlayer;
                m_bowVisualTrajectory.active = true;
            }
            const BowTrajectory& bow = m_bowVisualTrajectory;
            for (std::size_t point = 1U; point < bow.pointCount; ++point) {
                const ScreenPoint first = projectPoint(snapshot.camera, displaySize,
                    bow.points[point - 1U].x, bow.points[point - 1U].y,
                    bow.points[point - 1U].z);
                const ScreenPoint second = projectPoint(snapshot.camera, displaySize,
                    bow.points[point].x, bow.points[point].y, bow.points[point].z);
                if (first.visible && second.visible)
                    background->AddLine(ImVec2(first.x, first.y),
                        ImVec2(second.x, second.y), IM_COL32(92, 220, 255, 225),
                        2.0F);
            }
            if (bow.hasImpact) {
                constexpr double impactHalf = 0.16;
                const AxisAlignedBox impactBox{
                    bow.impact.x - impactHalf, bow.impact.y - impactHalf,
                    bow.impact.z - impactHalf, bow.impact.x + impactHalf,
                    bow.impact.y + impactHalf, bow.impact.z + impactHalf};
                const ImU32 impactColor = bow.impactPlayer
                    ? IM_COL32(255, 70, 86, 255)
                    : IM_COL32(92, 220, 255, 255);
                drawProjectedBox(background, snapshot.camera, displaySize,
                    impactBox, impactColor,
                    bow.impactPlayer ? "PLAYER IMPACT" : "IMPACT", true);
            }
        } else {
            m_bowVisualInitialized = false;
            m_bowVisualTrajectory = {};
        }
    }

    // Modern two-pane Click GUI. The left rail exposes stable feature pages;
    // each page owns its top-right master switch and all related detail
    // settings. Categories are labels, never collapsing containers, so every
    // function remains one click away.
    const float baseGuiWidth = 820.0F * static_cast<float>(std::clamp(
        m_features.clickGuiWidthPercent, 80, 150)) / 100.0F;
    // Keep the navigation rail usable at the smallest height while still
    // allowing generous vertical expansion on larger displays.
    const float baseGuiHeight = std::max(620.0F, 650.0F * static_cast<float>(
        std::clamp(m_features.clickGuiHeightPercent, 80, 150)) / 100.0F);
    constexpr float baseRailWidth = 198.0F;
    const float guiWidth = baseGuiWidth * uiScale;
    const float guiHeight = baseGuiHeight * uiScale;
    if (m_clickGuiX < -9000.0F) {
        m_clickGuiX = std::max(8.0F, (io.DisplaySize.x - guiWidth) * 0.5F);
        m_clickGuiY = std::max(8.0F, (io.DisplaySize.y - guiHeight) * 0.16F);
    }
    m_clickGuiThemeProgress +=
        ((m_features.clickGuiLightTheme ? 1.0F : 0.0F) - m_clickGuiThemeProgress) *
        (1.0F - std::exp(-12.0F * delta));
    const auto mixColor = [](const ImVec4& dark, const ImVec4& light,
                             const float amount) noexcept {
        return ImVec4(dark.x + (light.x - dark.x) * amount,
                      dark.y + (light.y - dark.y) * amount,
                      dark.z + (light.z - dark.z) * amount,
                      dark.w + (light.w - dark.w) * amount);
    };
    const float theme = std::clamp(m_clickGuiThemeProgress, 0.0F, 1.0F);
    ImVec4 guiSurface = mixColor(ImVec4(0.075F, 0.068F, 0.096F, 0.98F),
                                ImVec4(0.965F, 0.956F, 0.975F, 0.98F), theme);
    ImVec4 guiRail = mixColor(ImVec4(0.055F, 0.050F, 0.073F, 1.0F),
                             ImVec4(0.918F, 0.906F, 0.938F, 1.0F), theme);
    const float guiOpacity = static_cast<float>(std::clamp(
        m_features.clickGuiOpacity, 35, 100)) / 100.0F;
    guiSurface.w = guiOpacity;
    guiRail.w = std::min(1.0F, guiOpacity + 0.03F);
    const ImVec4 guiText = mixColor(ImVec4(0.94F, 0.92F, 0.98F, 1.0F),
                                   ImVec4(0.10F, 0.09F, 0.13F, 1.0F), theme);
    const ImVec4 guiMuted = mixColor(ImVec4(0.59F, 0.56F, 0.65F, 1.0F),
                                    ImVec4(0.39F, 0.36F, 0.44F, 1.0F), theme);
    const ImVec4 guiFrame = mixColor(ImVec4(0.14F, 0.13F, 0.18F, 1.0F),
                                    ImVec4(0.88F, 0.86F, 0.90F, 1.0F), theme);
    const ImVec4 guiScrollbarTrack = mixColor(
        ImVec4(0.065F, 0.058F, 0.082F, 0.72F),
        ImVec4(0.91F, 0.895F, 0.925F, 0.82F), theme);
    const ImVec4 guiScrollbarGrab = mixColor(
        ImVec4(0.42F, 0.38F, 0.50F, 0.92F),
        ImVec4(0.45F, 0.40F, 0.50F, 0.92F), theme);
    const std::array<float, 3U> accentChannels = unpackRgb(
        m_features.clickGuiAccentColor);
    const ImVec4 guiAccent(accentChannels[0U], accentChannels[1U],
                           accentChannels[2U], 1.0F);

    if (m_clickGuiProgress > 0.008F) {
        // iPadOS Spotlight-inspired materialization: the surface forms around
        // its centre with a short decelerating scale/lensing motion and fully
        // reversible opacity. Keeping every child inside this one transformed
        // parent also prevents the navigation rail from surviving a close.
        // Opacity/blur use the same temporal envelope as geometry. The former
        // fourth-power decelerate reached near-opaque too early and made an
        // otherwise longer motion still look abrupt.
        const float spotlightEase = guiEase;
        // Drive geometry directly from the spring, including its overshoot.
        // A second eased/sine motion would rebound a fraction of a beat later.
        const float spotlightScale = 1.26F - 0.26F * m_clickGuiProgress;
        const ImVec2 guiCenter(m_clickGuiX + guiWidth * 0.5F,
                               m_clickGuiY + guiHeight * 0.5F);
        // Layout always uses the final rectangle. Once ImGui has generated the
        // complete draw lists, they are transformed around guiCenter as one
        // flat image. This removes every top-left layout origin from the visual
        // path and makes all four directions perfectly symmetric.
        ImGui::SetNextWindowPos(ImVec2(m_clickGuiX, m_clickGuiY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(guiWidth, guiHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, spotlightEase);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 20.0F * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, guiSurface);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, guiText);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, guiMuted);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, guiFrame);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                              mixColor(ImVec4(0.19F, 0.17F, 0.24F, 1.0F),
                                       ImVec4(0.82F, 0.79F, 0.85F, 1.0F), theme));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, guiAccent);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, guiAccent);
        ImGui::PushStyleColor(ImGuiCol_Button, guiFrame);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(guiAccent.x, guiAccent.y, guiAccent.z, 0.76F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, guiAccent);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, guiScrollbarTrack);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, guiScrollbarGrab);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,
                              mixColor(guiScrollbarGrab, guiAccent, 0.42F));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, guiAccent);
        const ImGuiWindowFlags clickFlags = ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##McOverlayClickGui", nullptr, clickFlags)) {
            const FeatureSettings featuresBefore = m_features;
            bool changed = false;
            // ESP master is intentionally retired. Individual Player/Bed ESP
            // pages are the authoritative switches.
            if (!m_features.espEnabled) {
                m_features.espEnabled = true;
                changed = true;
            }

            const ImVec2 windowPosition = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            ImDrawList* const windowDraw = ImGui::GetWindowDrawList();
            const int parentContentVertexStart = 0;
            ImDrawList* settingsDraw = nullptr;
            int settingsVertexStart = 0;
            int settingsVertexEnd = 0;
            ImDrawList* const backgroundDraw = ImGui::GetBackgroundDrawList();
            const int shadowVertexStart = backgroundDraw->VtxBuffer.Size;
            const auto fadedGuiColor = [&](ImVec4 color) noexcept {
                color.w *= spotlightEase;
                return ImGui::ColorConvertFloat4ToU32(color);
            };
            windowDraw->PushClipRect(windowPosition,
                ImVec2(windowPosition.x + windowSize.x,
                       windowPosition.y + windowSize.y), true);
            for (int shadow = 4; shadow >= 1; --shadow) {
                const float spread = static_cast<float>(shadow) * 4.0F * uiScale;
                backgroundDraw->AddRectFilled(
                    ImVec2(windowPosition.x - spread,
                           windowPosition.y - spread + 7.0F * uiScale),
                    ImVec2(windowPosition.x + windowSize.x + spread,
                           windowPosition.y + windowSize.y + spread + 7.0F * uiScale),
                    IM_COL32(4, 3, 8, static_cast<int>(14.0F * spotlightEase)),
                    20.0F * uiScale + spread);
            }
            windowDraw->AddRectFilled(
                windowPosition,
                ImVec2(windowPosition.x + baseRailWidth * uiScale,
                       windowPosition.y + windowSize.y),
                fadedGuiColor(guiRail), 20.0F * uiScale,
                ImDrawFlags_RoundCornersLeft);
            windowDraw->AddLine(
                ImVec2(windowPosition.x + baseRailWidth * uiScale,
                       windowPosition.y + 18.0F * uiScale),
                ImVec2(windowPosition.x + baseRailWidth * uiScale,
                       windowPosition.y + windowSize.y - 18.0F * uiScale),
                fadedGuiColor(mixColor(
                    ImVec4(1, 1, 1, 0.08F), ImVec4(0, 0, 0, 0.10F), theme)));

            // Header drag surface spans both panes without stealing controls.
            ImGui::SetCursorScreenPos(ImVec2(windowPosition.x + 14.0F * uiScale,
                                              windowPosition.y + 8.0F * uiScale));
            ImGui::InvisibleButton("##clickGuiDrag",
                                   ImVec2((baseGuiWidth - 150.0F) * uiScale,
                                          34.0F * uiScale));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                m_clickGuiX += io.MouseDelta.x;
                m_clickGuiY += io.MouseDelta.y;
                const float maxX = std::max(4.0F, io.DisplaySize.x - guiWidth - 4.0F);
                const float maxY = std::max(4.0F, io.DisplaySize.y - guiHeight - 4.0F);
                m_clickGuiX = std::clamp(m_clickGuiX, 4.0F, maxX);
                m_clickGuiY = std::clamp(m_clickGuiY, 4.0F, maxY);
            }

            ImFont* const boldFont = m_boldFonts[static_cast<std::size_t>(
                std::clamp(m_guiScaleIndex, 0, 3))] != nullptr
                ? m_boldFonts[static_cast<std::size_t>(
                    std::clamp(m_guiScaleIndex, 0, 3))] : ImGui::GetFont();
            windowDraw->AddText(boldFont, ImGui::GetFontSize(),
                                ImVec2(windowPosition.x + 20.0F * uiScale,
                                       windowPosition.y + 17.0F * uiScale),
                                fadedGuiColor(guiText), "MC OVERLAY");
            windowDraw->AddText(ImVec2(windowPosition.x + 20.0F * uiScale,
                                       windowPosition.y + 38.0F * uiScale),
                                fadedGuiColor(guiMuted),
                                "Native workspace");

            struct NavItem final { const char* label; int page; float y; };
            const float railLayoutScale = std::clamp(
                (baseGuiHeight - 80.0F - 68.0F) / (650.0F - 68.0F),
                0.86F, 1.15F);
            const auto railY = [&](const float original) noexcept {
                return 68.0F + (original - 68.0F) * railLayoutScale;
            };
            const std::array<NavItem, 19U> navItems{{
                {"Player ESP", 0, railY(84.0F)}, {"Bed ESP", 1, railY(108.0F)},
                {"Nametag", 2, railY(132.0F)}, {"Fireball ESP", 14, railY(156.0F)},
                {"Bed Alert", 3, railY(205.0F)}, {"Safewalk", 4, railY(250.0F)},
                {"Scaffold", 5, railY(274.0F)}, {"Fly", 6, railY(298.0F)},
                {"BHop", 7, railY(322.0F)}, {"LongJump", 15, railY(346.0F)},
                {"Aim Assist", 8, railY(382.0F)}, {"Local Combat", 17, railY(406.0F)},
                {"Prediction", 16, railY(443.0F)}, {"Player Stats", 9, railY(484.0F)},
                {"Debug", 10, railY(508.0F)}, {"Blacklist", 11, railY(532.0F)},
                {"Text GUI", 12, railY(574.0F)},
                {"Fullscreen IME", 18, railY(612.0F)},
                {"Interface", 13, railY(650.0F)}}};
            const std::array<std::pair<const char*, float>, 9U> navGroups{{
                {"ESP", railY(68.0F)}, {"ALERT", railY(189.0F)},
                {"SAFE", railY(234.0F)}, {"COMBAT", railY(366.0F)},
                {"PREDICTION", railY(427.0F)}, {"DATA", railY(468.0F)},
                {"HUD", railY(558.0F)}, {"FIX", railY(596.0F)},
                {"APPLICATION", railY(634.0F)}}};
            float targetNavY = navItems.front().y;
            for (const NavItem& item : navItems)
                if (item.page == m_clickGuiPage) { targetNavY = item.y; break; }
            if (m_clickGuiNavPosition <= 0.0F) m_clickGuiNavPosition = targetNavY;
            m_clickGuiNavPosition += (targetNavY - m_clickGuiNavPosition) *
                (1.0F - std::exp(-15.0F * delta));
            windowDraw->AddRectFilled(
                ImVec2(windowPosition.x + 10.0F * uiScale,
                       windowPosition.y + (m_clickGuiNavPosition - 2.0F) * uiScale),
                ImVec2(windowPosition.x + (baseRailWidth - 10.0F) * uiScale,
                       windowPosition.y + (m_clickGuiNavPosition + 22.0F) * uiScale),
                fadedGuiColor(ImVec4(
                    guiAccent.x, guiAccent.y, guiAccent.z, 0.18F)),
                10.0F * uiScale);
            windowDraw->AddRectFilled(
                ImVec2(windowPosition.x + 12.0F * uiScale,
                       windowPosition.y + (m_clickGuiNavPosition + 3.0F) * uiScale),
                ImVec2(windowPosition.x + 15.0F * uiScale,
                       windowPosition.y + (m_clickGuiNavPosition + 17.0F) * uiScale),
                fadedGuiColor(guiAccent), 1.5F * uiScale);
            for (const auto& group : navGroups) {
                windowDraw->AddText(boldFont, ImGui::GetFontSize() * 0.72F,
                    ImVec2(windowPosition.x + 20.0F * uiScale,
                           windowPosition.y + group.second * uiScale),
                    fadedGuiColor(guiMuted), group.first);
            }
            for (const NavItem& item : navItems) {
                ImGui::SetCursorScreenPos(ImVec2(
                    windowPosition.x + 10.0F * uiScale,
                    windowPosition.y + (item.y - 2.0F) * uiScale));
                ImGui::PushID(item.page);
                if (ImGui::InvisibleButton("##nav", ImVec2(
                    (baseRailWidth - 20.0F) * uiScale, 24.0F * uiScale))) {
                    if (m_clickGuiPage != item.page) {
                        m_previousClickGuiPage = m_clickGuiPage;
                        m_clickGuiPage = item.page;
                        m_clickGuiPageProgress = 0.0F;
                    }
                }
                const bool navSelected = m_clickGuiPage == item.page;
                float& hoverProgress = m_clickGuiNavHover[static_cast<std::size_t>(item.page)];
                const float hoverTarget = ImGui::IsItemHovered() && !navSelected ? 1.0F : 0.0F;
                hoverProgress += (hoverTarget - hoverProgress) *
                    (1.0F - std::exp(-18.0F * delta));
                ImGui::PopID();
                if (hoverProgress > 0.005F) {
                    windowDraw->AddRectFilled(
                        ImVec2(windowPosition.x + 10.0F * uiScale,
                               windowPosition.y + (item.y - 2.0F) * uiScale),
                        ImVec2(windowPosition.x + (baseRailWidth - 10.0F) * uiScale,
                               windowPosition.y + (item.y + 22.0F) * uiScale),
                        fadedGuiColor(ImVec4(
                            guiAccent.x, guiAccent.y, guiAccent.z,
                            0.10F * hoverProgress)), 10.0F * uiScale);
                }
                const ImU32 itemColor = fadedGuiColor(
                    navSelected ? guiText : guiMuted);
                windowDraw->AddText(
                    navSelected ? boldFont : ImGui::GetFont(),
                    ImGui::GetFontSize(),
                    ImVec2(windowPosition.x + 25.0F * uiScale,
                           windowPosition.y + (item.y + 2.0F) * uiScale),
                    itemColor, item.label);
            }

            // Bottom-left sun/moon control is vector drawn, so it remains crisp
            // and does not depend on an icon font.
            const ImVec2 themeButtonMin(windowPosition.x + 18.0F * uiScale,
                                        windowPosition.y + (baseGuiHeight - 39.0F) * uiScale);
            ImGui::SetCursorScreenPos(themeButtonMin);
            if (ImGui::InvisibleButton("##themeToggle",
                                       ImVec2(28.0F * uiScale, 26.0F * uiScale))) {
                m_features.clickGuiLightTheme = !m_features.clickGuiLightTheme;
                changed = true;
            }
            const ImVec2 iconCenter(themeButtonMin.x + 13.0F * uiScale,
                                    themeButtonMin.y + 13.0F * uiScale);
            const ImU32 iconColor = fadedGuiColor(guiText);
            if (m_features.clickGuiLightTheme) {
                windowDraw->AddCircleFilled(iconCenter, 3.5F * uiScale, iconColor, 20);
                for (int ray = 0; ray < 8; ++ray) {
                    const float angle = static_cast<float>(ray) * 3.14159265F / 4.0F;
                    windowDraw->AddLine(
                        ImVec2(iconCenter.x + std::cos(angle) * 6.0F * uiScale,
                               iconCenter.y + std::sin(angle) * 6.0F * uiScale),
                        ImVec2(iconCenter.x + std::cos(angle) * 8.0F * uiScale,
                               iconCenter.y + std::sin(angle) * 8.0F * uiScale),
                        iconColor, 1.25F * uiScale);
                }
            } else {
                windowDraw->AddCircleFilled(iconCenter, 6.5F * uiScale, iconColor, 24);
                windowDraw->AddCircleFilled(
                    ImVec2(iconCenter.x + 3.0F * uiScale,
                           iconCenter.y - 2.0F * uiScale),
                    5.7F * uiScale, fadedGuiColor(guiRail), 24);
            }

            constexpr std::array<const char*, 19U> pageTitles{{
                "Player ESP", "Bed ESP", "Nametag", "Bed Alert",
                "Safewalk", "Scaffold", "Fly", "BHop", "Aim Assist",
                "Player Stats", "Debug", "Blacklist", "Text GUI", "Interface",
                "Fireball ESP", "LongJump", "Prediction", "Local Combat",
                "Fullscreen IME"}};
            constexpr std::array<const char*, 19U> pageDescriptions{{
                "Player outlines and teammate presentation",
                "Bed geometry and defense material card",
                "Confirmed-player identity and live health cards",
                "Persistent own-bed proximity warning",
                "Edge-aware crouch assistance and release timing",
                "Predictive hotbar block placement",
                "Local movement flight controls",
                "Air momentum and landing jump controls",
                "Crosshair slowdown or smooth target assistance",
                "Automatic TAB roster statistics",
                "Local diagnostics visible only to you",
                "UUID-based player records and encounter warnings",
                "Draggable enabled-feature list",
                "Appearance, scale and input binding",
                "Compact local-world ghast fireball boxes",
                "Single-player forward jump impulse",
                "High-confidence knockback and smooth bow paths",
                "Hard-gated integrated-world hostile-mob test tools",
                "Windows IME status and candidate overlay for fullscreen"}};
            const int page = std::clamp(m_clickGuiPage, 0, 18);
            const float contentX = windowPosition.x + (baseRailWidth + 22.0F) * uiScale;
            windowDraw->AddText(boldFont, ImGui::GetFontSize() * 1.16F,
                ImVec2(contentX, windowPosition.y + 17.0F * uiScale),
                fadedGuiColor(guiText),
                pageTitles[static_cast<std::size_t>(page)]);
            windowDraw->AddText(
                ImVec2(contentX, windowPosition.y + 42.0F * uiScale),
                fadedGuiColor(guiMuted),
                pageDescriptions[static_cast<std::size_t>(page)]);
            windowDraw->AddLine(
                ImVec2(contentX, windowPosition.y + 66.0F * uiScale),
                ImVec2(windowPosition.x + (baseGuiWidth - 20.0F) * uiScale,
                       windowPosition.y + 66.0F * uiScale),
                fadedGuiColor(mixColor(
                    ImVec4(1, 1, 1, 0.09F), ImVec4(0, 0, 0, 0.10F), theme)));

            bool* pageMaster = nullptr;
            float* pageMasterAnimation = nullptr;
            switch (page) {
            case 0: pageMaster = &m_features.entityEspEnabled;
                    pageMasterAnimation = &m_toggleAnimation[1]; break;
            case 1: pageMaster = &m_features.bedEspEnabled;
                    pageMasterAnimation = &m_toggleAnimation[2]; break;
            case 2: pageMaster = &m_features.nametagEnabled;
                    pageMasterAnimation = &m_toggleAnimation[16]; break;
            case 3: pageMaster = &m_features.bedThreatAlertsEnabled;
                    pageMasterAnimation = &m_toggleAnimation[5]; break;
            case 4: pageMaster = &m_features.safewalkEnabled;
                    pageMasterAnimation = &m_toggleAnimation[26]; break;
            case 5: pageMaster = &m_features.scaffoldEnabled;
                    pageMasterAnimation = &m_toggleAnimation[27]; break;
            case 6: pageMaster = &m_features.flyEnabled;
                    pageMasterAnimation = &m_toggleAnimation[28]; break;
            case 7: pageMaster = &m_features.bhopEnabled;
                    pageMasterAnimation = &m_toggleAnimation[29]; break;
            case 8: pageMaster = &m_features.aimAssistEnabled;
                    pageMasterAnimation = &m_toggleAnimation[30]; break;
            case 9: pageMaster = &m_features.hypixelPanelEnabled;
                    pageMasterAnimation = &m_toggleAnimation[4]; break;
            case 10: pageMaster = &m_features.debugChatEnabled;
                    pageMasterAnimation = &m_toggleAnimation[11]; break;
            case 11: pageMaster = &m_blacklist.showWithClickGui;
                     pageMasterAnimation = &m_toggleAnimation[33]; break;
            case 12: pageMaster = &m_features.textGuiEnabled;
                     pageMasterAnimation = &m_toggleAnimation[31]; break;
            case 14: pageMaster = &m_features.fireballEspEnabled;
                     pageMasterAnimation = &m_toggleAnimation[34]; break;
            case 15: pageMaster = &m_features.longJumpEnabled;
                     pageMasterAnimation = &m_toggleAnimation[35]; break;
            case 18: pageMaster = &m_features.fullscreenImeFixEnabled;
                     pageMasterAnimation = &m_toggleAnimation[44]; break;
            default: break;
            }
            if (pageMaster != nullptr && pageMasterAnimation != nullptr) {
                ImGui::SetCursorScreenPos(ImVec2(
                    windowPosition.x + (baseGuiWidth - 126.0F) * uiScale,
                    windowPosition.y + 20.0F * uiScale));
                const bool masterChanged = animatedToggle(
                    "Enabled", *pageMaster, *pageMasterAnimation, uiScale);
                if (page == 11 && masterChanged) {
                    m_blacklistAction = {};
                    m_blacklistAction.type = BlacklistAction::Type::Settings;
                    m_blacklistAction.panelEnabled = m_blacklist.panelEnabled;
                    m_blacklistAction.matchAlertsEnabled = m_blacklist.matchAlertsEnabled;
                    m_blacklistAction.allowIdOnlyNicks = m_blacklist.allowIdOnlyNicks;
                    m_blacklistAction.showWithClickGui = m_blacklist.showWithClickGui;
                    m_blacklistAction.collapsed = m_blacklist.collapsed;
                    m_blacklistAction.panelOpacity = m_blacklist.panelOpacity;
                    m_blacklistAction.panelColor = m_blacklist.panelColor;
                    m_blacklistActionDirty = true;
                } else {
                    changed |= masterChanged;
                }
            } else {
                windowDraw->AddText(boldFont, ImGui::GetFontSize() * 0.78F,
                    ImVec2(windowPosition.x + (baseGuiWidth - 79.0F) * uiScale,
                           windowPosition.y + 27.0F * uiScale),
                    fadedGuiColor(guiAccent), "SYSTEM");
            }

            auto beginHotkeyCapture = [&](const int target) noexcept {
                m_waitingForHotkey = true;
                m_hotkeyCaptureTarget = target;
                m_hotkeyCaptureCooldownFrames = 2;
                m_hotkeyCaptureArmed = false;
                m_inputState->captureHotkey.store(false, std::memory_order_release);
                m_inputState->capturedHotkey.store(0U, std::memory_order_release);
            };
            if (m_waitingForHotkey) {
                const unsigned captured = m_inputState->capturedHotkey.exchange(
                    0U, std::memory_order_acq_rel);
                if (captured != 0U) {
                    if (captured != VK_ESCAPE) {
                        if (m_hotkeyCaptureTarget == 1) {
                            setMenuHotkey(captured);
                            m_menuHotkeyDirty = true;
                        } else if (m_hotkeyCaptureTarget == 2) {
                            m_features.bedDefenseHotkey = static_cast<int>(captured);
                            changed = true;
                        } else if (m_hotkeyCaptureTarget == 3) {
                            m_features.hypixelPanelHotkey = static_cast<int>(captured);
                            changed = true;
                        } else if (m_hotkeyCaptureTarget == 4) {
                            m_features.safewalkHotkey = static_cast<int>(captured);
                            changed = true;
                        } else if (m_hotkeyCaptureTarget >= 100 &&
                                   m_hotkeyCaptureTarget < 115) {
                            const std::size_t featureIndex = static_cast<std::size_t>(
                                m_hotkeyCaptureTarget - 100);
                            for (int& configured : m_features.featureHotkeys)
                                if (configured == static_cast<int>(captured)) configured = 0;
                            m_features.featureHotkeys[featureIndex] =
                                static_cast<int>(captured);
                            if (featureIndex == 4U)
                                m_features.safewalkHotkey = static_cast<int>(captured);
                            changed = true;
                        }
                    }
                    m_waitingForHotkey = false;
                    m_hotkeyCaptureTarget = 0;
                    m_hotkeyCaptureArmed = false;
                    m_inputState->captureHotkey.store(false, std::memory_order_release);
                } else {
                    const bool mouseHeld = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
                        (::GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
                    if (mouseHeld) {
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

            m_clickGuiPageProgress += (1.0F - m_clickGuiPageProgress) *
                (1.0F - std::exp(-14.0F * delta));
            const float pageEase = std::clamp(m_clickGuiPageProgress, 0.0F, 1.0F);
            const ImVec2 childPos(contentX + (1.0F - pageEase) * 14.0F * uiScale,
                                  windowPosition.y + 79.0F * uiScale);
            ImGui::SetCursorScreenPos(childPos);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, guiEase * pageEase);
            ImGui::BeginChild("##settingsPage",
                ImVec2((baseGuiWidth - baseRailWidth - 38.0F) * uiScale,
                       (baseGuiHeight - 96.0F) * uiScale), false,
                ImGuiWindowFlags_AlwaysVerticalScrollbar);
            settingsDraw = ImGui::GetWindowDrawList();
            settingsVertexStart = 0;
            const auto sectionTitle = [&](const char* text) noexcept {
                ImGui::Spacing();
                ImGui::PushFont(boldFont);
                ImGui::TextColored(guiAccent, "%s", text);
                ImGui::PopFont();
                ImGui::Spacing();
            };
            const auto hotkeyControl = [&](const char* label, const int target,
                                           const unsigned key) noexcept {
                ImGui::TextDisabled("%s", label);
                ImGui::SameLine(0.0F, 14.0F * uiScale);
                const char* buttonText = m_waitingForHotkey &&
                    m_hotkeyCaptureTarget == target ? "Press a key..." : hotkeyName(key);
                if (ImGui::Button(buttonText, ImVec2(126.0F * uiScale, 0.0F)))
                    beginHotkeyCapture(target);
            };

            const int featureHotkeyIndex = page <= 12 ? page
                : (page == 14 ? 13 : (page == 15 ? 14 : -1));
            if (featureHotkeyIndex >= 0) {
                hotkeyControl("Feature hotkey", 100 + featureHotkeyIndex,
                    static_cast<unsigned>(std::max(0,
                        m_features.featureHotkeys[static_cast<std::size_t>(
                            featureHotkeyIndex)])));
                ImGui::Spacing();
            }

            if (page == 0) {
                sectionTitle("TARGETS");
                changed |= animatedToggle("Players only",
                    m_features.entityEspPlayersOnly, m_toggleAnimation[8], uiScale);
                changed |= animatedToggle("Show teammate boxes",
                    m_features.showTeammateBoxes, m_toggleAnimation[12], uiScale);
                changed |= animatedToggle("Show teammate arrows",
                    m_features.showTeammateArrows, m_toggleAnimation[25], uiScale);
                changed |= animatedToggle("World labels",
                    m_features.labelsEnabled, m_toggleAnimation[3], uiScale);
                sectionTitle("APPEARANCE");
                std::array<float, 3U> playerColor = unpackRgb(m_features.playerEspColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Player box color", playerColor.data(),
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.playerEspColor = packRgb(playerColor);
                    changed = true;
                }
            } else if (page == 1) {
                sectionTitle("BED GEOMETRY");
                changed |= animatedToggle("Automatic bed refresh",
                    m_features.bedAutoRefreshEnabled, m_toggleAnimation[7], uiScale);
                changed |= animatedToggle("Semi-transparent box fill",
                    m_features.bedEspFilled, m_toggleAnimation[9], uiScale);
                std::array<float, 3U> bedColor = unpackRgb(m_features.bedEspColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Bed box color", bedColor.data(),
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.bedEspColor = packRgb(bedColor);
                    changed = true;
                }
                if (ImGui::Button("Refresh loaded beds now",
                                  ImVec2(210.0F * uiScale, 0.0F)))
                    m_bedRescanPending = true;
                sectionTitle("DEFENSE MATERIAL CARD");
                changed |= animatedToggle("Show defense material card",
                    m_features.bedDefensePanelEnabled, m_toggleAnimation[6], uiScale);
                changed |= animatedToggle("Include own bed",
                    m_features.showOwnBedDefenseInfo, m_toggleAnimation[10], uiScale);
                changed |= animatedToggle("Hold key to show",
                    m_features.bedDefenseHoldToShow, m_toggleAnimation[13], uiScale);
                hotkeyControl("Hold bind", 2,
                    static_cast<unsigned>(m_features.bedDefenseHotkey));
                changed |= animatedToggle("Distance-scaled card",
                    m_features.bedDefensePerspectiveScale, m_toggleAnimation[14], uiScale);
                ImGui::TextDisabled("Block radius (bed level and above)");
                for (int radius = 3; radius <= 10; ++radius) {
                    if (radius != 3) ImGui::SameLine();
                    ImGui::PushID(radius);
                    const bool selected = radius == m_features.bedDefenseRadius;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                    char label[4]{};
                    std::snprintf(label, sizeof(label), "%d", radius);
                    if (ImGui::Button(label, ImVec2(38.0F * uiScale, 0.0F)) && !selected) {
                        m_features.bedDefenseRadius = radius;
                        changed = true;
                    }
                    if (selected) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                std::array<float, 3U> cardColor = unpackRgb(
                    m_features.bedDefensePanelColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Card background", cardColor.data(),
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.bedDefensePanelColor = packRgb(cardColor);
                    changed = true;
                }
                ImGui::SetNextItemWidth(300.0F * uiScale);
                changed |= ImGui::SliderInt("Card opacity",
                    &m_features.bedDefensePanelOpacity, 0, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
            } else if (page == 2) {
                sectionTitle("PLAYER FILTER");
                changed |= animatedToggle("Show teammate nametags",
                    m_features.showTeammateNametags, m_toggleAnimation[19], uiScale);
                changed |= animatedToggle("Only nearby enemies",
                    m_features.nametagNearbyEnemiesOnly, m_toggleAnimation[20], uiScale);
                if (m_features.nametagNearbyEnemiesOnly) {
                    ImGui::SetNextItemWidth(320.0F * uiScale);
                    changed |= ImGui::SliderInt("Enemy range", &m_features.nametagRange,
                        4, 128, "%d blocks", ImGuiSliderFlags_AlwaysClamp);
                }
                ImGui::TextDisabled("Only colour-validated TAB players are shown; shop NPCs are excluded.");
                sectionTitle("LAYOUT & SIZE");
                changed |= animatedToggle("Smart side placement",
                    m_features.nametagSidePlacement, m_toggleAnimation[17], uiScale);
                changed |= animatedToggle("Enemy team pulse",
                    m_features.nametagTeamPulse, m_toggleAnimation[21], uiScale);
                constexpr std::array<const char*, 4U> nametagSizes{{"S", "M", "L", "XL"}};
                ImGui::TextDisabled("Card size");
                for (int sizeIndex = 0; sizeIndex < 4; ++sizeIndex) {
                    if (sizeIndex != 0) ImGui::SameLine();
                    const bool selected = sizeIndex == m_features.nametagSizeIndex;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                    if (ImGui::Button(nametagSizes[static_cast<std::size_t>(sizeIndex)],
                        ImVec2(62.0F * uiScale, 0.0F)) && !selected) {
                        m_features.nametagSizeIndex = sizeIndex;
                        changed = true;
                    }
                    if (selected) ImGui::PopStyleColor();
                }
                sectionTitle("SURFACE");
                std::array<float, 3U> nametagColor = unpackRgb(
                    m_features.nametagPanelColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Nametag background", nametagColor.data(),
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.nametagPanelColor = packRgb(nametagColor);
                    changed = true;
                }
                ImGui::SetNextItemWidth(300.0F * uiScale);
                changed |= ImGui::SliderInt("Nametag opacity",
                    &m_features.nametagPanelOpacity, 10, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                sectionTitle("OBSERVED EQUIPMENT");
                changed |= animatedToggle("Show held special item",
                    m_features.enemyItemIndicatorsEnabled, m_toggleAnimation[18], uiScale);
                ImGui::TextWrapped("Minecraft servers do not send another player's private inventory. Fireballs, TNT, diamonds, emeralds and invisibility potions can only be reported while visibly held.");
            } else if (page == 3) {
                sectionTitle("THREAT RULE");
                ImGui::TextWrapped("Track confirmed enemy roster members around your locked own bed.");
                ImGui::Spacing();
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Warning range",
                    &m_features.bedThreatRadius, 3, 32, "%d blocks",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::Spacing();
                ImGui::TextDisabled("The warning remains visible while a tracked enemy stays inside the range.");
            } else if (page == 4) {
                sectionTitle("EDGE ASSIST");
                ImGui::TextWrapped(
                    "Uses Minecraft's native inset-AABB ledge rule and the player's real next-tick motion, then holds the normal sneak key state.");
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Edge timing",
                    &m_features.safewalkEdgeSensitivity, 0, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Minimum look pitch",
                    &m_features.safewalkMinimumPitch, -90, 90, "%d deg",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Stand delay after placement",
                    &m_features.safewalkReleaseDelayMs, 0, 750, "%d ms",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextDisabled(
                    "0%% = earlier guard; 100%% = last safe body margin. Neither waits until the live hitbox is unsupported.");
            } else if (page == 5) {
                sectionTitle("AUTOMATIC PLACEMENT");
                ImGui::TextWrapped(
                    "Places a real hotbar block below/predictively ahead of your movement. Wool, planks, sandstone and stable building blocks are accepted; sand and gravel are always excluded.");
                ImGui::Spacing();
                changed |= animatedToggle("Keep placement on takeoff layer",
                    m_features.scaffoldSameLayerOnly,
                    m_toggleAnimation[43], uiScale);
                ImGui::TextDisabled(
                    "When enabled, jumping bridges forward on the takeoff layer instead of towering upward.");
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.34F, 1.0F),
                    "WARNING: Do not use this on a server. It can cause a ban.");
                ImGui::TextDisabled("Placement still requires a reachable solid neighbour.");
            } else if (page == 6) {
                sectionTitle("FLIGHT SPEED");
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Speed", &m_features.flySpeedPercent,
                    10, 500, "%d%%", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.34F, 1.0F),
                    "WARNING: Do not use this on a server. It can cause a ban.");
                ImGui::TextDisabled("This is local motion control and contains no server-correction bypass.");
            } else if (page == 7) {
                sectionTitle("AIR CONTROL");
                changed |= animatedToggle("Auto-jump on landing",
                    m_features.bhopAutoJump, m_toggleAnimation[32], uiScale);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Air speed",
                    &m_features.bhopAirSpeedPercent, 10, 300, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextColored(ImVec4(1.0F, 0.42F, 0.34F, 1.0F),
                    "WARNING: Do not use this on a server. It can cause a ban.");
                ImGui::TextDisabled("Airborne horizontal velocity follows current movement input.");
            } else if (page == 8) {
                sectionTitle("MODE");
                const bool slowdown = m_features.aimSlowdownMode;
                if (slowdown) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                if (ImGui::Button("Crosshair slowdown", ImVec2(180.0F * uiScale, 0.0F)) &&
                    !slowdown) { m_features.aimSlowdownMode = true; changed = true; }
                if (slowdown) ImGui::PopStyleColor();
                ImGui::SameLine();
                if (!slowdown) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                if (ImGui::Button("Smooth assist", ImVec2(150.0F * uiScale, 0.0F)) &&
                    slowdown) { m_features.aimSlowdownMode = false; changed = true; }
                if (!slowdown) ImGui::PopStyleColor();
                sectionTitle("RESPONSE");
                ImGui::SetNextItemWidth(320.0F * uiScale);
                if (m_features.aimSlowdownMode)
                    changed |= ImGui::SliderInt("Sensitivity coefficient",
                        &m_features.aimSlowdownPercent, 5, 95, "%d%%",
                        ImGuiSliderFlags_AlwaysClamp);
                else
                    changed |= ImGui::SliderInt("Aim speed",
                        &m_features.aimSpeedPercent, 1, 100, "%d%%",
                        ImGuiSliderFlags_AlwaysClamp);
                changed |= animatedToggle("Prioritize nearest target",
                    m_features.aimNearestPriority, m_toggleAnimation[37], uiScale);
                sectionTitle("TARGET WINDOW");
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Minimum distance",
                    &m_features.aimMinimumDistance, 0,
                    std::max(0, m_features.aimMaximumDistance - 1), "%d blocks",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Maximum distance",
                    &m_features.aimMaximumDistance,
                    std::max(1, m_features.aimMinimumDistance), 128, "%d blocks",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Field of view",
                    &m_features.aimFovDegrees, 1, 360, "%d deg",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextDisabled("Only colour-validated enemy TAB players are eligible targets.");
            } else if (page == 9) {
                sectionTitle("VISIBILITY");
                changed |= animatedToggle("Hold key to show roster",
                    m_features.hypixelPanelHoldToShow, m_toggleAnimation[15], uiScale);
                hotkeyControl("Panel bind", 3,
                    static_cast<unsigned>(m_features.hypixelPanelHotkey));
                sectionTitle("CARD SURFACE");
                ImGui::TextDisabled("Background");
                const bool black = m_features.hypixelPanelColor != 0xFFFFFFU;
                if (black) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                if (ImGui::Button("Black", ImVec2(104.0F * uiScale, 0.0F)) && !black) {
                    m_features.hypixelPanelColor = 0x000000U;
                    changed = true;
                }
                if (black) ImGui::PopStyleColor();
                ImGui::SameLine();
                if (!black) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                if (ImGui::Button("White", ImVec2(104.0F * uiScale, 0.0F)) && black) {
                    m_features.hypixelPanelColor = 0xFFFFFFU;
                    changed = true;
                }
                if (!black) ImGui::PopStyleColor();
                ImGui::SetNextItemWidth(300.0F * uiScale);
                changed |= ImGui::SliderInt("Panel opacity",
                    &m_features.hypixelPanelOpacity, 0, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                std::array<float, 3U> statsRailColor = unpackRgb(
                    m_features.hypixelRailColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("STATS rail", statsRailColor.data(),
                        ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.hypixelRailColor = packRgb(statsRailColor);
                    changed = true;
                }
                ImGui::SetNextItemWidth(300.0F * uiScale);
                changed |= ImGui::SliderInt("Rail opacity",
                    &m_features.hypixelRailOpacity, 0, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextDisabled("Font size");
                constexpr std::array<const char*, 4U> statsFonts{{"S", "M", "L", "XL"}};
                for (int fontIndex = 0; fontIndex < 4; ++fontIndex) {
                    if (fontIndex != 0) ImGui::SameLine();
                    const bool selected = fontIndex == m_features.hypixelPanelFontIndex;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                    if (ImGui::Button(statsFonts[static_cast<std::size_t>(fontIndex)],
                        ImVec2(62.0F * uiScale, 0.0F)) && !selected) {
                        m_features.hypixelPanelFontIndex = fontIndex;
                        changed = true;
                    }
                    if (selected) ImGui::PopStyleColor();
                }
                ImGui::Text("Panel size: %d%% W  /  %d%% H",
                            m_features.hypixelPanelScale,
                            m_features.hypixelPanelHeight);
                ImGui::SameLine(0.0F, 16.0F * uiScale);
                if (ImGui::Button("Reset position & size")) {
                    m_features.hypixelPanelScale = 100;
                    m_features.hypixelPanelHeight = 100;
                    m_features.hypixelPanelX = -1;
                    m_features.hypixelPanelY = -1;
                    changed = true;
                }
                ImGui::TextDisabled("Open the GUI, drag the column header to move, or drag the lower-right handle to resize.");
                sectionTitle("MANUAL LOOKUP");
                ImGui::SetNextItemWidth(280.0F * uiScale);
                ImGui::InputTextWithHint("##hypixelPlayer", "Minecraft player name",
                    m_hypixelInput.data(), m_hypixelInput.size());
                ImGui::SameLine();
                if (ImGui::Button("Query", ImVec2(90.0F * uiScale, 0.0F))) {
                    const std::string_view playerId(m_hypixelInput.data());
                    const bool valid = !playerId.empty() && playerId.size() <= 16U &&
                        std::all_of(playerId.begin(), playerId.end(), [](const char c) noexcept {
                            return (c >= 'A' && c <= 'Z') ||
                                   (c >= 'a' && c <= 'z') ||
                                   (c >= '0' && c <= '9') || c == '_';
                        });
                    if (valid) {
                        m_hypixelQuery.fill('\0');
                        std::copy(playerId.begin(), playerId.end(), m_hypixelQuery.begin());
                        m_hypixelQueryPending = true;
                    }
                }
            } else if (page == 10) {
                sectionTitle("LOCAL CHAT");
                ImGui::TextWrapped("Match probes, roster teams, teammate decisions, own-bed ownership and automatic statistics requests are written only to your local chat.");
                ImGui::Spacing();
                ImGui::TextDisabled("Disable the page switch above for a clean normal session.");
            } else if (page == 11) {
                const auto publishBlacklistSettings = [&]() noexcept {
                    m_blacklistAction = {};
                    m_blacklistAction.type = BlacklistAction::Type::Settings;
                    m_blacklistAction.panelEnabled = m_blacklist.panelEnabled;
                    m_blacklistAction.matchAlertsEnabled = m_blacklist.matchAlertsEnabled;
                    m_blacklistAction.allowIdOnlyNicks = m_blacklist.allowIdOnlyNicks;
                    m_blacklistAction.showWithClickGui = m_blacklist.showWithClickGui;
                    m_blacklistAction.collapsed = m_blacklist.collapsed;
                    m_blacklistAction.panelOpacity = m_blacklist.panelOpacity;
                    m_blacklistAction.panelColor = m_blacklist.panelColor;
                    m_blacklistActionDirty = true;
                };
                sectionTitle("ENCOUNTER POLICY");
                bool blacklistSettingsChanged = false;
                blacklistSettingsChanged |= animatedToggle(
                    "Warn once when a match starts", m_blacklist.matchAlertsEnabled,
                    m_toggleAnimation[23], uiScale);
                blacklistSettingsChanged |= animatedToggle(
                    "Allow ID-only records for nicks", m_blacklist.allowIdOnlyNicks,
                    m_toggleAnimation[24], uiScale);
                sectionTitle("PANEL SURFACE");
                blacklistSettingsChanged |= animatedToggle(
                    "Show panel in game", m_blacklist.panelEnabled,
                    m_toggleAnimation[22], uiScale);
                ImGui::TextDisabled(
                    "The page master controls whether the panel stays visible with Click GUI.");
                std::array<float, 3U> blacklistColor = unpackRgb(m_blacklist.panelColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Panel background", blacklistColor.data(),
                        ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_DisplayRGB)) {
                    m_blacklist.panelColor = packRgb(blacklistColor);
                    blacklistSettingsChanged = true;
                }
                ImGui::SetNextItemWidth(300.0F * uiScale);
                blacklistSettingsChanged |= ImGui::SliderInt(
                    "Panel opacity", &m_blacklist.panelOpacity, 0, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                if (blacklistSettingsChanged) publishBlacklistSettings();

                sectionTitle("ADD RECENT PLAYER");
                if (ImGui::Button("+ Add player",
                                  ImVec2(130.0F * uiScale, 0.0F))) {
                    m_blacklistAddOpen = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Opens a separate animated dialog.");

                sectionTitle("SAVED PLAYERS");
                ImGui::BeginChild("##blacklistEntries", ImVec2(0.0F, 190.0F * uiScale),
                                  false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
                for (std::uint32_t index = 0U; index < m_blacklist.count; ++index) {
                    const BlacklistEntry& entry = m_blacklist.entries[index];
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::PushFont(boldFont);
                    ImGui::TextUnformatted(entry.name.data());
                    ImGui::PopFont();
                    if (entry.nick) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.84F, 0.48F, 1.0F, 1.0F), "NICK");
                    }
                    ImGui::TextWrapped("%s", entry.reason.data());
                    bool warning = entry.warnOnEncounter;
                    if (ImGui::Checkbox("Encounter warning", &warning)) {
                        m_blacklistAction = {};
                        m_blacklistAction.type = BlacklistAction::Type::Warning;
                        std::snprintf(m_blacklistAction.key.data(),
                            m_blacklistAction.key.size(), "%s", entry.key.data());
                        m_blacklistAction.warnOnEncounter = warning;
                        m_blacklistActionDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        m_blacklistAction = {};
                        m_blacklistAction.type = BlacklistAction::Type::Remove;
                        std::snprintf(m_blacklistAction.key.data(),
                            m_blacklistAction.key.size(), "%s", entry.key.data());
                        m_blacklistActionDirty = true;
                    }
                    ImGui::Separator();
                    ImGui::PopID();
                }
                ImGui::EndChild();
            } else if (page == 12) {
                sectionTitle("ENABLED MODULE LIST");
                ImGui::TextWrapped(
                    "Shows enabled modules as animated text without a background. Drag the list while the Click GUI is open.");
                std::array<float, 3U> textColor = unpackRgb(m_features.textGuiColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Flow base color", textColor.data(),
                        ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.textGuiColor = packRgb(textColor);
                    changed = true;
                }
                if (ImGui::Button("Reset text position",
                                  ImVec2(180.0F * uiScale, 0.0F))) {
                    m_features.textGuiX = -1;
                    m_features.textGuiY = -1;
                    changed = true;
                }
                sectionTitle("LAYOUT");
                changed |= animatedToggle("Left accent line",
                    m_features.textGuiVerticalLine, m_toggleAnimation[38], uiScale);
                ImGui::TextDisabled("Text alignment");
                constexpr std::array<const char*, 3U> alignLabels{{
                    "Left", "Center", "Right"}};
                for (int alignment = 0; alignment < 3; ++alignment) {
                    if (alignment != 0) ImGui::SameLine();
                    const bool selected = m_features.textGuiAlignment == alignment;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                    if (ImGui::Button(alignLabels[static_cast<std::size_t>(alignment)],
                        ImVec2(88.0F * uiScale, 0.0F)) && !selected) {
                        m_features.textGuiAlignment = alignment;
                        changed = true;
                    }
                    if (selected) ImGui::PopStyleColor();
                }
            } else if (page == 14) {
                sectionTitle("PROJECTILE BOX");
                ImGui::TextWrapped(
                    "Tracks EntityFireball instances in an integrated single-player world. The compact box is intentionally smaller than the large-fireball collision volume.");
                changed |= animatedToggle("Semi-transparent fill",
                    m_features.fireballEspFilled, m_toggleAnimation[36], uiScale);
                std::array<float, 3U> fireballColor = unpackRgb(
                    m_features.fireballEspColor);
                ImGui::SetNextItemWidth(220.0F * uiScale);
                if (ImGui::ColorEdit3("Fireball color", fireballColor.data(),
                        ImGuiColorEditFlags_NoInputs |
                        ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.fireballEspColor = packRgb(fireballColor);
                    changed = true;
                }
                ImGui::TextColored(ImVec4(1.0F, 0.58F, 0.30F, 1.0F),
                    "LOCAL WORLD ONLY: automatically disabled on every multiplayer server.");
            } else if (page == 15) {
                sectionTitle("JUMP IMPULSE");
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Horizontal speed",
                    &m_features.longJumpSpeedPercent, 25, 250, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextWrapped(
                    "Applies a forward jump impulse at the next grounded movement step, with a bounded cooldown.");
                ImGui::TextColored(ImVec4(1.0F, 0.58F, 0.30F, 1.0F),
                    "LOCAL WORLD ONLY: automatically disabled on every multiplayer server.");
            } else if (page == 16) {
                sectionTitle("TRAJECTORY SOURCES");
                changed |= animatedToggle("Knockback prediction",
                    m_features.knockbackPredictionEnabled,
                    m_toggleAnimation[39], uiScale);
                ImGui::TextDisabled(
                    "Requires health loss plus a newly airborne velocity impulse.");
                changed |= animatedToggle("Bow prediction",
                    m_features.bowPredictionEnabled,
                    m_toggleAnimation[40], uiScale);
                ImGui::TextDisabled(
                    "20 TPS physics samples are blended every rendered frame.");
            } else if (page == 17) {
                sectionTitle("HOSTILE MOB LAB");
                changed |= animatedToggle("Auto-attack hostile mobs",
                    m_features.localMobAuraEnabled,
                    m_toggleAnimation[41], uiScale);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Local reach",
                    &m_features.localMobReach, 3, 10, "%d blocks",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Attack interval",
                    &m_features.localAttackDelayMs, 100, 1500, "%d ms",
                    ImGuiSliderFlags_AlwaysClamp);
                sectionTitle("LOCAL INCOMING VELOCITY");
                changed |= animatedToggle("Scale local knockback",
                    m_features.localVelocityEnabled,
                    m_toggleAnimation[42], uiScale);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Velocity retained",
                    &m_features.localVelocityPercent, 0, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::TextColored(ImVec4(1.0F, 0.58F, 0.30F, 1.0F),
                    "INTEGRATED SINGLE-PLAYER ONLY: Agent hard-disables this page elsewhere.");
                ImGui::TextWrapped(
                    "The attack helper accepts only non-player hostile candidates; it never targets players.");
            } else if (page == 18) {
                sectionTitle("WINDOWS INPUT METHOD BRIDGE");
                ImGui::TextWrapped(
                    "Mirrors the active Windows input method, live composition text and the current candidate page into the OpenGL frame. This keeps candidates visible in exclusive fullscreen without synthesizing input.");
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "The original IME remains the text owner. The overlay only observes WM_IME messages and never commits or replaces characters.");
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.42F, 0.84F, 0.78F, 1.0F),
                    "Candidate card appears at the top-center while composing and briefly after an input-method switch.");
            } else {
                sectionTitle("INTERFACE SIZE");
                constexpr std::array<const char*, 4U> sizeLabels{"S", "M", "L", "XL"};
                for (int sizeIndex = 0; sizeIndex < 4; ++sizeIndex) {
                    if (sizeIndex != 0) ImGui::SameLine();
                    const bool selected = m_guiScaleIndex == sizeIndex;
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, guiAccent);
                    if (ImGui::Button(sizeLabels[static_cast<std::size_t>(sizeIndex)],
                        ImVec2(64.0F * uiScale, 0.0F)) && !selected) {
                        m_guiScaleIndex = sizeIndex;
                        m_guiScaleDirty = true;
                    }
                    if (selected) ImGui::PopStyleColor();
                }
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Window width",
                    &m_features.clickGuiWidthPercent, 80, 150, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Window height",
                    &m_features.clickGuiHeightPercent, 80, 150, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetNextItemWidth(320.0F * uiScale);
                changed |= ImGui::SliderInt("Surface opacity",
                    &m_features.clickGuiOpacity, 35, 100, "%d%%",
                    ImGuiSliderFlags_AlwaysClamp);
                sectionTitle("INPUT");
                hotkeyControl("Open Click GUI", 1, m_menuHotkey);
                ImGui::TextDisabled("ESC closes the GUI and restores Minecraft mouse capture.");
                sectionTitle("THEME");
                ImGui::TextWrapped("Use the sun/moon button at the bottom-left to switch between the light and dark interface.");
                std::array<float, 3U> accentColor = unpackRgb(
                    m_features.clickGuiAccentColor);
                ImGui::SetNextItemWidth(240.0F * uiScale);
                if (ImGui::ColorEdit3("Theme accent", accentColor.data(),
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                    m_features.clickGuiAccentColor = packRgb(accentColor);
                    changed = true;
                }
                sectionTitle("SERVER SAFETY");
                changed |= animatedToggle(
                    "Allow movement modules on Hypixel",
                    m_features.allowHypixelMovement,
                    m_toggleAnimation[34], uiScale);
                ImGui::TextColored(ImVec4(1.0F, 0.34F, 0.28F, 1.0F),
                    "DANGER: Fly, BHop and Scaffold can cause a server ban.");
                ImGui::TextWrapped(
                    "By default these three modules are force-disabled whenever the current server address is Hypixel. Enable this exception only if you explicitly accept that risk.");
            }
            settingsVertexEnd = settingsDraw != nullptr
                ? settingsDraw->VtxBuffer.Size : settingsVertexStart;
            ImGui::EndChild();
            ImGui::PopStyleVar();

            // The renderer applies the same guard as AgentRuntime so a click
            // cannot leave a high-risk module enabled for even one rendered
            // frame before the next runtime snapshot arrives.
            if (snapshot.hypixelServer && !m_features.allowHypixelMovement &&
                (m_features.scaffoldEnabled || m_features.flyEnabled ||
                 m_features.bhopEnabled)) {
                if (m_features.scaffoldEnabled && !featuresBefore.scaffoldEnabled)
                    enqueueMessage(
                        "WARNING: Scaffold can cause a server ban. Use only offline.",
                        false);
                if (m_features.flyEnabled && !featuresBefore.flyEnabled)
                    enqueueMessage(
                        "WARNING: Fly can cause a server ban. Use only offline.",
                        false);
                if (m_features.bhopEnabled && !featuresBefore.bhopEnabled)
                    enqueueMessage(
                        "WARNING: BHop can cause a server ban. Use only offline.",
                        false);
                m_features.scaffoldEnabled = false;
                m_features.flyEnabled = false;
                m_features.bhopEnabled = false;
                enqueueMessage(
                    "BLOCKED: movement module disabled on Hypixel. See Interface / Server Safety.",
                    false);
                changed = true;
            }
            m_featureSettingsDirty = m_featureSettingsDirty || changed;
            if (changed) enqueueFeatureToasts(featuresBefore, m_features);
            windowDraw->PopClipRect();

            // Treat all generated GUI content as one flat layer. Scaling its
            // vertices once around the window centre makes the rail, header and
            // controls gather/scatter radially instead of appearing to travel
            // from the top-left. This is O(number of GUI vertices), requires no
            // per-widget animation state and leaves the final layout untouched.
            const float contentScale = spotlightScale;
            const ImVec2 contentCenter(windowPosition.x + windowSize.x * 0.5F,
                                       windowPosition.y + windowSize.y * 0.5F);
            const auto transformContent = [&](ImDrawList* const drawList,
                                              int begin, int end,
                                              const bool transformClips) noexcept {
                if (drawList == nullptr || std::abs(contentScale - 1.0F) < 0.0001F)
                    return;
                begin = std::clamp(begin, 0, drawList->VtxBuffer.Size);
                end = std::clamp(end, begin, drawList->VtxBuffer.Size);
                for (int vertexIndex = begin; vertexIndex < end; ++vertexIndex) {
                    ImVec2& position = drawList->VtxBuffer[vertexIndex].pos;
                    position.x = contentCenter.x +
                        (position.x - contentCenter.x) * contentScale;
                    position.y = contentCenter.y +
                        (position.y - contentCenter.y) * contentScale;
                }
                if (transformClips) {
                    for (ImDrawCmd& command : drawList->CmdBuffer) {
                        command.ClipRect.x = contentCenter.x +
                            (command.ClipRect.x - contentCenter.x) * contentScale;
                        command.ClipRect.y = contentCenter.y +
                            (command.ClipRect.y - contentCenter.y) * contentScale;
                        command.ClipRect.z = contentCenter.x +
                            (command.ClipRect.z - contentCenter.x) * contentScale;
                        command.ClipRect.w = contentCenter.y +
                            (command.ClipRect.w - contentCenter.y) * contentScale;
                    }
                }
            };
            const int parentContentVertexEnd = windowDraw->VtxBuffer.Size;
            transformContent(windowDraw, parentContentVertexStart,
                             parentContentVertexEnd, true);
            if (settingsDraw != windowDraw)
                transformContent(settingsDraw, settingsVertexStart,
                                 settingsVertexEnd, true);
            transformContent(backgroundDraw, shadowVertexStart,
                             backgroundDraw->VtxBuffer.Size, false);
        }
        ImGui::End();
        ImGui::PopStyleColor(15);
        ImGui::PopStyleVar(4);
    }

    // Adding a recent player is intentionally a separate modal surface. It no
    // longer expands the Blacklist settings page and therefore cannot disturb
    // that page's scroll position or make the form feel visually mixed with
    // persistent settings.
    if (!interactive) m_blacklistAddOpen = false;
    advancePresentationSpring(m_blacklistAddProgress,
                              m_blacklistAddVelocity,
                              m_blacklistAddOpen ? 1.0F : 0.0F, delta);
    if (m_blacklistAddProgress > 0.005F) {
        const float modalProgress = std::clamp(m_blacklistAddProgress, 0.0F, 1.0F);
        const float modalEase = modalProgress * modalProgress *
                                (3.0F - 2.0F * modalProgress);
        const float modalScale = 1.12F - 0.12F * m_blacklistAddProgress;
        const ImVec2 modalSize(460.0F * uiScale, 392.0F * uiScale);
        const ImVec2 modalPosition(
            std::max(6.0F, (io.DisplaySize.x - modalSize.x) * 0.5F),
            std::max(6.0F, (io.DisplaySize.y - modalSize.y) * 0.5F));
        const ImVec2 modalCenter(modalPosition.x + modalSize.x * 0.5F,
                                 modalPosition.y + modalSize.y * 0.5F);
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("##BlacklistAddModalBlocker", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav)) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(0.0F, 0.0F), io.DisplaySize,
                IM_COL32(5, 4, 9, static_cast<int>(82.0F * modalEase)));
            ImGui::InvisibleButton("##blacklistModalOutside", io.DisplaySize);
            if (interactive && modalProgress > 0.985F &&
                ImGui::IsItemClicked(ImGuiMouseButton_Left))
                m_blacklistAddOpen = false;
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::SetNextWindowPos(modalPosition, ImGuiCond_Always);
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, modalEase);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(22.0F * uiScale, 20.0F * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18.0F * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, guiSurface);
        ImGui::PushStyleColor(ImGuiCol_Text, guiText);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, guiMuted);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, guiFrame);
        ImGui::PushStyleColor(ImGuiCol_Button, guiFrame);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(guiAccent.x, guiAccent.y, guiAccent.z, 0.76F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, guiAccent);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, guiAccent);
        ImGuiWindowFlags modalFlags = ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse;
        if (!interactive || modalProgress < 0.985F)
            modalFlags |= ImGuiWindowFlags_NoInputs;
        ImDrawList* modalDraw = nullptr;
        int modalVertexEnd = 0;
        if (ImGui::Begin("##BlacklistAddDialog", nullptr, modalFlags)) {
            modalDraw = ImGui::GetWindowDrawList();
            ImFont* const modalBold = m_boldFonts[static_cast<std::size_t>(
                std::clamp(m_guiScaleIndex, 0, 3))] != nullptr
                ? m_boldFonts[static_cast<std::size_t>(
                    std::clamp(m_guiScaleIndex, 0, 3))] : ImGui::GetFont();
            ImGui::PushFont(modalBold);
            ImGui::TextUnformatted("Add to blacklist");
            ImGui::PopFont();
            ImGui::SameLine(modalSize.x - 61.0F * uiScale);
            if (ImGui::Button("x", ImVec2(30.0F * uiScale,
                                           28.0F * uiScale)))
                m_blacklistAddOpen = false;
            ImGui::TextDisabled("Choose a recently observed player; UUID is preferred when available.");
            ImGui::Spacing();
            const char* preview = "Select a recent player";
            if (m_blacklistSelectedPlayer >= 0 &&
                static_cast<std::uint32_t>(m_blacklistSelectedPlayer) <
                    snapshot.playerCount) {
                preview = snapshot.players[static_cast<std::size_t>(
                    m_blacklistSelectedPlayer)].name.data();
            }
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::BeginCombo("##blacklistRecentModal", preview)) {
                for (std::uint32_t index = 0U; index < snapshot.playerCount; ++index) {
                    const PlayerIdentity& identity = snapshot.players[index];
                    const bool selected = static_cast<int>(index) ==
                                          m_blacklistSelectedPlayer;
                    char label[64]{};
                    std::snprintf(label, sizeof(label), "%s%s",
                        identity.name.data(), identity.uuid[0U] == '\0'
                            ? "  (nick / no UUID)" : "");
                    if (ImGui::Selectable(label, selected)) {
                        m_blacklistSelectedPlayer = static_cast<int>(index);
                        m_blacklistIdOnlyNick = identity.uuid[0U] == '\0';
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputTextWithHint("##blacklistReasonModal", "Reason",
                m_blacklistReasonInput.data(), m_blacklistReasonInput.size());
            if (m_blacklist.presetCount > 0U) {
                ImGui::TextDisabled("Reason presets");
                for (std::uint32_t index = 0U;
                     index < m_blacklist.presetCount; ++index) {
                    if (index != 0U) ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::SmallButton(m_blacklist.presets[index].data()))
                        std::snprintf(m_blacklistReasonInput.data(),
                            m_blacklistReasonInput.size(), "%s",
                            m_blacklist.presets[index].data());
                    ImGui::PopID();
                }
            }
            if (m_blacklist.allowIdOnlyNicks)
                ImGui::Checkbox("Store ID only when UUID is unavailable",
                                &m_blacklistIdOnlyNick);
            ImGui::Checkbox("Warn on encounter", &m_blacklistWarnOnEncounter);
            const bool selectionValid = m_blacklistSelectedPlayer >= 0 &&
                static_cast<std::uint32_t>(m_blacklistSelectedPlayer) <
                    snapshot.playerCount;
            ImGui::SetCursorPosY(modalSize.y - 58.0F * uiScale);
            if (ImGui::Button("Cancel", ImVec2(104.0F * uiScale, 34.0F * uiScale)))
                m_blacklistAddOpen = false;
            ImGui::SameLine();
            if (!selectionValid) ImGui::BeginDisabled();
            if (ImGui::Button("Add player",
                              ImVec2(138.0F * uiScale, 34.0F * uiScale)) &&
                selectionValid) {
                const PlayerIdentity& identity = snapshot.players[
                    static_cast<std::size_t>(m_blacklistSelectedPlayer)];
                m_blacklistAction = {};
                m_blacklistAction.type = BlacklistAction::Type::Add;
                std::snprintf(m_blacklistAction.name.data(),
                    m_blacklistAction.name.size(), "%s", identity.name.data());
                std::snprintf(m_blacklistAction.uuid.data(),
                    m_blacklistAction.uuid.size(), "%s", identity.uuid.data());
                std::snprintf(m_blacklistAction.reason.data(),
                    m_blacklistAction.reason.size(), "%s",
                    m_blacklistReasonInput[0U] == '\0'
                        ? "No reason supplied" : m_blacklistReasonInput.data());
                m_blacklistAction.idOnlyNick = m_blacklistIdOnlyNick;
                m_blacklistAction.warnOnEncounter = m_blacklistWarnOnEncounter;
                m_blacklistActionDirty = true;
                m_blacklistAddOpen = false;
                m_blacklistSelectedPlayer = -1;
                m_blacklistReasonInput.fill('\0');
            }
            if (!selectionValid) ImGui::EndDisabled();
            // SetCursorPosY deliberately extends to the modal footer. Submit a
            // final bounded item so Dear ImGui can validate the content extent.
            ImGui::Dummy(ImVec2(1.0F, 1.0F));
            modalVertexEnd = modalDraw->VtxBuffer.Size;
        }
        ImGui::End();
        ImGui::PopStyleColor(8);
        ImGui::PopStyleVar(4);
        if (modalDraw != nullptr && std::abs(modalScale - 1.0F) > 0.0001F) {
            modalVertexEnd = std::clamp(modalVertexEnd, 0,
                                        modalDraw->VtxBuffer.Size);
            for (int vertex = 0; vertex < modalVertexEnd; ++vertex) {
                ImVec2& position = modalDraw->VtxBuffer[vertex].pos;
                position.x = modalCenter.x +
                    (position.x - modalCenter.x) * modalScale;
                position.y = modalCenter.y +
                    (position.y - modalCenter.y) * modalScale;
            }
        }
    }

    // Text GUI is deliberately a text-only HUD: no window surface is drawn.
    // A single invisible hit target owns dragging while the Click GUI is open,
    // avoiding competing per-row hover/cursor state.
    if (m_features.textGuiEnabled) {
        struct TextModule { const char* name; bool enabled; };
        const std::array<TextModule, 18U> modules{{
            {"Player ESP", m_features.entityEspEnabled},
            {"Bed ESP", m_features.bedEspEnabled},
            {"Nametag", m_features.nametagEnabled},
            {"Bed Alert", m_features.bedThreatAlertsEnabled},
            {"Safewalk", m_features.safewalkEnabled},
            {"Scaffold", m_features.scaffoldEnabled},
            {"Fly", m_features.flyEnabled},
            {"BHop", m_features.bhopEnabled},
            {"Aim Assist", m_features.aimAssistEnabled},
            {"Player Stats", m_features.hypixelPanelEnabled},
            {"Debug", m_features.debugChatEnabled},
            {"Blacklist", m_blacklist.panelEnabled},
            {"Fireball ESP", m_features.fireballEspEnabled},
            {"LongJump", m_features.longJumpEnabled},
            {"Knockback Prediction", m_features.knockbackPredictionEnabled},
            {"Bow Prediction", m_features.bowPredictionEnabled},
            {"Local Mob Aura", m_features.localMobAuraEnabled},
            {"Local Velocity", m_features.localVelocityEnabled}}};
        ImFont* const textGuiFont = m_boldFonts[static_cast<std::size_t>(
            std::clamp(m_guiScaleIndex, 0, 3))] != nullptr
            ? m_boldFonts[static_cast<std::size_t>(
                std::clamp(m_guiScaleIndex, 0, 3))] : ImGui::GetFont();
        const float textGuiFontSize = ImGui::GetFontSize() * 1.20F;
        const auto measureText = [&](const char* const value) noexcept {
            return textGuiFont->CalcTextSizeA(textGuiFontSize, 100000.0F,
                                              0.0F, value);
        };
        const std::uint64_t glyphClock = static_cast<std::uint64_t>(
            ImGui::GetTime() * 1000.0);
        if (!m_textGuiGlyphsInitialized ||
            glyphClock >= m_textGuiNextShuffleTick) {
            for (std::size_t moduleIndex = 0U;
                 moduleIndex < modules.size(); ++moduleIndex) {
                const std::size_t length = std::min<std::size_t>(
                    std::strlen(modules[moduleIndex].name), 32U);
                std::array<std::uint8_t, 32U> order{};
                for (std::size_t character = 0U; character < length; ++character) {
                    order[character] = static_cast<std::uint8_t>(character);
                    m_textGuiGlyphTargets[moduleIndex][character] = 0.50F;
                }
                std::uint32_t randomState = static_cast<std::uint32_t>(
                    glyphClock ^ (moduleIndex + 1U) * 0x9E3779B9U);
                for (std::size_t remaining = length; remaining > 1U; --remaining) {
                    randomState = randomState * 1664525U + 1013904223U;
                    const std::size_t swapIndex = randomState % remaining;
                    std::swap(order[remaining - 1U], order[swapIndex]);
                }
                for (std::size_t bright = 0U; bright < (length + 1U) / 2U;
                     ++bright) {
                    m_textGuiGlyphTargets[moduleIndex][order[bright]] = 1.0F;
                }
            }
            if (!m_textGuiGlyphsInitialized) {
                m_textGuiGlyphBrightness = m_textGuiGlyphTargets;
                m_textGuiGlyphsInitialized = true;
            }
            // Recompose the half-bright mask at a calm cadence. Brightness is
            // interpolated below, so individual glyphs never flash abruptly.
            m_textGuiNextShuffleTick = glyphClock + 720U;
        }
        // Keep the 720 ms target shuffle cadence, but let each glyph breathe
        // slowly toward its next luminance instead of flashing between masks.
        const float glyphBlend = 1.0F - std::exp(-2.0F * delta);
        for (std::size_t moduleIndex = 0U; moduleIndex < modules.size(); ++moduleIndex)
            for (std::size_t character = 0U; character < 32U; ++character)
                m_textGuiGlyphBrightness[moduleIndex][character] +=
                    (m_textGuiGlyphTargets[moduleIndex][character] -
                     m_textGuiGlyphBrightness[moduleIndex][character]) * glyphBlend;

        float visibleRows = 0.0F;
        float maximumTextWidth = 0.0F;
        for (std::size_t index = 0U; index < modules.size(); ++index) {
            advancePresentationSpring(m_textGuiModuleProgress[index],
                                      m_textGuiModuleVelocity[index],
                                      modules[index].enabled ? 1.0F : 0.0F, delta);
            const float progress = std::clamp(
                m_textGuiModuleProgress[index], 0.0F, 1.0F);
            if (progress <= 0.004F) continue;
            visibleRows += progress;
            maximumTextWidth = std::max(maximumTextWidth,
                                        measureText(modules[index].name).x);
        }
        if (visibleRows > 0.004F) {
            const float width = maximumTextWidth +
                (m_features.textGuiVerticalLine ? 28.0F : 20.0F) * uiScale;
            const float lineHeight = textGuiFontSize + 5.0F * uiScale;
            const float height = lineHeight * visibleRows +
                                 8.0F * uiScale;
            const float defaultX = std::max(5.0F, io.DisplaySize.x - width - 16.0F);
            const float defaultY = std::max(5.0F, io.DisplaySize.y * 0.18F);
            float textX = m_features.textGuiX < 0 ? defaultX
                : io.DisplaySize.x * static_cast<float>(m_features.textGuiX) / 1000.0F;
            float textY = m_features.textGuiY < 0 ? defaultY
                : io.DisplaySize.y * static_cast<float>(m_features.textGuiY) / 1000.0F;
            textX = std::clamp(textX, 4.0F,
                std::max(4.0F, io.DisplaySize.x - width - 4.0F));
            textY = std::clamp(textY, 4.0F,
                std::max(4.0F, io.DisplaySize.y - height - 4.0F));
            ImGui::SetNextWindowPos(ImVec2(textX, textY), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0F);
            ImGuiWindowFlags textFlags = ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                ImGuiWindowFlags_NoScrollbar;
            if (!interactive) textFlags |= ImGuiWindowFlags_NoInputs;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            if (ImGui::Begin("##TextGuiHud", nullptr, textFlags)) {
                ImGui::InvisibleButton("##TextGuiDrag", ImVec2(width, height));
                if (interactive && ImGui::IsItemActive() &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F)) {
                    textX = std::clamp(textX + io.MouseDelta.x, 4.0F,
                        std::max(4.0F, io.DisplaySize.x - width - 4.0F));
                    textY = std::clamp(textY + io.MouseDelta.y, 4.0F,
                        std::max(4.0F, io.DisplaySize.y - height - 4.0F));
                    m_features.textGuiX = std::clamp(static_cast<int>(std::lround(
                        textX / std::max(1.0F, io.DisplaySize.x) * 1000.0F)), 0, 1000);
                    m_features.textGuiY = std::clamp(static_cast<int>(std::lround(
                        textY / std::max(1.0F, io.DisplaySize.y) * 1000.0F)), 0, 1000);
                    m_featureSettingsDirty = true;
                }
                ImDrawList* const textDraw = ImGui::GetWindowDrawList();
                const std::array<float, 3U> baseRgb =
                    unpackRgb(m_features.textGuiColor);
                const ImVec4 base(baseRgb[0U], baseRgb[1U], baseRgb[2U], 1.0F);
                float rowY = textY + 4.0F * uiScale;
                if (m_features.textGuiVerticalLine) {
                    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(
                        ImVec4(base.x, base.y, base.z, 0.82F));
                    textDraw->AddRectFilled(
                        ImVec2(textX + 3.0F * uiScale, textY + 3.0F * uiScale),
                        ImVec2(textX + 6.0F * uiScale,
                               textY + height - 3.0F * uiScale),
                        lineColor, 1.5F * uiScale);
                }
                for (std::size_t moduleIndex = 0U;
                     moduleIndex < modules.size(); ++moduleIndex) {
                    const TextModule& module = modules[moduleIndex];
                    const float progress = std::clamp(
                        m_textGuiModuleProgress[moduleIndex], 0.0F, 1.0F);
                    if (progress <= 0.004F) continue;
                    const float eased = progress * progress * (3.0F - 2.0F * progress);
                    const ImVec2 textSize = measureText(module.name);
                    const float contentLeft = textX +
                        (m_features.textGuiVerticalLine ? 11.0F : 4.0F) * uiScale;
                    const float contentRight = textX + width - 4.0F * uiScale;
                    float glyphX = contentLeft;
                    if (m_features.textGuiAlignment == 1)
                        glyphX = (contentLeft + contentRight - textSize.x) * 0.5F;
                    else if (m_features.textGuiAlignment == 2)
                        glyphX = contentRight - textSize.x;
                    const float entryDirection = m_features.textGuiAlignment == 0
                        ? -1.0F : 1.0F;
                    glyphX += entryDirection * (1.0F - eased) * 18.0F * uiScale;
                    const float glyphY = rowY + (1.0F - eased) * 4.0F * uiScale;
                    // Half of the glyphs are bright and half are 50% dimmed.
                    // A continuously regenerated mask flows through a smooth
                    // exponential transition, producing a restrained optical
                    // shimmer rather than a scrolling-lyrics effect.
                    for (std::size_t characterIndex = 0U;
                         module.name[characterIndex] != '\0'; ++characterIndex) {
                        char glyph[2]{module.name[characterIndex], '\0'};
                        const float glyphWidth = measureText(glyph).x;
                        const float brightness = m_textGuiGlyphBrightness[
                            moduleIndex][std::min<std::size_t>(characterIndex, 31U)];
                        textDraw->AddText(textGuiFont, textGuiFontSize,
                            ImVec2(glyphX + 1.0F, glyphY + 1.4F),
                            IM_COL32(0, 0, 0,
                                static_cast<int>(145.0F * eased * brightness)), glyph);
                        const ImVec4 glyphColor(
                            std::clamp(base.x * 0.58F + 0.42F, 0.0F, 1.0F),
                            std::clamp(base.y * 0.58F + 0.42F, 0.0F, 1.0F),
                            std::clamp(base.z * 0.58F + 0.42F, 0.0F, 1.0F),
                            eased * brightness);
                        textDraw->AddText(textGuiFont, textGuiFontSize,
                            ImVec2(glyphX, glyphY),
                            ImGui::ColorConvertFloat4ToU32(glyphColor), glyph);
                        glyphX += glyphWidth;
                    }
                    rowY += lineHeight * progress;
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
    }

    const bool statsHotkeyDown =
        (::GetAsyncKeyState(m_features.hypixelPanelHotkey) & 0x8000) != 0;
    const bool statsPanelTarget = m_features.hypixelPanelEnabled &&
        snapshot.matchActive && snapshot.playerCount > 0U &&
        (!m_features.hypixelPanelHoldToShow || statsHotkeyDown || interactive);
    advancePresentationSpring(m_statsPanelProgress, m_statsPanelVelocity,
                              statsPanelTarget ? 1.0F : 0.0F, delta);

    if (m_statsPanelProgress > 0.005F) {
            const float panelLinear = std::clamp(m_statsPanelProgress, 0.0F, 1.0F);
            const float panelEase = panelLinear * panelLinear *
                                    (3.0F - 2.0F * panelLinear);
            const float panelPresentationScale =
                1.26F - 0.26F * m_statsPanelProgress;
            const float panelWidthScale = static_cast<float>(std::clamp(
                m_features.hypixelPanelScale, 70, 160)) / 100.0F;
            const float panelHeightScale = static_cast<float>(std::clamp(
                m_features.hypixelPanelHeight, 60, 400)) / 100.0F;
            const float panelWidth = std::clamp(
                448.0F * panelWidthScale, 420.0F,
                std::max(420.0F, io.DisplaySize.x * 0.92F));
            const float panelHeight = std::clamp(
                230.0F * panelHeightScale, 138.0F,
                std::max(138.0F, io.DisplaySize.y - 8.0F));
            const float defaultPanelX = std::max(6.0F,
                io.DisplaySize.x - panelWidth - 12.0F);
            const float storedPanelX = m_features.hypixelPanelX < 0
                ? defaultPanelX
                : io.DisplaySize.x * static_cast<float>(m_features.hypixelPanelX) / 1000.0F;
            const float storedPanelY = m_features.hypixelPanelY < 0
                ? 12.0F
                : io.DisplaySize.y * static_cast<float>(m_features.hypixelPanelY) / 1000.0F;
            const float targetPanelX = std::clamp(storedPanelX, 4.0F,
                std::max(4.0F, io.DisplaySize.x - panelWidth - 4.0F));
            const float targetPanelY = std::clamp(storedPanelY, 4.0F,
                std::max(4.0F, io.DisplaySize.y - panelHeight - 4.0F));
            ImGui::SetNextWindowPos(ImVec2(targetPanelX, targetPanelY),
                                    ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(panelWidth, panelHeight), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0F);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panelEase);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                ImVec2(0.0F, 0.0F));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
            ImGuiWindowFlags boardFlags = ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;
            if (!interactive) boardFlags |= ImGuiWindowFlags_NoInputs;
            ImDrawList* statsCardDraw = nullptr;
            ImDrawList* statsRowsDraw = nullptr;
            int statsCardVertexStart = 0;
            int statsCardVertexEnd = 0;
            int statsRowsVertexStart = 0;
            int statsRowsVertexEnd = 0;
            if (ImGui::Begin("##LiveBedWarsPlayers", nullptr, boardFlags)) {
                ImDrawList* const cardDraw = ImGui::GetWindowDrawList();
                statsCardDraw = cardDraw;
                statsCardVertexStart = cardDraw->VtxBuffer.Size;
                const ImVec2 cardMin = ImGui::GetWindowPos();
                const ImVec2 cardMax(cardMin.x + ImGui::GetWindowWidth(),
                                     cardMin.y + ImGui::GetWindowHeight());
                cardDraw->AddRectFilled(
                    ImVec2(cardMin.x + 3.0F, cardMin.y + 5.0F),
                    ImVec2(cardMax.x + 3.0F, cardMax.y + 5.0F),
                    IM_COL32(0, 0, 0, 72), 12.0F);
                const int statsPanelAlpha = static_cast<int>(std::lround(
                    std::clamp(m_features.hypixelPanelOpacity, 0, 100) * 2.55));
                const bool whitePanel = m_features.hypixelPanelColor == 0xFFFFFFU;
                const ImU32 primaryText = whitePanel
                    ? IM_COL32(14, 14, 18, 255) : IM_COL32(248, 248, 250, 255);
                const ImU32 secondaryText = whitePanel
                    ? IM_COL32(55, 55, 62, 255) : IM_COL32(210, 210, 218, 255);
                if (statsPanelAlpha < 250) {
                    captureBackdropTexture();
                    if (m_blurTexture != 0U) {
                        // 13-tap separable-Gaussian approximation. The former
                        // five equal acrylic samples preserved hard edges;
                        // weighted centre/axis/diagonal taps produce a softer
                        // Gaussian backdrop without allocating another FBO.
                        struct BlurTap final { ImVec2 offset; int alpha; };
                        constexpr std::array<BlurTap, 13U> blurTaps{{
                            {ImVec2(0, 0), 54},
                            {ImVec2(-2, 0), 40}, {ImVec2(2, 0), 40},
                            {ImVec2(0, -2), 40}, {ImVec2(0, 2), 40},
                            {ImVec2(-2, -2), 26}, {ImVec2(2, -2), 26},
                            {ImVec2(-2, 2), 26}, {ImVec2(2, 2), 26},
                            {ImVec2(-5, 0), 17}, {ImVec2(5, 0), 17},
                            {ImVec2(0, -5), 17}, {ImVec2(0, 5), 17}}};
                        const float exitBlurSpread = 1.0F +
                            (1.0F - panelLinear) * 2.8F;
                        for (const BlurTap& tap : blurTaps) {
                            const float left = std::clamp(cardMin.x +
                                                          tap.offset.x * exitBlurSpread,
                                                          0.0F, io.DisplaySize.x);
                            const float top = std::clamp(cardMin.y +
                                                         tap.offset.y * exitBlurSpread,
                                                         0.0F, io.DisplaySize.y);
                            const float right = std::clamp(cardMax.x +
                                                           tap.offset.x * exitBlurSpread,
                                                           0.0F, io.DisplaySize.x);
                            const float bottom = std::clamp(cardMax.y +
                                                            tap.offset.y * exitBlurSpread,
                                                            0.0F, io.DisplaySize.y);
                            cardDraw->AddImageRounded(
                                reinterpret_cast<ImTextureID>(
                                    static_cast<std::uintptr_t>(m_blurTexture)),
                                cardMin, cardMax,
                                ImVec2(left / io.DisplaySize.x,
                                       1.0F - top / io.DisplaySize.y),
                                ImVec2(right / io.DisplaySize.x,
                                       1.0F - bottom / io.DisplaySize.y),
                                IM_COL32(255, 255, 255, static_cast<int>(
                                    std::lround(static_cast<float>(tap.alpha) * panelEase))),
                                12.0F);
                        }
                    }
                }
                cardDraw->AddRectFilled(cardMin, cardMax,
                    packedRgbColor(whitePanel ? 0xFFFFFFU : 0x000000U, statsPanelAlpha),
                    12.0F);
                cardDraw->AddRect(cardMin, cardMax,
                    whitePanel ? IM_COL32(0, 0, 0, 52) : IM_COL32(255, 255, 255, 48),
                    12.0F, 0, 1.0F);
                const int statsRailAlpha = static_cast<int>(std::lround(
                    std::clamp(m_features.hypixelRailOpacity, 0, 100) * 2.55));
                cardDraw->AddRectFilled(
                    cardMin, ImVec2(cardMin.x + 24.0F, cardMax.y),
                    packedRgbColor(m_features.hypixelRailColor, statsRailAlpha),
                    12.0F,
                    ImDrawFlags_RoundCornersLeft);
                const std::size_t statsFontIndex = static_cast<std::size_t>(
                    std::clamp(m_features.hypixelPanelFontIndex, 0, 3));
                ImFont* const panelFont = m_fonts[statsFontIndex] != nullptr
                    ? m_fonts[statsFontIndex] : ImGui::GetFont();
                ImFont* const panelBold = m_boldFonts[statsFontIndex] != nullptr
                    ? m_boldFonts[statsFontIndex] : panelFont;
                // Independent geometry: resizing the card never stretches the
                // rasterized font or row pitch. This keeps every row legible at
                // all GUI size presets and prevents baseline overlap.
                const float panelFontSize = panelFont->LegacySize;
                const float panelBoldSize = panelBold->LegacySize;
                const float rowHeight = std::ceil(std::max(20.0F,
                                                          panelFontSize + 5.0F));
                constexpr float railWidth = 24.0F;
                const float headerHeight = std::ceil(panelBoldSize + 11.0F);
                if (interactive && panelLinear > 0.985F) {
                    const ImVec2 resizeMin(cardMax.x - 22.0F,
                                           cardMax.y - 22.0F);
                    const bool resizeHovered = ImGui::IsMouseHoveringRect(
                        resizeMin, cardMax, false);
                    const bool headerHovered = ImGui::IsMouseHoveringRect(
                        cardMin,
                        ImVec2(cardMax.x - 22.0F,
                               cardMin.y + headerHeight), false);
                    const bool railHovered = ImGui::IsMouseHoveringRect(
                        cardMin,
                        ImVec2(cardMin.x + railWidth, cardMax.y), false);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (resizeHovered) {
                            m_statsPanelResizing = true;
                            m_statsPanelDragging = false;
                            m_statsPanelResizeStartX = io.MousePos.x;
                            m_statsPanelResizeStartY = io.MousePos.y;
                            m_statsPanelResizeStartScale = m_features.hypixelPanelScale;
                            m_statsPanelResizeStartHeight = m_features.hypixelPanelHeight;
                        } else if (headerHovered || railHovered) {
                            m_statsPanelDragging = true;
                            m_statsPanelResizing = false;
                            m_statsPanelDragStartMouseX = io.MousePos.x;
                            m_statsPanelDragStartMouseY = io.MousePos.y;
                            m_statsPanelDragStartPanelX = targetPanelX;
                            m_statsPanelDragStartPanelY = targetPanelY;
                        }
                    }
                    if (m_statsPanelResizing && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        const float dragX = io.MousePos.x - m_statsPanelResizeStartX;
                        const float dragY = io.MousePos.y - m_statsPanelResizeStartY;
                        const int resizedWidth = std::clamp(
                            m_statsPanelResizeStartScale + static_cast<int>(std::lround(
                                dragX * 100.0F / 448.0F)), 70, 160);
                        const int resizedHeight = std::clamp(
                            m_statsPanelResizeStartHeight + static_cast<int>(std::lround(
                                dragY * 100.0F / 230.0F)), 60, 400);
                        if (resizedWidth != m_features.hypixelPanelScale ||
                            resizedHeight != m_features.hypixelPanelHeight) {
                            m_features.hypixelPanelScale = resizedWidth;
                            m_features.hypixelPanelHeight = resizedHeight;
                            m_statsPanelTransformDirty = true;
                        }
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                    } else if (m_statsPanelDragging &&
                               ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        // Calculate from the immutable press origin. Accumulating
                        // MouseDelta into a normalized/rounded position caused
                        // quantization feedback, lag and cursor-shape flicker.
                        const float movedX = m_statsPanelDragStartPanelX +
                            (io.MousePos.x - m_statsPanelDragStartMouseX);
                        const float movedY = m_statsPanelDragStartPanelY +
                            (io.MousePos.y - m_statsPanelDragStartMouseY);
                        m_features.hypixelPanelX = std::clamp(static_cast<int>(std::lround(
                            movedX / std::max(1.0F, io.DisplaySize.x) * 1000.0F)), 0, 1000);
                        m_features.hypixelPanelY = std::clamp(static_cast<int>(std::lround(
                            movedY / std::max(1.0F, io.DisplaySize.y) * 1000.0F)), 0, 1000);
                        m_statsPanelTransformDirty = true;
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    } else if (resizeHovered) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                    } else if (headerHovered || railHovered) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    }
                    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                        (m_statsPanelDragging || m_statsPanelResizing)) {
                        if (m_statsPanelTransformDirty) m_featureSettingsDirty = true;
                        m_statsPanelTransformDirty = false;
                        m_statsPanelDragging = false;
                        m_statsPanelResizing = false;
                    }
                }
                constexpr std::array<char, 5U> verticalLabel{'S', 'T', 'A', 'T', 'S'};
                const float labelAdvance = std::max(13.0F, panelBoldSize * 0.84F);
                const float labelHeight = panelBoldSize +
                    labelAdvance * static_cast<float>(verticalLabel.size() - 1U);
                const float labelStartY = cardMin.y +
                    std::max(0.0F, (panelHeight - labelHeight) * 0.5F);
                for (std::size_t letter = 0U; letter < verticalLabel.size(); ++letter) {
                    const char text[2]{verticalLabel[letter], '\0'};
                    const ImVec2 textSize = panelBold->CalcTextSizeA(
                        panelBoldSize, FLT_MAX, 0.0F, text);
                    cardDraw->AddText(panelBold, panelBoldSize,
                        ImVec2(cardMin.x + (railWidth - textSize.x) * 0.5F,
                               labelStartY + static_cast<float>(letter) * labelAdvance),
                        contrastingTextColor(m_features.hypixelRailColor), text);
                }
                const ImVec2 columnHeader(cardMin.x + railWidth + 4.0F,
                                          cardMin.y + 6.0F);
                const float contentWidth = std::max(360.0F, panelWidth - railWidth - 8.0F);
                const std::array<float, 8U> columns{{
                    4.0F, contentWidth * 0.335F, contentWidth * 0.445F,
                    contentWidth * 0.555F, contentWidth * 0.655F,
                    contentWidth * 0.755F, contentWidth * 0.865F,
                    contentWidth * 0.955F}};
                const ImU32 headerColor = primaryText;
                const auto header = [&](const float x, const char* text) noexcept {
                    cardDraw->AddText(panelBold, panelBoldSize,
                        ImVec2(columnHeader.x + x, columnHeader.y),
                        headerColor, text);
                };
                constexpr std::array<const char*, 8U> headers{{
                    "PLAYER", "STAR", "FKDR", "WLR", "BBLR", "FINALS", "WINS", "WS"}};
                for (std::size_t column = 0U; column < headers.size(); ++column)
                    header(columns[column], headers[column]);
                ImGui::SetCursorScreenPos(ImVec2(columnHeader.x,
                                                  cardMin.y + headerHeight));
                std::array<std::uint32_t, GameSnapshot::MaxDiscoveredPlayers> order{};
                for (std::uint32_t index = 0U; index < snapshot.playerCount; ++index) {
                    order[index] = index;
                }
                const auto teamRank = [](const char code) noexcept {
                    constexpr std::array<char, 8U> orderCodes{
                        'c', '9', 'a', 'e', 'b', 'f', 'd', '7'};
                    const auto found = std::find(orderCodes.begin(), orderCodes.end(), code);
                    return found == orderCodes.end()
                        ? 8 : static_cast<int>(found - orderCodes.begin());
                };
                const auto teamLetter = [](const char code) noexcept {
                    switch (code) {
                    case 'c': return 'R';
                    case '9': return 'B';
                    case 'a': return 'G';
                    case 'e': return 'Y';
                    case 'b': return 'A';
                    case 'f': return 'W';
                    case 'd': return 'P';
                    case '7': return 'S';
                    default: return '?';
                    }
                };
                std::sort(order.begin(), order.begin() + snapshot.playerCount,
                          [&](const std::uint32_t first, const std::uint32_t second) noexcept {
                              const PlayerIdentity& a = snapshot.players[first];
                              const PlayerIdentity& b = snapshot.players[second];
                              const int ar = teamRank(a.teamColor);
                              const int br = teamRank(b.teamColor);
                              return ar != br ? ar < br
                                  : std::strcmp(a.name.data(), b.name.data()) < 0;
                          });
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, 0.0F));
                ImGui::BeginChild("##liveRosterRows",
                    ImVec2(panelWidth - railWidth - 8.0F,
                           panelHeight - headerHeight - 4.0F),
                    false, ImGuiWindowFlags_NoBackground);
                statsRowsDraw = ImGui::GetWindowDrawList();
                statsRowsVertexStart = statsRowsDraw != nullptr
                    ? statsRowsDraw->VtxBuffer.Size : 0;
                for (std::uint32_t ordered = 0U; ordered < snapshot.playerCount; ++ordered) {
                    const PlayerIdentity& identity = snapshot.players[order[ordered]];
                    const char code = identity.teamColor;
                    const PlayerStatsEntry* stats = nullptr;
                    for (std::uint32_t statsIndex = 0U;
                         statsIndex < m_playerStats.count; ++statsIndex) {
                        if (std::strcmp(m_playerStats.entries[statsIndex].name.data(),
                                        identity.name.data()) == 0) {
                            stats = &m_playerStats.entries[statsIndex];
                            break;
                        }
                    }
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    const ImVec2 size(ImGui::GetContentRegionAvail().x,
                                      rowHeight);
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    const int rowAlpha = (ordered & 1U) != 0U ? 18 : 10;
                    drawList->AddRectFilled(pos,
                        ImVec2(pos.x + size.x, pos.y + size.y),
                        whitePanel ? IM_COL32(0, 0, 0, rowAlpha)
                                   : IM_COL32(255, 255, 255, rowAlpha),
                        0.0F);
                    const float textY = pos.y + (rowHeight - panelFontSize) * 0.5F;
                    const char teamTag[4]{'[', teamLetter(code), ']', '\0'};
                    drawList->AddText(panelBold, panelBoldSize,
                        ImVec2(pos.x + columns[0U], textY),
                        ImGui::ColorConvertFloat4ToU32(teamColor(code)), teamTag);
                    const ImVec4 nameClip(
                        pos.x + 30.0F, pos.y,
                        pos.x + columns[1U] - 5.0F, pos.y + size.y);
                    drawList->AddText(panelFont, panelFontSize,
                        ImVec2(pos.x + 30.0F, textY), primaryText,
                        identity.name.data(), nullptr, 0.0F, &nameClip);
                    if (stats == nullptr) {
                        drawList->AddText(panelFont, panelFontSize,
                            ImVec2(pos.x + columns[1U], textY),
                            secondaryText, "Querying...");
                    } else if (stats->failed) {
                        const bool suspectedNick = std::strcmp(
                            stats->status.data(), "unavailable") == 0;
                        drawList->AddText(panelBold, panelBoldSize,
                            ImVec2(pos.x + columns[1U], textY),
                            suspectedNick ? IM_COL32(224, 159, 255, 255)
                                          : IM_COL32(255, 116, 127, 255),
                            suspectedNick ? "SUSPECTED NICK" : "UNAVAILABLE");
                    } else {
                        char starsText[24]{};
                        char fkdrText[16]{};
                        char wlrText[16]{};
                        char bblrText[16]{};
                        char finalsText[16]{};
                        char winsText[16]{};
                        char winStreakText[12]{};
                        const PrestigeStyle prestige = bedWarsPrestigeStyle(stats->stars);
                        std::snprintf(starsText, sizeof(starsText), "%d", stats->stars);
                        std::snprintf(fkdrText, sizeof(fkdrText), "%.2f", stats->fkdr);
                        std::snprintf(wlrText, sizeof(wlrText), "%.2f", stats->wlr);
                        std::snprintf(bblrText, sizeof(bblrText), "%.2f", stats->bblr);
                        formatCompactCount(finalsText, sizeof(finalsText), stats->finalKills);
                        formatCompactCount(winsText, sizeof(winsText), stats->wins);
                        std::snprintf(winStreakText, sizeof(winStreakText), "%d", stats->winStreak);
                        const ImU32 fkdrColor = stats->fkdr >= 3.0
                            ? IM_COL32(89, 235, 122, 255)
                            : (stats->fkdr >= 1.5 ? IM_COL32(255, 214, 82, 255)
                                                  : IM_COL32(255, 97, 107, 255));
                        drawList->AddText(panelBold, panelBoldSize,
                            ImVec2(pos.x + columns[1U], textY),
                            prestige.color, starsText);
                        const ImVec2 starNumberSize = panelBold->CalcTextSizeA(
                            panelBoldSize, FLT_MAX, 0.0F, starsText);
                        drawPrestigeStar(drawList,
                            ImVec2(pos.x + columns[1U] + starNumberSize.x + 6.0F,
                                   pos.y + size.y * 0.5F),
                            4.1F, prestige.color, prestige.master);
                        drawList->AddText(panelBold, panelBoldSize,
                            ImVec2(pos.x + columns[2U], textY), fkdrColor, fkdrText);
                        drawList->AddText(panelFont, panelFontSize,
                            ImVec2(pos.x + columns[3U], textY),
                            stats->wlr >= 1.0 ? IM_COL32(57, 190, 112, 255) : secondaryText,
                            wlrText);
                        drawList->AddText(panelFont, panelFontSize,
                            ImVec2(pos.x + columns[4U], textY),
                            stats->bblr >= 1.5 ? IM_COL32(54, 164, 219, 255) : secondaryText,
                            bblrText);
                        drawList->AddText(panelFont, panelFontSize,
                            ImVec2(pos.x + columns[5U], textY),
                            stats->finalKills >= 5000 ? IM_COL32(224, 151, 35, 255) : secondaryText,
                            finalsText);
                        drawList->AddText(panelFont, panelFontSize,
                            ImVec2(pos.x + columns[6U], textY), secondaryText, winsText);
                        drawList->AddText(panelFont, panelFontSize,
                            ImVec2(pos.x + columns[7U], textY),
                            stats->winStreak >= 10 ? IM_COL32(238, 83, 70, 255) : secondaryText,
                            winStreakText);
                    }
                    ImGui::Dummy(size);
                }
                statsRowsVertexEnd = statsRowsDraw != nullptr
                    ? statsRowsDraw->VtxBuffer.Size : statsRowsVertexStart;
                ImGui::EndChild();
                ImGui::PopStyleVar();
                if (interactive) {
                    const ImU32 gripColor = whitePanel
                        ? IM_COL32(20, 20, 24, 155) : IM_COL32(255, 255, 255, 160);
                    for (int gripLine = 0; gripLine < 3; ++gripLine) {
                        const float inset = 4.0F + static_cast<float>(gripLine) * 4.0F;
                        cardDraw->AddLine(
                            ImVec2(cardMax.x - inset, cardMax.y - 2.0F),
                            ImVec2(cardMax.x - 2.0F, cardMax.y - inset),
                            gripColor, 1.0F);
                    }
                }
                statsCardVertexEnd = cardDraw->VtxBuffer.Size;
            }
            ImGui::End();
            ImGui::PopStyleVar(3);

            // Use the same whole-surface spotlight transform as the Click GUI.
            // Geometry is laid out at its final coordinates and all vertices
            // gather/scatter around the panel centre as one flat layer.
            const ImVec2 statsCenter(targetPanelX + panelWidth * 0.5F,
                                     targetPanelY + panelHeight * 0.5F);
            const auto transformStats = [&](ImDrawList* const drawList,
                                            int begin, int end) noexcept {
                if (drawList == nullptr) return;
                begin = std::clamp(begin, 0, drawList->VtxBuffer.Size);
                end = std::clamp(end, begin, drawList->VtxBuffer.Size);
                for (int vertexIndex = begin; vertexIndex < end; ++vertexIndex) {
                    ImDrawVert& vertex = drawList->VtxBuffer[vertexIndex];
                    if (std::abs(panelPresentationScale - 1.0F) >= 0.0001F) {
                        vertex.pos.x = statsCenter.x +
                            (vertex.pos.x - statsCenter.x) * panelPresentationScale;
                        vertex.pos.y = statsCenter.y +
                            (vertex.pos.y - statsCenter.y) * panelPresentationScale;
                    }
                    const unsigned alpha = static_cast<unsigned>(vertex.col >> 24U);
                    const unsigned faded = static_cast<unsigned>(std::clamp(
                        std::lround(static_cast<float>(alpha) * panelEase),
                        0L, 255L));
                    vertex.col = (vertex.col & 0x00FFFFFFU) | (faded << 24U);
                }
                if (std::abs(panelPresentationScale - 1.0F) >= 0.0001F) {
                    for (ImDrawCmd& command : drawList->CmdBuffer) {
                        command.ClipRect.x = statsCenter.x +
                            (command.ClipRect.x - statsCenter.x) * panelPresentationScale;
                        command.ClipRect.y = statsCenter.y +
                            (command.ClipRect.y - statsCenter.y) * panelPresentationScale;
                        command.ClipRect.z = statsCenter.x +
                            (command.ClipRect.z - statsCenter.x) * panelPresentationScale;
                        command.ClipRect.w = statsCenter.y +
                            (command.ClipRect.w - statsCenter.y) * panelPresentationScale;
                    }
                }
            };
            transformStats(statsCardDraw, statsCardVertexStart,
                           statsCardVertexEnd);
            if (statsRowsDraw != statsCardDraw)
                transformStats(statsRowsDraw, statsRowsVertexStart,
                               statsRowsVertexEnd);
    }

    // A blacklist encounter is admitted only through the cumulative,
    // colour-validated TAB roster. This excludes lobby/shop bots and keeps a
    // UUID match stable across respawn gaps and later name changes.
    if (!snapshot.matchActive) {
        m_blacklistWarnedCount = 0U;
        m_blacklistMatchWasActive = false;
    } else {
        m_blacklistMatchWasActive = true;
        if (m_blacklist.matchAlertsEnabled) {
            for (std::uint32_t entryIndex = 0U;
                 entryIndex < m_blacklist.count; ++entryIndex) {
                const BlacklistEntry& entry = m_blacklist.entries[entryIndex];
                if (!entry.warnOnEncounter) continue;
                bool encountered = false;
                for (std::uint32_t playerIndex = 0U;
                     playerIndex < snapshot.playerCount; ++playerIndex) {
                    const PlayerIdentity& player = snapshot.players[playerIndex];
                    encountered = entry.idOnly
                        ? ::_stricmp(entry.name.data(), player.name.data()) == 0
                        : entry.uuid[0U] != '\0' && player.uuid[0U] != '\0' &&
                          ::_stricmp(entry.uuid.data(), player.uuid.data()) == 0;
                    if (encountered) break;
                }
                if (!encountered) continue;
                bool alreadyWarned = false;
                for (std::uint32_t warned = 0U;
                     warned < m_blacklistWarnedCount; ++warned) {
                    if (::_stricmp(m_blacklistWarnedKeys[warned].data(),
                                   entry.key.data()) == 0) {
                        alreadyWarned = true;
                        break;
                    }
                }
                if (alreadyWarned) continue;
                if (m_blacklistWarnedCount < m_blacklistWarnedKeys.size()) {
                    std::snprintf(m_blacklistWarnedKeys[m_blacklistWarnedCount].data(),
                        m_blacklistWarnedKeys[m_blacklistWarnedCount].size(), "%s",
                        entry.key.data());
                    ++m_blacklistWarnedCount;
                }
                char message[52]{};
                std::snprintf(message, sizeof(message), "WARNING: %s is blacklisted",
                              entry.name.data());
                enqueueMessage(message, false);
            }
        }
    }
    // showWithClickGui applies to the whole Click GUI session rather than only
    // the Blacklist settings page, so navigation never unexpectedly dismisses
    // the panel while the user is reviewing another category.
    const bool blacklistPanelTarget = m_blacklist.panelEnabled ||
        (m_blacklist.showWithClickGui && interactive);
    advancePresentationSpring(m_blacklistPanelProgress,
                              m_blacklistPanelVelocity,
                              blacklistPanelTarget ? 1.0F : 0.0F, delta);
    if (m_blacklistPanelProgress > 0.005F) {
        const float presentation = std::clamp(m_blacklistPanelProgress, 0.0F, 1.0F);
        const float panelAlphaEase = presentation * presentation *
                                     (3.0F - 2.0F * presentation);
        const float panelScale = 1.26F - 0.26F * m_blacklistPanelProgress;
        const float requestedWidth = std::clamp(330.0F *
            static_cast<float>(m_blacklist.panelWidth) / 100.0F,
            260.0F, std::max(260.0F, io.DisplaySize.x * 0.72F));
        const float expandedHeight = std::clamp(420.0F *
            static_cast<float>(m_blacklist.panelHeight) / 100.0F,
            240.0F, std::max(240.0F, io.DisplaySize.y - 8.0F));
        const float requestedHeight = m_blacklist.collapsed ? 58.0F : expandedHeight;
        const float storedX = m_blacklist.panelX < 0 ? 18.0F
            : io.DisplaySize.x * static_cast<float>(m_blacklist.panelX) / 1000.0F;
        const float storedY = m_blacklist.panelY < 0
            ? std::max(8.0F, (io.DisplaySize.y - requestedHeight) * 0.5F)
            : io.DisplaySize.y * static_cast<float>(m_blacklist.panelY) / 1000.0F;
        const float panelX = std::clamp(storedX, 4.0F,
            std::max(4.0F, io.DisplaySize.x - 260.0F - 4.0F));
        const float panelY = std::clamp(storedY, 4.0F,
            std::max(4.0F, io.DisplaySize.y - 240.0F - 4.0F));
        // Resizing is anchored to the upper-left. If the lower/right edge
        // reaches the viewport, cap the effective size instead of moving the
        // saved top-left in the opposite direction.
        const float width = std::clamp(requestedWidth, 260.0F,
            std::max(260.0F, io.DisplaySize.x - panelX - 4.0F));
        const float minimumHeight = m_blacklist.collapsed ? 58.0F : 240.0F;
        const float height = std::clamp(requestedHeight, minimumHeight,
            std::max(minimumHeight, io.DisplaySize.y - panelY - 4.0F));
        ImGui::SetNextWindowPos(ImVec2(panelX, panelY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panelAlphaEase);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground;
        if (!interactive) flags |= ImGuiWindowFlags_NoInputs;
        ImDrawList* panelDraw = nullptr;
        ImDrawList* entriesDraw = nullptr;
        int panelBegin = 0, panelEnd = 0, entriesBegin = 0, entriesEnd = 0;
        if (ImGui::Begin("##BlacklistPanel", nullptr, flags)) {
            panelDraw = ImGui::GetWindowDrawList();
            panelBegin = panelDraw->VtxBuffer.Size;
            const ImVec2 minimum = ImGui::GetWindowPos();
            const ImVec2 maximum(minimum.x + width, minimum.y + height);
            const int surfaceAlpha = static_cast<int>(std::lround(
                std::clamp(m_blacklist.panelOpacity, 0, 100) * 2.55));
            if (surfaceAlpha < 250) {
                captureBackdropTexture();
                if (m_blurTexture != 0U) {
                    constexpr std::array<ImVec2, 9U> taps{{
                        ImVec2(0,0), ImVec2(-3,0), ImVec2(3,0), ImVec2(0,-3),
                        ImVec2(0,3), ImVec2(-5,-5), ImVec2(5,-5),
                        ImVec2(-5,5), ImVec2(5,5)}};
                    const float exitBlurSpread = 1.0F +
                        (1.0F - presentation) * 2.8F;
                    for (const ImVec2 tap : taps) {
                        const float left = std::clamp(minimum.x +
                            tap.x * exitBlurSpread, 0.0F, io.DisplaySize.x);
                        const float top = std::clamp(minimum.y +
                            tap.y * exitBlurSpread, 0.0F, io.DisplaySize.y);
                        const float right = std::clamp(maximum.x +
                            tap.x * exitBlurSpread, 0.0F, io.DisplaySize.x);
                        const float bottom = std::clamp(maximum.y +
                            tap.y * exitBlurSpread, 0.0F, io.DisplaySize.y);
                        panelDraw->AddImageRounded(reinterpret_cast<ImTextureID>(
                            static_cast<std::uintptr_t>(m_blurTexture)), minimum, maximum,
                            ImVec2(left / io.DisplaySize.x, 1.0F - top / io.DisplaySize.y),
                            ImVec2(right / io.DisplaySize.x, 1.0F - bottom / io.DisplaySize.y),
                            IM_COL32(255,255,255,static_cast<int>(
                                std::lround(36.0F * panelAlphaEase))), 18.0F);
                    }
                }
            }
            // A drop shadow extending below a 58 px collapsed window is
            // clipped by ImGui's rectangular window clip and becomes the
            // straight strip seen under the rounded card. Omit it only for the
            // collapsed state; the expanded surface keeps its elevation.
            if (!m_blacklist.collapsed) {
                panelDraw->AddRectFilled(ImVec2(minimum.x + 4, minimum.y + 7),
                    ImVec2(maximum.x + 4, maximum.y + 7),
                    IM_COL32(0,0,0,72), 18.0F);
            }
            panelDraw->AddRectFilled(minimum, maximum,
                packedRgbColor(m_blacklist.panelColor, surfaceAlpha), 18.0F);
            panelDraw->AddRect(minimum, maximum, IM_COL32(255,255,255,42),
                               18.0F, 0, 1.0F);
            ImFont* const bold = m_boldFonts[static_cast<std::size_t>(
                std::clamp(m_guiScaleIndex, 0, 3))] != nullptr
                ? m_boldFonts[static_cast<std::size_t>(
                    std::clamp(m_guiScaleIndex, 0, 3))] : ImGui::GetFont();
            panelDraw->AddText(bold, ImGui::GetFontSize() * 1.08F,
                ImVec2(minimum.x + 18.0F, minimum.y + 15.0F),
                IM_COL32(252,248,255,255), "BLACKLIST");
            char countLabel[32]{};
            std::snprintf(countLabel, sizeof(countLabel), "%u saved",
                          m_blacklist.count);
            panelDraw->AddText(ImVec2(minimum.x + 18.0F, minimum.y + 39.0F),
                               IM_COL32(190,184,200,255), countLabel);

            const ImVec2 collapseMin(maximum.x - 48.0F, minimum.y + 9.0F);
            const ImVec2 collapseMax(maximum.x - 10.0F, minimum.y + 47.0F);
            panelDraw->AddRectFilled(collapseMin, collapseMax,
                IM_COL32(255,255,255,18), 11.0F);
            const float chevronY = (collapseMin.y + collapseMax.y) * 0.5F;
            const float chevronDirection = m_blacklist.collapsed ? -1.0F : 1.0F;
            panelDraw->AddLine(
                ImVec2(collapseMin.x + 11.0F, chevronY - 4.0F * chevronDirection),
                ImVec2((collapseMin.x + collapseMax.x) * 0.5F, chevronY + 4.0F * chevronDirection),
                IM_COL32(232,226,238,255), 2.0F);
            panelDraw->AddLine(
                ImVec2((collapseMin.x + collapseMax.x) * 0.5F, chevronY + 4.0F * chevronDirection),
                ImVec2(collapseMax.x - 11.0F, chevronY - 4.0F * chevronDirection),
                IM_COL32(232,226,238,255), 2.0F);

            if (interactive && presentation > 0.985F) {
                const ImVec2 resizeMin(maximum.x - 24.0F, maximum.y - 24.0F);
                const bool collapseHovered = ImGui::IsMouseHoveringRect(
                    collapseMin, collapseMax, false);
                const bool resizeHovered = !m_blacklist.collapsed &&
                    ImGui::IsMouseHoveringRect(resizeMin, maximum, false);
                const bool headerHovered = ImGui::IsMouseHoveringRect(
                    minimum, ImVec2(collapseMin.x - 4.0F, minimum.y + 58.0F), false);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (collapseHovered) {
                        m_blacklist.collapsed = !m_blacklist.collapsed;
                        m_blacklistAction = {};
                        m_blacklistAction.type = BlacklistAction::Type::Settings;
                        m_blacklistAction.panelEnabled = m_blacklist.panelEnabled;
                        m_blacklistAction.matchAlertsEnabled = m_blacklist.matchAlertsEnabled;
                        m_blacklistAction.allowIdOnlyNicks = m_blacklist.allowIdOnlyNicks;
                        m_blacklistAction.showWithClickGui = m_blacklist.showWithClickGui;
                        m_blacklistAction.collapsed = m_blacklist.collapsed;
                        m_blacklistAction.panelOpacity = m_blacklist.panelOpacity;
                        m_blacklistAction.panelColor = m_blacklist.panelColor;
                        m_blacklistActionDirty = true;
                    } else if (resizeHovered) {
                        m_blacklistPanelResizing = true;
                        m_blacklistPanelDragging = false;
                        // The default Y is vertically centred and therefore
                        // depends on height. Materialise the current top-left
                        // position before resizing so dragging the lower-right
                        // grip never moves the top edge in the opposite
                        // direction.
                        if (m_blacklist.panelX < 0) {
                            m_blacklist.panelX = std::clamp(
                                static_cast<int>(std::lround(panelX /
                                    std::max(1.0F, io.DisplaySize.x) * 1000.0F)),
                                0, 1000);
                        }
                        if (m_blacklist.panelY < 0) {
                            m_blacklist.panelY = std::clamp(
                                static_cast<int>(std::lround(panelY /
                                    std::max(1.0F, io.DisplaySize.y) * 1000.0F)),
                                0, 1000);
                        }
                        m_blacklistResizeStartMouseX = io.MousePos.x;
                        m_blacklistResizeStartMouseY = io.MousePos.y;
                        m_blacklistResizeStartWidth = m_blacklist.panelWidth;
                        m_blacklistResizeStartHeight = m_blacklist.panelHeight;
                    } else if (headerHovered) {
                        m_blacklistPanelDragging = true;
                        m_blacklistPanelResizing = false;
                        m_blacklistDragStartMouseX = io.MousePos.x;
                        m_blacklistDragStartMouseY = io.MousePos.y;
                        m_blacklistDragStartPanelX = panelX;
                        m_blacklistDragStartPanelY = panelY;
                    }
                }
                if (m_blacklistPanelDragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    const float x = m_blacklistDragStartPanelX +
                        io.MousePos.x - m_blacklistDragStartMouseX;
                    const float y = m_blacklistDragStartPanelY +
                        io.MousePos.y - m_blacklistDragStartMouseY;
                    m_blacklist.panelX = std::clamp(static_cast<int>(std::lround(
                        x / std::max(1.0F, io.DisplaySize.x) * 1000.0F)), 0, 1000);
                    m_blacklist.panelY = std::clamp(static_cast<int>(std::lround(
                        y / std::max(1.0F, io.DisplaySize.y) * 1000.0F)), 0, 1000);
                    m_blacklistPanelTransformDirty = true;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                } else if (m_blacklistPanelResizing &&
                           ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    m_blacklist.panelWidth = std::clamp(
                        m_blacklistResizeStartWidth + static_cast<int>(std::lround(
                            (io.MousePos.x - m_blacklistResizeStartMouseX) * 100.0F / 330.0F)),
                        60, 180);
                    m_blacklist.panelHeight = std::clamp(
                        m_blacklistResizeStartHeight + static_cast<int>(std::lround(
                            (io.MousePos.y - m_blacklistResizeStartMouseY) * 100.0F / 420.0F)),
                        60, 300);
                    m_blacklistPanelTransformDirty = true;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                } else if (resizeHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                else if (headerHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                    (m_blacklistPanelDragging || m_blacklistPanelResizing)) {
                    if (m_blacklistPanelTransformDirty) {
                        m_blacklistAction = {};
                        m_blacklistAction.type = BlacklistAction::Type::Layout;
                        m_blacklistAction.x = m_blacklist.panelX;
                        m_blacklistAction.y = m_blacklist.panelY;
                        m_blacklistAction.width = m_blacklist.panelWidth;
                        m_blacklistAction.height = m_blacklist.panelHeight;
                        m_blacklistActionDirty = true;
                    }
                    m_blacklistPanelTransformDirty = false;
                    m_blacklistPanelDragging = false;
                    m_blacklistPanelResizing = false;
                }
            }

            if (!m_blacklist.collapsed) {
            ImGui::SetCursorScreenPos(ImVec2(minimum.x + 12.0F, minimum.y + 64.0F));
            ImGui::BeginChild("##BlacklistCards", ImVec2(width - 24.0F, height - 112.0F),
                              false, ImGuiWindowFlags_AlwaysVerticalScrollbar |
                              ImGuiWindowFlags_NoBackground);
            entriesDraw = ImGui::GetWindowDrawList();
            entriesBegin = entriesDraw->VtxBuffer.Size;
            for (std::uint32_t index = 0U; index < m_blacklist.count; ++index) {
                const BlacklistEntry& entry = m_blacklist.entries[index];
                ImGui::PushID(static_cast<int>(index));
                const ImVec2 cardMin = ImGui::GetCursorScreenPos();
                const float cardWidth = ImGui::GetContentRegionAvail().x;
                const ImVec2 cardMax(cardMin.x + cardWidth, cardMin.y + 76.0F);
                entriesDraw->AddRectFilled(cardMin, cardMax,
                    IM_COL32(255,255,255,18), 12.0F);
                entriesDraw->AddRect(cardMin, cardMax, IM_COL32(255,255,255,25),
                                     12.0F, 0, 1.0F);
                unsigned faceTexture = 0U;
                if (entry.facePath[0U] != '\0') {
                    BlacklistTexture* slot = nullptr;
                    for (BlacklistTexture& cached : m_blacklistTextures) {
                        if (cached.path[0U] != '\0' &&
                            std::strcmp(cached.path.data(), entry.facePath.data()) == 0) {
                            slot = &cached; break;
                        }
                        if (slot == nullptr && cached.path[0U] == '\0') slot = &cached;
                    }
                    if (slot != nullptr && slot->path[0U] == '\0') {
                        std::snprintf(slot->path.data(), slot->path.size(), "%s",
                                      entry.facePath.data());
                        int imageWidth = 0, imageHeight = 0, channels = 0;
                        unsigned char* pixels = stbi_load(entry.facePath.data(),
                            &imageWidth, &imageHeight, &channels, 4);
                        if (pixels != nullptr && imageWidth > 0 && imageHeight > 0) {
                            GLint lastTexture = 0;
                            ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
                            ::glGenTextures(1, &slot->texture);
                            ::glBindTexture(GL_TEXTURE_2D, slot->texture);
                            ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                            ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                            ::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, imageWidth,
                                imageHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                            ::glBindTexture(GL_TEXTURE_2D,
                                            static_cast<GLuint>(lastTexture));
                        }
                        stbi_image_free(pixels);
                    }
                    if (slot != nullptr) faceTexture = slot->texture;
                }
                const ImVec2 avatarMin(cardMin.x + 10.0F, cardMin.y + 10.0F);
                const ImVec2 avatarMax(avatarMin.x + 46.0F, avatarMin.y + 46.0F);
                entriesDraw->AddRectFilled(avatarMin, avatarMax,
                    IM_COL32(88,72,110,255), 9.0F);
                if (faceTexture != 0U) entriesDraw->AddImageRounded(
                    reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(faceTexture)),
                    avatarMin, avatarMax, ImVec2(0,0), ImVec2(1,1),
                    IM_COL32_WHITE, 9.0F);
                entriesDraw->AddText(bold, ImGui::GetFontSize() * 1.06F,
                    ImVec2(cardMin.x + 68.0F, cardMin.y + 9.0F),
                    IM_COL32(250,248,252,255), entry.name.data());
                if (entry.nick) entriesDraw->AddText(
                    ImVec2(cardMax.x - 49.0F, cardMin.y + 11.0F),
                    IM_COL32(221,125,255,255), "NICK");
                const ImVec4 reasonClip(cardMin.x + 68.0F, cardMin.y + 31.0F,
                                        cardMax.x - 8.0F, cardMin.y + 52.0F);
                entriesDraw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(cardMin.x + 68.0F, cardMin.y + 31.0F),
                    IM_COL32(214,207,220,255), entry.reason.data(), nullptr,
                    0.0F, &reasonClip);
                std::time_t seconds = static_cast<std::time_t>(entry.addedAt / 1000);
                std::tm local{};
                char date[24]{};
                if (::_localtime64_s(&local, &seconds) == 0)
                    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
                entriesDraw->AddText(ImVec2(cardMin.x + 68.0F, cardMin.y + 54.0F),
                    IM_COL32(157,150,166,255), date);
                if (interactive) {
                    ImGui::SetCursorScreenPos(ImVec2(cardMax.x - 64.0F, cardMax.y - 24.0F));
                    if (ImGui::SmallButton("Delete")) {
                        m_blacklistAction = {};
                        m_blacklistAction.type = BlacklistAction::Type::Remove;
                        std::snprintf(m_blacklistAction.key.data(),
                            m_blacklistAction.key.size(), "%s", entry.key.data());
                        m_blacklistActionDirty = true;
                    }
                }
                ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + 7.0F));
                ImGui::Dummy(ImVec2(cardWidth, 1.0F));
                ImGui::PopID();
            }
            entriesEnd = entriesDraw->VtxBuffer.Size;
            ImGui::EndChild();
            const ImVec2 addMin(minimum.x + 14.0F, maximum.y - 38.0F);
            ImGui::SetCursorScreenPos(addMin);
            // Always submit an item after SetCursorScreenPos. The window's
            // NoInputs flag already makes the button inert while the Click
            // GUI is closed; short-circuiting the Button call here left the
            // cursor beyond the previous content boundary and trips ImGui's
            // ErrorCheckUsingSetCursorPosToExtendParentBoundaries assertion.
            if (ImGui::InvisibleButton("##BlacklistAddPlayer",
                                       ImVec2(34.0F, 28.0F)) && interactive) {
                m_previousClickGuiPage = m_clickGuiPage;
                m_clickGuiPage = 11;
                m_clickGuiPageProgress = 0.0F;
                m_blacklistAddOpen = true;
            }
            const bool addHovered = interactive && ImGui::IsItemHovered();
            panelDraw->AddRectFilled(addMin,
                ImVec2(addMin.x + 34.0F, addMin.y + 28.0F),
                addHovered ? IM_COL32(255,255,255,34)
                           : IM_COL32(255,255,255,20), 10.0F);
            const ImVec2 addCenter(addMin.x + 17.0F, addMin.y + 14.0F);
            panelDraw->AddLine(ImVec2(addCenter.x - 5.5F, addCenter.y),
                               ImVec2(addCenter.x + 5.5F, addCenter.y),
                               IM_COL32(242,236,248,255), 2.0F);
            panelDraw->AddLine(ImVec2(addCenter.x, addCenter.y - 5.5F),
                               ImVec2(addCenter.x, addCenter.y + 5.5F),
                               IM_COL32(242,236,248,255), 2.0F);
            }
            panelEnd = panelDraw->VtxBuffer.Size;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        const ImVec2 center(panelX + width * 0.5F, panelY + height * 0.5F);
        const auto transform = [&](ImDrawList* drawList, int begin, int end) noexcept {
            if (drawList == nullptr) return;
            begin = std::clamp(begin, 0, drawList->VtxBuffer.Size);
            end = std::clamp(end, begin, drawList->VtxBuffer.Size);
            for (int vertex = begin; vertex < end; ++vertex) {
                ImDrawVert& drawVertex = drawList->VtxBuffer[vertex];
                if (std::abs(panelScale - 1.0F) >= 0.0001F) {
                    drawVertex.pos.x = center.x +
                        (drawVertex.pos.x - center.x) * panelScale;
                    drawVertex.pos.y = center.y +
                        (drawVertex.pos.y - center.y) * panelScale;
                }
                const unsigned alpha = static_cast<unsigned>(drawVertex.col >> 24U);
                const unsigned faded = static_cast<unsigned>(std::clamp(
                    std::lround(static_cast<float>(alpha) * panelAlphaEase),
                    0L, 255L));
                drawVertex.col = (drawVertex.col & 0x00FFFFFFU) |
                    (faded << 24U);
            }
            if (std::abs(panelScale - 1.0F) >= 0.0001F) {
                for (ImDrawCmd& command : drawList->CmdBuffer) {
                    command.ClipRect.x = center.x +
                        (command.ClipRect.x - center.x) * panelScale;
                    command.ClipRect.y = center.y +
                        (command.ClipRect.y - center.y) * panelScale;
                    command.ClipRect.z = center.x +
                        (command.ClipRect.z - center.x) * panelScale;
                    command.ClipRect.w = center.y +
                        (command.ClipRect.w - center.y) * panelScale;
                }
            }
        };
        transform(panelDraw, panelBegin, panelEnd);
        if (entriesDraw != panelDraw) transform(entriesDraw, entriesBegin, entriesEnd);
    }

    renderToasts(delta, uiScale);

    // Never render a software cursor. Windows remains the only cursor owner,
    // preserving the exact system/game DPI-scaled pointer size and avoiding a
    // second sprite that can race the title-screen cursor.
    io.MouseDrawCursor = false;
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
    if (message == WM_INPUTLANGCHANGE || message == WM_IME_STARTCOMPOSITION ||
        message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION ||
        message == WM_IME_NOTIFY) {
        updateImeState(input, window, message, lParam);
    }
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
            if (::GetCapture() == window) ::ReleaseCapture();
            input.clickGuiToggle.store(true, std::memory_order_release);
            handled = true;
            return 1;
        }
        const unsigned configured = input.menuHotkey.load(std::memory_order_acquire);
        if (static_cast<unsigned>(wParam) == configured || resolvedKey == configured) {
            if (input.interactive.load(std::memory_order_acquire) &&
                ::GetCapture() == window) {
                ::ReleaseCapture();
            }
            input.clickGuiToggle.store(true, std::memory_order_release);
            handled = true;
            return 1;
        }
    }
    ImGuiContext* const context = input.imguiContext.load(std::memory_order_acquire);
    const bool directBackend =
        input.directImGuiWndProc.load(std::memory_order_acquire);
    if (input.acceptImGuiMessages.load(std::memory_order_acquire) &&
        input.interactive.load(std::memory_order_acquire) && context != nullptr &&
        directBackend) {
        ImGui::SetCurrentContext(context);
        (void)ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
    }
    // When the Click GUI is open Minecraft must not receive any relative/raw
    // movement or gameplay key, even if ImGui currently has no hovered item.
    // WM_INPUT is cleaned up through DefWindowProc but never forwarded into the
    // client WndProc. This is what prevents Lunar's camera from rotating.
    const bool clientCursorMessage = message == WM_SETCURSOR &&
        LOWORD(lParam) == HTCLIENT;
    const bool overlayMouseMessage = isMouseMessage(message) &&
        (message != WM_SETCURSOR || clientCursorMessage);
    const bool interactiveNow = input.interactive.load(std::memory_order_acquire);
    if (interactiveNow && !directBackend) {
        // The official backend performs this exact capture transition on its
        // owning thread. Lunar's split presentation thread cannot call that
        // backend from WndProc, so mirror only the Win32 capture portion here.
        // This keeps drag delivery continuous when the pointer crosses a card
        // or the game client boundary; position still comes from one absolute
        // GetCursorPos source on the render thread.
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
            message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN) {
            if (::GetCapture() == nullptr) ::SetCapture(window);
        } else if (message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
                   message == WM_MBUTTONUP || message == WM_XBUTTONUP) {
            const bool anyButtonDown =
                (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0 ||
                (::GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0 ||
                (::GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0;
            if (!anyButtonDown && ::GetCapture() == window) ::ReleaseCapture();
        }
    }
    if (clientCursorMessage && interactiveNow) {
        if (HCURSOR const cursor =
                input.sessionCursor.load(std::memory_order_acquire);
            cursor != nullptr) {
            ::SetCursor(cursor);
            handled = true;
            return TRUE;
        }
    }
    if (interactiveNow &&
        (overlayMouseMessage || isKeyboardMessage(message))) {
        handled = true;
        return message == WM_INPUT
            ? ::DefWindowProcW(window, message, wParam, lParam) : 1;
    }
    handled = false;
    return 0;
}

} // namespace mcoverlay
