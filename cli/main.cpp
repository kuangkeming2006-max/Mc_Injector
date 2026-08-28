// McInjectorLite — a pure C++/Win32 terminal injector for the native
// Minecraft overlay agent. No Qt, no runtime DLLs: double-clicking the
// executable opens a console REPL that scans Java processes, manages the
// DPAPI-protected Hypixel API key, injects the agent (JVM Attach with the
// visible native-DLL fallback), and follows the live telemetry/statistics
// pipeline. The agent's own Click GUI still provides in-game feature toggles.

#include "ConsoleIo.h"
#include "HypixelService.h"
#include "PlayerStatsService.h"
#include "WinApiKeyStore.h"
#include "WinOverlayManager.h"
#include "WinProcessScanner.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace cli;

namespace {

WinOverlayManager *g_manager = nullptr;
HWND g_messageWindow = nullptr;
HANDLE g_shutdownEvent = nullptr;

std::wstring formatDouble(const double value, const int precision = 1)
{
    wchar_t buffer[64]{};
    swprintf(buffer, std::size(buffer), L"%.*f", precision, value);
    return buffer;
}

// ---------------------------------------------------------------------------
// Timer bridge: the manager's timer ids map directly onto window timers of a
// message-only window.
// ---------------------------------------------------------------------------

LRESULT CALLBACK messageWindowProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam)
{
    if (message == WM_TIMER && g_manager != nullptr) {
        g_manager->onTimer(static_cast<WinOverlayManager::TimerId>(wParam));
        return 0;
    }
    if (message == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// REPL
// ---------------------------------------------------------------------------

struct Repl
{
    WinApiKeyStore keys;
    WinOverlayManager manager;
    HypixelService hypixel{&keys};
    PlayerStatsService stats{&keys};

    std::vector<JavaProcess> processes;
    uint32_t selectedPid = 0;
    uint64_t lastPrintedSequence = 0;
    unsigned long long lastTelemetryPrintTick = 0;
    bool quitting = false;
    bool helpShown = false;

    bool start()
    {
        g_manager = &manager;

        // Freshness marking runs every second regardless of session state.
        manager.requestTimer = [](const WinOverlayManager::TimerId id,
                                  const UINT milliseconds) {
            if (g_messageWindow != nullptr) {
                ::SetTimer(g_messageWindow, static_cast<UINT_PTR>(id),
                           milliseconds, nullptr);
            }
        };
        manager.cancelTimer = [](const WinOverlayManager::TimerId id) {
            if (g_messageWindow != nullptr)
                ::KillTimer(g_messageWindow, static_cast<UINT_PTR>(id));
        };
        manager.requestTimer(WinOverlayManager::TimerFreshness, 1000);

        wireCallbacks();

        hypixel.start();
        stats.start();
        return true;
    }

    void wireCallbacks()
    {
        const auto publishHypixelToAgent = [this] {
            const HypixelService::Result &result = hypixel.result();
            const std::wstring status = hypixel.errorMessage().empty()
                ? hypixel.statusMessage()
                : hypixel.errorMessage();
            manager.publishHypixelResult(
                static_cast<int>(hypixel.state()), result.uuid,
                result.displayName, result.wins, result.losses,
                result.finalKills, result.finalDeaths, result.bedsBroken,
                result.bedsLost, result.winRate, result.fkdr, status);
        };

        manager.onStateChanged = [this] { console::print(L"[状态] " + stateLabel()); };
        manager.onStatus = [this](const std::wstring &status) {
            console::print(L"[状态] " + status);
        };
        manager.onError = [this](const std::wstring &code,
                                 const std::wstring &detail) {
            if (!code.empty())
                console::print(L"[错误] " + code + L": " + detail);
        };
        manager.onRenderer = [](const std::wstring &renderer) {
            if (!renderer.empty())
                console::print(L"[渲染器] " + renderer);
        };
        manager.onTelemetry = [this] { printTelemetry(); };
        manager.onPlayerName = [this](const std::wstring &name) {
            if (!name.empty())
                console::print(L"[玩家] 当前玩家: " + name);
        };
        manager.onMatchState = [this](const bool active) {
            console::print(active
                               ? L"[对局] 进入 Bed Wars 对局，自动统计查询已启动"
                               : L"[对局] Bed Wars 对局结束");
        };
        manager.onPlayerFound = [this](const std::wstring &playerName,
                                       const std::wstring &teamPrefix) {
            console::print(L"[队伍] 发现玩家 " + playerName + L"（队伍色 "
                           + teamPrefix + L"），已进入自动查询队列");
            stats.enqueuePlayer(playerName, teamPrefix);
        };
        manager.onHypixelQuery = [this](const std::wstring &playerId) {
            console::print(L"[Hypixel] 游戏内请求查询 " + playerId + L" ...");
            hypixel.lookupPlayer(playerId);
        };
        manager.onTargetExited = [this](const uint32_t pid) {
            console::print(L"[提示] 目标进程 " + std::to_wstring(pid)
                           + L" 已退出，注入会话结束。");
            selectedPid = 0;
            processes = scanJavaProcesses();
            printProcesses();
        };
        manager.onMenuHotkeyChanged = [](const int virtualKey) {
            console::print(L"[设置] 游戏内 Click GUI 呼出键已更新为虚拟键码 "
                           + std::to_wstring(virtualKey));
        };
        manager.onGuiScaleIndexChanged = [](const int index) {
            console::print(L"[设置] 游戏内界面尺寸已更新为 "
                           + std::to_wstring(index));
        };
        manager.onSessionReady = [this, publishHypixelToAgent] {
            publishHypixelToAgent();
        };

        stats.onStatsReady = [this](const std::wstring &playerName,
                                    const std::wstring &teamPrefix,
                                    const int stars, const double fkdr,
                                    const int level) {
            manager.publishPlayerStats(playerName, teamPrefix, stars, fkdr,
                                       level);
            console::print(L"[统计] " + playerName + L"（" + teamPrefix
                           + L"） 星级 " + std::to_wstring(stars)
                           + L"  FKDR " + formatDouble(fkdr, 2)
                           + L"  网络等级 " + std::to_wstring(level));
        };
        stats.onStatsFailed = [this](const std::wstring &playerName,
                                     const std::wstring &reason) {
            manager.publishPlayerStatsError(playerName, reason);
            console::print(L"[统计] " + playerName + L" 查询失败: " + reason);
        };

        hypixel.onChanged = [this, publishHypixelToAgent] {
            publishHypixelToAgent();
        };
        hypixel.onResultReady = [this] {
            const HypixelService::Result &result = hypixel.result();
            if (result.displayName.empty())
                return;
            console::print(L"[Hypixel] " + result.displayName
                           + L"  胜 " + std::to_wstring(result.wins)
                           + L"  负 " + std::to_wstring(result.losses)
                           + L"  终杀 " + std::to_wstring(result.finalKills)
                           + L"  终死 " + std::to_wstring(result.finalDeaths)
                           + L"  拆床 " + std::to_wstring(result.bedsBroken)
                           + L"  失床 " + std::to_wstring(result.bedsLost)
                           + L"  胜率 " + formatDouble(result.winRate, 2)
                           + L"  FKDR " + formatDouble(result.fkdr, 2));
        };
    }

    std::wstring stateLabel() const
    {
        switch (manager.state()) {
        case WinOverlayManager::State::Detached:
            return L"已断开（等待选择进程）";
        case WinOverlayManager::State::Validating:
            return L"正在验证目标进程…";
        case WinOverlayManager::State::StartingIpc:
            return L"正在建立 IPC 管道…";
        case WinOverlayManager::State::LaunchingAttachHelper:
            return L"正在启动注入器…";
        case WinOverlayManager::State::WaitingForAgent:
            return L"等待 Agent 认证握手…";
        case WinOverlayManager::State::WaitingForOpenGL:
            return L"等待 Minecraft 的 OpenGL 首帧…";
        case WinOverlayManager::State::Active:
            return L"覆盖层已激活";
        case WinOverlayManager::State::Detaching:
            return L"正在断开…";
        case WinOverlayManager::State::Error:
            return L"错误";
        }
        return {};
    }

    void printTelemetry()
    {
        const GameSnapshot &game = manager.game();
        if (!game.received)
            return;
        if (lastPrintedSequence == game.sequence
            && lastTelemetryPrintTick != 0) {
            return;
        }
        const unsigned long long now = ::GetTickCount64();
        if (lastTelemetryPrintTick != 0 && now - lastTelemetryPrintTick < 1000)
            return;
        lastPrintedSequence = game.sequence;
        lastTelemetryPrintTick = now;

        if (!game.available) {
            console::print(L"[遥测] 映射未就绪: " + game.mappingProfile + L" ("
                           + game.mappingState + L")");
            return;
        }
        console::print(L"[遥测] 生命 " + formatDouble(game.health) + L"/"
                       + formatDouble(game.maxHealth)
                       + L"  坐标 (" + formatDouble(game.x) + L", "
                       + formatDouble(game.y) + L", "
                       + formatDouble(game.z) + L")"
                       + L"  实体 " + std::to_wstring(game.loadedEntities)
                       + L"  床 " + std::to_wstring(game.bedCount)
                       + L"  映射 " + game.mappingProfile + L"("
                       + game.mappingState + L")"
                       + L"  seq " + std::to_wstring(game.sequence));
    }

    void printBanner()
    {
        console::print(L"======================================================");
        console::print(L"  McInjectorLite — 轻量版 Minecraft 覆盖层注入器（纯 Win32 终端版）");
        console::print(L"  与 MinecraftOverlayManager 共用同一 Agent 与注入后端");
        console::print(L"======================================================");
    }

    void printHelp()
    {
        helpShown = true;
        console::print(L"命令说明:");
        console::print(L"  进程选择与注入:");
        console::print(L"    list, l          扫描运行中的 java.exe / javaw.exe 进程");
        console::print(L"    select <序号|PID> 选择要注入的进程");
        console::print(L"    attach [序号|PID] 注入覆盖层（优先 JVM Attach，自动回退原生 DLL 加载）");
        console::print(L"    detach, d        优雅断开当前注入会话");
        console::print(L"    status, st       查看会话状态与游戏遥测");
        console::print(L"");
        console::print(L"  Hypixel API:");
        console::print(L"    key, k           查看 API Key 状态");
        console::print(L"    setkey <KEY>     保存 Hypixel API Key（Windows DPAPI 用户级加密）");
        console::print(L"    clearkey         删除已保存的 API Key");
        console::print(L"    query <玩家名>   手动查询玩家的 Bed Wars 数据");
        console::print(L"");
        console::print(L"  游戏内控制:");
        console::print(L"    bedrescan        请求立即刷新床缓存");
        console::print(L"    hotkey <VK>      设置 Click GUI 呼出键虚拟键码（默认 0xDE 单引号）");
        console::print(L"    scale <0-3>      设置游戏内界面尺寸 0=S 1=M 2=L 3=XL");
        console::print(L"");
        console::print(L"  其他:");
        console::print(L"    help, ?          显示本帮助");
        console::print(L"    quit, exit       退出");
        console::print(L"");
        console::print(L"  游戏内按单引号键打开 Click GUI，可开关各项功能。");
    }

    void printProcesses()
    {
        if (processes.empty()) {
            console::print(L"（未发现 java.exe/javaw.exe 进程。请先启动 Minecraft 1.8.9。）");
            return;
        }
        console::print(L"序号   PID       窗口标题                                    内存");
        for (std::size_t index = 0; index < processes.size(); ++index) {
            const JavaProcess &process = processes.at(index);
            const std::wstring marker =
                process.pid == selectedPid ? L"* " : L"  ";
            std::wstring title = process.windowTitle;
            if (title.size() > 40)
                title = title.substr(0, 40);
            while (title.size() < 40)
                title += L' ';
            console::print(marker + std::to_wstring(index + 1) + L"  "
                           + std::to_wstring(process.pid) + L"    "
                           + title + L" " + process.memoryText);
        }
    }

    void printKeyStatus()
    {
        console::print(std::wstring(L"Hypixel Key: ")
                       + (keys.configured() ? L"已配置（DPAPI 加密存储）"
                                            : L"未配置（输入 setkey <KEY> 保存）")
                       + L" — " + keys.statusMessage());
    }

    void printSessionStatus()
    {
        console::print(L"会话状态: " + stateLabel());
        if (manager.targetPid() != 0) {
            console::print(L"目标进程: PID " + std::to_wstring(manager.targetPid())
                           + L" — " + manager.targetTitle());
        }
        if (!manager.renderer().empty())
            console::print(L"渲染器: " + manager.renderer());
        const GameSnapshot &game = manager.game();
        if (game.received) {
            console::print(std::wstring(L"遥测: ")
                           + (game.available ? L"可用" : L"映射不可用")
                           + L" | 生命 " + formatDouble(game.health)
                           + L"/" + formatDouble(game.maxHealth)
                           + L" | 坐标 (" + formatDouble(game.x) + L", "
                           + formatDouble(game.y) + L", "
                           + formatDouble(game.z) + L")"
                           + L" | 实体 " + std::to_wstring(game.loadedEntities)
                           + L" | 床 " + std::to_wstring(game.bedCount)
                           + L" | 映射 " + game.mappingProfile + L" ("
                           + game.mappingState + L")"
                           + L" | seq " + std::to_wstring(game.sequence));
        } else {
            console::print(L"遥测: 尚未收到 GAME_STATE 数据");
        }
        if (!manager.playerName().empty())
            console::print(L"当前玩家: " + manager.playerName());
        console::print(std::wstring(L"对局: ")
                       + (manager.matchActive() ? L"Bed Wars 进行中" : L"无"));
        printKeyStatus();
    }

    uint32_t parseTarget(const std::wstring &token) const
    {
        wchar_t *end = nullptr;
        const unsigned long value = wcstoul(token.c_str(), &end, 10);
        if (end == token.c_str() || *end != L'\0' || value == 0
            || value > UINT32_MAX) {
            return 0;
        }
        const uint32_t numeric = static_cast<uint32_t>(value);
        for (const JavaProcess &process : processes) {
            if (process.pid == numeric)
                return numeric;
        }
        if (numeric <= processes.size())
            return processes.at(numeric - 1).pid;
        return 0;
    }

    void handleSelect(const std::vector<std::wstring> &parts)
    {
        if (parts.size() < 2) {
            if (selectedPid != 0)
                console::print(L"当前选择: PID " + std::to_wstring(selectedPid));
            else
                console::print(L"尚未选择进程（select <序号|PID>，用 list 查看列表）");
            return;
        }
        processes = scanJavaProcesses();
        const uint32_t pid = parseTarget(parts.at(1));
        if (pid == 0) {
            console::print(L"[错误] 无效的序号/PID: " + parts.at(1)
                           + L"（先运行 list 查看进程）");
            return;
        }
        selectedPid = pid;
        console::print(L"已选择 PID " + std::to_wstring(pid)
                       + L"（输入 attach 注入）");
    }

    void handleAttach(const std::vector<std::wstring> &parts)
    {
        if (manager.busy()) {
            console::print(L"[提示] 已有注入操作在进行中，请稍候。");
            return;
        }
        uint32_t pid = 0;
        if (parts.size() > 1) {
            processes = scanJavaProcesses();
            pid = parseTarget(parts.at(1));
            if (pid == 0) {
                console::print(L"[错误] 无效的序号/PID: " + parts.at(1)
                               + L"（先运行 list 查看进程）");
                return;
            }
        } else {
            pid = selectedPid;
            if (pid == 0) {
                processes = scanJavaProcesses();
                if (processes.size() == 1) {
                    pid = processes.front().pid;
                } else {
                    console::print(L"[提示] 请先选择进程: attach <序号|PID>（运行 list 查看）");
                    return;
                }
            }
        }
        selectedPid = pid;
        console::print(L"正在注入 PID " + std::to_wstring(pid) + L" …");
        manager.attachToProcess(pid);
    }

    void handleCommand(const std::wstring &line)
    {
        std::vector<std::wstring> parts;
        std::size_t begin = 0;
        while (begin < line.size()) {
            while (begin < line.size() && iswspace(line[begin]) != 0)
                ++begin;
            if (begin >= line.size())
                break;
            std::size_t end = line.find(L' ', begin);
            if (end == std::wstring::npos)
                end = line.size();
            parts.push_back(line.substr(begin, end - begin));
            begin = end + 1;
        }
        if (parts.empty()) {
            console::prompt();
            return;
        }

        std::wstring command = parts.front();
        for (auto &character : command)
            character = towlower(character);

        if (command == L"help" || command == L"?" || command == L"h") {
            printHelp();
        } else if (command == L"list" || command == L"scan" || command == L"l") {
            processes = scanJavaProcesses();
            printProcesses();
        } else if (command == L"select" || command == L"s") {
            handleSelect(parts);
        } else if (command == L"attach" || command == L"a") {
            handleAttach(parts);
        } else if (command == L"detach" || command == L"d") {
            if (!manager.attached()
                && manager.state() == WinOverlayManager::State::Detached) {
                console::print(L"[提示] 当前没有注入会话。");
            } else {
                console::print(L"正在断开…");
                manager.detach();
            }
        } else if (command == L"status" || command == L"st") {
            printSessionStatus();
        } else if (command == L"key" || command == L"k") {
            printKeyStatus();
        } else if (command == L"setkey") {
            if (parts.size() < 2) {
                console::print(L"用法: setkey <API_KEY>（在 developer.hypixel.net 注册应用获取）");
            } else if (!keys.saveKey(parts.at(1))) {
                console::print(L"[错误] " + keys.statusMessage());
            } else {
                console::print(L"[密钥] " + keys.statusMessage());
                hypixel.reloadConfiguration();
                stats.reloadConfiguration();
            }
        } else if (command == L"clearkey") {
            keys.clearKey();
            console::print(L"[密钥] " + keys.statusMessage());
            hypixel.reloadConfiguration();
            stats.reloadConfiguration();
        } else if (command == L"query" || command == L"q") {
            if (parts.size() < 2) {
                console::print(L"用法: query <玩家名>");
            } else {
                console::print(L"[Hypixel] 正在查询 " + parts.at(1) + L" …");
                hypixel.lookupPlayer(parts.at(1));
            }
        } else if (command == L"bedrescan") {
            if (!manager.attached()) {
                console::print(L"[提示] 尚未注入任何进程。");
            } else {
                manager.refreshBedCache();
                console::print(L"[提示] 已请求刷新床缓存。");
            }
        } else if (command == L"hotkey") {
            if (parts.size() < 2) {
                console::print(L"用法: hotkey <虚拟键码>（当前: "
                               + std::to_wstring(manager.menuHotkey()) + L"）");
            } else {
                wchar_t *end = nullptr;
                const long virtualKey = wcstol(parts.at(1).c_str(), &end, 10);
                if (end == parts.at(1).c_str() || *end != L'\0'
                    || virtualKey < 8 || virtualKey > 254) {
                    console::print(L"[错误] 虚拟键码需在 8..254 之间。");
                } else {
                    manager.setMenuHotkey(static_cast<int>(virtualKey));
                }
            }
        } else if (command == L"scale") {
            if (parts.size() < 2) {
                console::print(L"用法: scale <0-3>（0=S 1=M 2=L 3=XL，当前: "
                               + std::to_wstring(manager.guiScaleIndex()) + L"）");
            } else {
                wchar_t *end = nullptr;
                const long index = wcstol(parts.at(1).c_str(), &end, 10);
                if (end == parts.at(1).c_str() || *end != L'\0'
                    || index < 0 || index > 3) {
                    console::print(L"[错误] 尺寸需在 0..3 之间。");
                } else {
                    manager.setGuiScaleIndex(static_cast<int>(index));
                }
            }
        } else if (command == L"quit" || command == L"exit") {
            console::print(L"再见。注入会话将优雅断开。");
            quitting = true;
            ::SetEvent(g_shutdownEvent);
            return;
        } else {
            console::print(L"[错误] 未知命令: " + command
                           + L"（输入 help 查看帮助）");
        }
        console::prompt();
    }

    void processArguments(const std::vector<std::wstring> &arguments)
    {
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const std::wstring &argument = arguments.at(index);
            if (argument == L"--help" || argument == L"-h") {
                printHelp();
                ::SetEvent(g_shutdownEvent);
                return;
            }
            if (argument == L"--list") {
                processes = scanJavaProcesses();
                printProcesses();
                ::SetEvent(g_shutdownEvent);
                return;
            }
            if ((argument == L"--attach" || argument == L"--pid")
                && index + 1 < arguments.size()) {
                processes = scanJavaProcesses();
                const uint32_t pid = parseTarget(arguments.at(index + 1));
                ++index;
                if (pid == 0) {
                    console::print(L"[错误] 无效的进程序号/PID: "
                                   + arguments.at(index));
                } else {
                    selectedPid = pid;
                    manager.attachToProcess(pid);
                }
                continue;
            }
            if (argument == L"--key" && index + 1 < arguments.size()) {
                ++index;
                (void) keys.saveKey(arguments.at(index));
                console::print(L"[密钥] " + keys.statusMessage());
                hypixel.reloadConfiguration();
                stats.reloadConfiguration();
                continue;
            }
            if (argument == L"--clear-key") {
                keys.clearKey();
                console::print(L"[密钥] " + keys.statusMessage());
                hypixel.reloadConfiguration();
                stats.reloadConfiguration();
                continue;
            }
            if (argument == L"--query" && index + 1 < arguments.size()) {
                ++index;
                console::print(L"[Hypixel] 正在查询 " + arguments.at(index)
                               + L" …");
                hypixel.lookupPlayer(arguments.at(index));
                continue;
            }
            console::print(L"[错误] 未知参数: " + argument);
            ::SetEvent(g_shutdownEvent);
            return;
        }
    }
};

} // namespace

