// McInjectorLite — minimal pure-Win32 Minecraft injector.
//
// Double-click the executable: it scans for running Minecraft
// (javaw.exe/java.exe) processes, lists them in the console, and lets the
// user pick one with the arrow keys (or W/S). Enter injects the native
// overlay agent (JVM Attach with the visible native-DLL fallback), R
// rescans, Q/Esc exits. Injection status is shown on the same screen; the
// process stays resident so the agent session remains supervised.

#include "WinOverlayManager.h"
#include "WinProcessScanner.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

using namespace cli;

namespace {

HANDLE g_stdout = nullptr;
HANDLE g_stdin = nullptr;
bool g_consoleOutput = false;
constexpr WORD kDefaultAttributes =
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
// FOREGROUND_BLACK is 0 and not always defined by MinGW's wincon.h.
constexpr WORD kSelectedAttributes =
    BACKGROUND_GREEN | BACKGROUND_INTENSITY;

void writeText(const std::wstring &text)
{
    if (g_consoleOutput) {
        DWORD written = 0;
        (void) ::WriteConsoleW(g_stdout, text.data(),
                               static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return;
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    (void) ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                 static_cast<int>(text.size()), utf8.data(),
                                 needed, nullptr, nullptr);
    DWORD written = 0;
    (void) ::WriteFile(g_stdout, utf8.data(), static_cast<DWORD>(utf8.size()),
                       &written, nullptr);
}

void writeLine(const std::wstring &line)
{
    writeText(line + L"\r\n");
}

void writeLineStyled(const std::wstring &line, const WORD attributes)
{
    if (g_consoleOutput)
        ::SetConsoleTextAttribute(g_stdout, attributes);
    writeLine(line);
    if (g_consoleOutput)
        ::SetConsoleTextAttribute(g_stdout, kDefaultAttributes);
}

void clearScreen()
{
    if (!g_consoleOutput)
        return;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (::GetConsoleScreenBufferInfo(g_stdout, &info) == FALSE)
        return;
    const DWORD size = static_cast<DWORD>(info.dwSize.X * info.dwSize.Y);
    DWORD written = 0;
    (void) ::FillConsoleOutputCharacterW(g_stdout, L' ', size, {0, 0}, &written);
    (void) ::FillConsoleOutputAttribute(g_stdout, info.wAttributes, size,
                                        {0, 0}, &written);
    (void) ::SetConsoleCursorPosition(g_stdout, {0, 0});
}

std::wstring stateText(const WinOverlayManager::State state)
{
    switch (state) {
    case WinOverlayManager::State::Detached:
        return L"未注入";
    case WinOverlayManager::State::Validating:
        return L"正在验证目标进程…";
    case WinOverlayManager::State::StartingIpc:
        return L"正在建立 IPC 管道…";
    case WinOverlayManager::State::LaunchingAttachHelper:
        return L"正在启动注入器…";
    case WinOverlayManager::State::WaitingForAgent:
        return L"等待 Agent 认证握手…";
    case WinOverlayManager::State::WaitingForOpenGL:
        return L"已注入 — 等待游戏 OpenGL 首帧…";
    case WinOverlayManager::State::Active:
        return L"注入成功 — 覆盖层已激活（游戏内按 ' 打开菜单）";
    case WinOverlayManager::State::Detaching:
        return L"正在断开…";
    case WinOverlayManager::State::Error:
        return L"注入失败";
    }
    return {};
}

struct App
{
    WinOverlayManager manager;
    std::vector<JavaProcess> processes;
    int selection = 0;
    bool dirty = true;
    bool running = true;
    bool consoleInput = false;

    void init()
    {
        processes = scanJavaProcesses();
        manager.onStateChanged = [this] { dirty = true; };
        manager.onStatus = [this](const std::wstring &) { dirty = true; };
        manager.onError = [this](const std::wstring &, const std::wstring &) {
            dirty = true;
        };
        manager.onRenderer = [this](const std::wstring &) { dirty = true; };
        manager.onTargetExited = [this](uint32_t) {
            processes = scanJavaProcesses();
            selection = 0;
            dirty = true;
        };
    }

    void clampSelection()
    {
        if (processes.empty()) {
            selection = 0;
            return;
        }
        if (selection < 0)
            selection = 0;
        if (selection >= static_cast<int>(processes.size()))
            selection = static_cast<int>(processes.size()) - 1;
    }

    void rescan()
    {
        processes = scanJavaProcesses();
        clampSelection();
        dirty = true;
    }

    void attachSelection()
    {
        if (manager.busy())
            return;
        if (processes.empty()) {
            rescan();
            return;
        }
        clampSelection();
        manager.attachToProcess(processes.at(static_cast<std::size_t>(selection)).pid);
        dirty = true;
    }

    void consumeInput()
    {
        for (int iteration = 0; iteration < 32 && running; ++iteration) {
            INPUT_RECORD record{};
            DWORD count = 0;
            if (::PeekConsoleInputW(g_stdin, &record, 1, &count) == FALSE
                || count == 0) {
                break;
            }
            if (::ReadConsoleInputW(g_stdin, &record, 1, &count) == FALSE
                || count != 1) {
                break;
            }
            if (record.EventType != KEY_EVENT
                || record.Event.KeyEvent.bKeyDown == FALSE) {
                continue;
            }
            handleKey(record.Event.KeyEvent.wVirtualKeyCode,
                      record.Event.KeyEvent.uChar.UnicodeChar);
        }
    }

    void handleKey(const WORD virtualKey, const wchar_t character)
    {
        const bool up = virtualKey == VK_UP || character == L'w' || character == L'W';
        const bool down = virtualKey == VK_DOWN || character == L's' || character == L'S';
        const bool quit = virtualKey == VK_ESCAPE || character == L'q' || character == L'Q';

        if (quit) {
            running = false;
            return;
        }
        if (character == L'r' || character == L'R') {
            rescan();
            return;
        }

        if (manager.state() == WinOverlayManager::State::Error) {
            if (virtualKey == VK_RETURN)
                rescan();
            return;
        }

        if (up) {
            --selection;
            clampSelection();
            dirty = true;
        } else if (down) {
            ++selection;
            clampSelection();
            dirty = true;
        } else if (virtualKey == VK_RETURN) {
            attachSelection();
        }
    }

    void render()
    {
        clearScreen();
        writeLine(L"McInjectorLite · Minecraft 极简注入器");
        writeLine(L"--------------------------------------------");
        if (processes.empty()) {
            writeLine(L"");
            writeLine(L"  未发现 Minecraft 进程 — 按 R 重新扫描");
        } else {
            for (std::size_t index = 0; index < processes.size(); ++index) {
                const JavaProcess &process = processes.at(index);
                std::wstring title = process.windowTitle;
                if (title.size() > 36) {
                    title.resize(36);
                    title += L"...";
                }
                while (title.size() < 39)
                    title += L' ';
                const std::wstring row =
                    (static_cast<int>(index) == selection ? L"  > " : L"    ")
                    + std::to_wstring(index + 1) + L". " + title
                    + L" PID " + std::to_wstring(process.pid)
                    + L"   " + process.memoryText;
                if (static_cast<int>(index) == selection) {
                    writeLineStyled(row, kSelectedAttributes);
                } else {
                    writeLine(row);
                }
            }
        }
        writeLine(L"");
        writeLine(L"状态: " + stateText(manager.state()));
        if (manager.state() == WinOverlayManager::State::Error) {
            writeLine(L"错误: " + manager.errorCode() + L" — "
                      + manager.errorDetail());
        } else if (!manager.statusMessage().empty()
                   && manager.state() != WinOverlayManager::State::Detached) {
            writeLine(L"详情: " + manager.statusMessage());
        }
        if (manager.state() == WinOverlayManager::State::Error) {
            writeLine(L"Enter 返回列表   Q 退出");
        } else {
            writeLine(L"↑/↓ 选择   Enter 注入   R 刷新   Q 退出");
        }
    }
};

} // namespace

int wmain(int argc, wchar_t **argv)
{
    g_stdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    g_stdin = ::GetStdHandle(STD_INPUT_HANDLE);

    DWORD consoleMode = 0;
    g_consoleOutput = ::GetConsoleMode(g_stdout, &consoleMode) != FALSE;
    DWORD originalInputMode = 0;
    const bool consoleInput =
        ::GetConsoleMode(g_stdin, &originalInputMode) != FALSE;
    if (consoleInput) {
        // Raw key input: no line buffering, no echo. Ctrl+C processing stays.
        (void) ::SetConsoleMode(g_stdin, (originalInputMode
                                          & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
                                             | ENABLE_EXTENDED_FLAGS);
    }

    App app;
    app.init();
    app.consoleInput = consoleInput;

    // Optional one-shot form: McInjectorLite.exe --attach <序号|PID>
    for (int index = 1; index < argc; ++index) {
        if (std::wstring(argv[index]) == L"--attach" && index + 1 < argc) {
            app.rescan();
            wchar_t *end = nullptr;
            const unsigned long value = wcstoul(argv[index + 1], &end, 10);
            if (end != argv[index + 1] && *end == L'\0' && value > 0) {
                for (std::size_t row = 0; row < app.processes.size(); ++row) {
                    if (app.processes.at(row).pid == value
                        || value == row + 1) {
                        app.selection = static_cast<int>(row);
                        break;
                    }
                }
                app.attachSelection();
            }
            ++index;
        }
    }

    app.render();

    // Scripted mode (redirected stdin, e.g. `McInjectorLite.exe --attach 2`):
    // no TUI. With --attach, wait for the injection result and exit; without
    // it, the process list above is the whole output.
    if (!consoleInput) {
        const bool attached = app.manager.targetPid() != 0
            || app.manager.busy()
            || app.manager.state() == WinOverlayManager::State::Error;
        if (!attached)
            return EXIT_SUCCESS;

        const std::array<HANDLE, 2> scriptHandles = {
            app.manager.pipeEvent(), app.manager.helperEvent()};
        const unsigned long long deadline =
            ::GetTickCount64() + 30'000ULL; // bound scripted runs
        const auto finished = [&app] {
            return app.manager.state() == WinOverlayManager::State::Active
                || app.manager.state() == WinOverlayManager::State::WaitingForOpenGL
                || app.manager.state() == WinOverlayManager::State::Error;
        };
        while (app.running && !finished() && ::GetTickCount64() < deadline) {
            const DWORD result = ::MsgWaitForMultipleObjectsEx(
                static_cast<DWORD>(scriptHandles.size()), scriptHandles.data(),
                400, 0, 0);
            if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1)
                app.manager.processMessages();
            app.manager.onTick();
            app.manager.postDrain();
            if (app.dirty) {
                app.render();
                app.dirty = false;
            }
        }
        if (app.manager.state() == WinOverlayManager::State::Active) {
            writeLine(L"");
            writeLine(L"注入成功 — 覆盖层已激活");
        } else if (app.manager.state() == WinOverlayManager::State::WaitingForOpenGL) {
            writeLine(L"");
            writeLine(L"注入成功 — Agent 已加载，等待游戏首帧");
        } else if (app.manager.state() == WinOverlayManager::State::Error) {
            writeLine(L"");
            writeLine(L"注入失败: " + app.manager.errorCode() + L" — "
                      + app.manager.errorDetail());
        } else {
            writeLine(L"");
            writeLine(L"注入超时或未完成");
        }
        app.manager.shutdown();
        return EXIT_SUCCESS;
    }

    const std::array<HANDLE, 3> waitHandles = {
        g_stdin, app.manager.pipeEvent(), app.manager.helperEvent()};

    while (app.running) {
        const DWORD result = ::MsgWaitForMultipleObjectsEx(
            static_cast<DWORD>(waitHandles.size()), waitHandles.data(), 400,
            QS_ALLINPUT, 0);

        if (result == WAIT_OBJECT_0) {
            app.consumeInput();
        } else if (result == WAIT_OBJECT_0 + 1 || result == WAIT_OBJECT_0 + 2) {
            app.manager.processMessages();
        } else if (result == WAIT_OBJECT_0 + waitHandles.size()) {
            MSG message{};
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }

        app.manager.onTick();
        app.manager.postDrain();
        if (app.dirty) {
            app.render();
            app.dirty = false;
        }
    }

    app.manager.shutdown();
    (void) ::SetConsoleMode(g_stdin, originalInputMode);
    return EXIT_SUCCESS;
}
