#include "WinProcessScanner.h"

#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <utility>

namespace cli {
namespace {

class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
    ~ScopedHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(m_handle);
    }

    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_handle = nullptr;
};

struct WindowCandidate
{
    HWND handle = nullptr;
    std::wstring title;
    int score = -1;
};

struct WindowMapContext
{
    std::vector<std::pair<DWORD, WindowCandidate>> windows;
};

std::wstring windowText(HWND window)
{
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0)
        return {};
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(window, buffer.data(), length + 1);
    if (copied <= 0)
        return {};
    buffer.resize(static_cast<std::size_t>(copied));
    return buffer;
}

BOOL CALLBACK collectTopLevelWindow(HWND window, LPARAM context)
{
    if (::IsWindowVisible(window) == FALSE)
        return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == 0)
        return TRUE;

    const LONG_PTR extendedStyle = ::GetWindowLongPtrW(window, GWL_EXSTYLE);
    std::wstring title = windowText(window);
    while (!title.empty() && iswspace(title.back()) != 0)
        title.pop_back();
    while (!title.empty() && iswspace(title.front()) != 0)
        title.erase(title.begin());

    // A visible, unowned, titled application window is usually the actual GLFW
    // or LWJGL Minecraft surface; the score still permits launchers.
    int score = 10;
    if (!title.empty())
        score += 50;
    if (::GetWindow(window, GW_OWNER) == nullptr)
        score += 25;
    if ((extendedStyle & WS_EX_TOOLWINDOW) == 0)
        score += 15;
    if (::IsIconic(window) != FALSE)
        score -= 5;

    auto *windows = reinterpret_cast<WindowMapContext *>(context);
    auto existing = std::find_if(windows->windows.begin(), windows->windows.end(),
                                 [pid](const auto &entry) {
                                     return entry.first == pid;
                                 });
    if (existing == windows->windows.end()) {
        windows->windows.push_back({pid, WindowCandidate{window, std::move(title), score}});
    } else if (score > existing->second.score) {
        existing->second = WindowCandidate{window, std::move(title), score};
    }

    return TRUE;
}

std::wstring executablePath(HANDLE process)
{
    if (process == nullptr)
        return {};
    std::array<wchar_t, 32768> path{};
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (::QueryFullProcessImageNameW(process, 0, path.data(), &pathLength) == FALSE)
        return {};
    return std::wstring(path.data(), pathLength);
}

uint64_t workingSetBytes(HANDLE process)
{
    if (process == nullptr)
        return 0;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(process,
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                               sizeof(counters)) == FALSE) {
        return 0;
    }
    return counters.WorkingSetSize;
}

std::wstring formatBytes(const uint64_t bytes)
{
    if (bytes == 0)
        return L"Unavailable";

    constexpr double kibibyte = 1024.0;
    constexpr double mebibyte = kibibyte * 1024.0;
    constexpr double gibibyte = mebibyte * 1024.0;

    wchar_t buffer[64]{};
    if (bytes >= static_cast<uint64_t>(gibibyte)) {
        swprintf(buffer, std::size(buffer), L"%.2f GiB", bytes / gibibyte);
    } else {
        swprintf(buffer, std::size(buffer), L"%.0f MiB", bytes / mebibyte);
    }
    return buffer;
}

} // namespace

std::vector<JavaProcess> scanJavaProcesses()
{
    std::vector<JavaProcess> processes;

    WindowMapContext windows;
    ::EnumWindows(collectTopLevelWindow, reinterpret_cast<LPARAM>(&windows));

    const ScopedHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return processes;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot.get(), &entry) == FALSE)
        return processes;

    do {
        const bool isJava = _wcsicmp(entry.szExeFile, L"java.exe") == 0
                         || _wcsicmp(entry.szExeFile, L"javaw.exe") == 0;
        if (!isJava)
            continue;

        // Start with full query access for memory counters. Restricted JVMs
        // may reject it, so fall back to limited query access and still
        // report PID/path.
        ScopedHandle process(::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                           FALSE, entry.th32ProcessID));
        ScopedHandle limitedProcess(process.valid()
            ? nullptr
            : ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE, entry.th32ProcessID));
        const HANDLE queryHandle = process.valid() ? process.get() : limitedProcess.get();

        JavaProcess info;
        info.pid = entry.th32ProcessID;
        info.executableName = entry.szExeFile;
        info.executablePath = executablePath(queryHandle);
        if (info.executablePath.empty())
            info.executablePath = info.executableName;
        info.memoryBytes = process.valid() ? workingSetBytes(process.get()) : 0;
        info.memoryText = formatBytes(info.memoryBytes);

        const auto window = std::find_if(windows.windows.cbegin(), windows.windows.cend(),
                                         [&entry](const auto &candidate) {
                                             return candidate.first == entry.th32ProcessID;
                                         });
        if (window != windows.windows.cend()) {
            info.windowHandle = window->second.handle;
            info.windowTitle = window->second.title;
        }
        if (info.windowTitle.empty()) {
            wchar_t fallback[64]{};
            swprintf(fallback, std::size(fallback), L"Java process %lu", entry.th32ProcessID);
            info.windowTitle = fallback;
        }

        processes.push_back(std::move(info));
    } while (::Process32NextW(snapshot.get(), &entry) != FALSE);

    std::sort(processes.begin(), processes.end(),
              [](const JavaProcess &left, const JavaProcess &right) {
                  const bool leftHasWindow = left.windowHandle != nullptr;
                  const bool rightHasWindow = right.windowHandle != nullptr;
                  if (leftHasWindow != rightHasWindow)
                      return leftHasWindow > rightHasWindow;
                  const int order = ::CompareStringOrdinal(left.windowTitle.c_str(), -1,
                                                           right.windowTitle.c_str(), -1,
                                                           TRUE);
                  if (order == CSTR_LESS_THAN)
                      return true;
                  if (order == CSTR_GREATER_THAN)
                      return false;
                  return left.pid < right.pid;
              });

    return processes;
}

} // namespace cli