int wmain(int argc, wchar_t **argv)
{
    console::initialize();

    g_shutdownEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Message-only window carrying the manager's one-shot/repeating timers.
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = messageWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"McInjectorLiteMessageWindow";
    if (::RegisterClassW(&windowClass) == 0
        && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        console::print(L"[错误] 无法注册消息窗口类。");
        return EXIT_FAILURE;
    }
    g_messageWindow = ::CreateWindowExW(
        0, windowClass.lpszClassName, L"McInjectorLite", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    if (g_messageWindow == nullptr) {
        console::print(L"[错误] 无法创建消息窗口。");
        return EXIT_FAILURE;
    }

    Repl repl;
    if (!repl.start()) {
        console::print(L"[错误] 初始化失败。");
        return EXIT_FAILURE;
    }

    // Startup banner, key status, and the initial process list.
    repl.printBanner();
    repl.printKeyStatus();
    repl.processes = scanJavaProcesses();
    repl.printProcesses();
    repl.printHelp();
    console::print(L"");
    console::prompt();

    repl.processArguments(std::vector<std::wstring>(argv, argv + argc));

    // Blocking stdin reader thread feeding the REPL through the main loop.
    console::startInput([&repl](const std::optional<std::wstring> &line) {
        if (!line.has_value()) {
            console::print(L"");
            console::print(L"[提示] 标准输入已关闭，退出。");
            ::SetEvent(g_shutdownEvent);
            return;
        }
        repl.handleCommand(*line);
    });

    const std::array<HANDLE, 5> waitHandles = {
        console::inputEvent(), repl.manager.pipeEvent(),
        repl.hypixel.event(), repl.stats.event(), g_shutdownEvent};

    bool running = true;
    while (running && !repl.quitting) {
        const DWORD result = ::MsgWaitForMultipleObjectsEx(
            static_cast<DWORD>(waitHandles.size()), waitHandles.data(),
            INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        const DWORD index = result - WAIT_OBJECT_0;

        if (result == WAIT_OBJECT_0 + waitHandles.size()) {
            // A message arrived (WM_TIMER or WM_PAINT on the hidden window).
            MSG message{};
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                if (message.message == WM_QUIT) {
                    running = false;
                    break;
                }
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            continue;
        }

        if (result >= WAIT_OBJECT_0
            && result < WAIT_OBJECT_0 + waitHandles.size()) {
            switch (index) {
            case 0:
                console::drainInput();
                break;
            case 1:
                repl.manager.processMessages();
                break;
            case 2:
                repl.hypixel.processMessages();
                break;
            case 3:
                repl.stats.processMessages();
                break;
            case 4:
                running = false;
                break;
            default:
                break;
            }
            repl.manager.postDrain();
            continue;
        }
        // WAIT_TIMEOUT and failures simply loop again.
    }

    // Graceful teardown: stop the network workers, then the agent session.
    repl.hypixel.shutdown();
    repl.stats.shutdown();
    repl.manager.shutdown();
    console::stopInput();
    g_manager = nullptr;
    if (g_messageWindow != nullptr) {
        ::DestroyWindow(g_messageWindow);
        g_messageWindow = nullptr;
    }
    ::CloseHandle(g_shutdownEvent);
    return EXIT_SUCCESS;
}
