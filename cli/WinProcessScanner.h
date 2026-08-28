#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cli {

struct JavaProcess
{
    uint32_t pid = 0;
    std::wstring executableName;
    std::wstring executablePath;
    std::wstring windowTitle;
    uint64_t memoryBytes = 0;
    std::wstring memoryText;
    HWND windowHandle = nullptr;
};

// Enumerates java.exe/javaw.exe processes, resolves executable path,
// working-set memory, and the best visible window per process. Returns the
// list sorted by window presence, then title, then PID.
std::vector<JavaProcess> scanJavaProcesses();

} // namespace cli
