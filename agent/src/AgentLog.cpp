#include "AgentLog.h"

#include <windows.h>

#include <string>

namespace mcoverlay::log {
namespace {

void write(const char* level, const std::string_view message) noexcept
{
    try {
        std::string line;
        line.reserve(message.size() + 40U);
        line.append("[McOverlayAgent][");
        line.append(level);
        line.append("] ");
        line.append(message);
        line.push_back('\n');
        ::OutputDebugStringA(line.c_str());
    } catch (...) {
        ::OutputDebugStringA("[McOverlayAgent][ERROR] Logging failed.\n");
    }
}

} // namespace

void info(const std::string_view message) noexcept
{
    write("INFO", message);
}

void error(const std::string_view message) noexcept
{
    write("ERROR", message);
}

} // namespace mcoverlay::log

