#include "ConsoleIo.h"

#include <string>
#include <thread>

namespace cli::console {
namespace {

bool g_consoleOutput = false;
std::thread g_inputThread;
WaitQueue<std::optional<std::wstring>> g_lines;
std::function<void(const std::optional<std::wstring> &)> g_onLine;

HANDLE stdoutHandle()
{
    return ::GetStdHandle(STD_OUTPUT_HANDLE);
}

void writeOut(const std::wstring &text)
{
    if (g_consoleOutput) {
        DWORD written = 0;
        (void) ::WriteConsoleW(stdoutHandle(), text.data(),
                               static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    // Redirected output: emit UTF-8 bytes so pipes and log files stay clean.
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
    (void) ::WriteFile(stdoutHandle(), utf8.data(),
                       static_cast<DWORD>(utf8.size()), &written, nullptr);
}

std::wstring utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    (void) ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                 static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

void inputThreadProc()
{
    const HANDLE standardInput = ::GetStdHandle(STD_INPUT_HANDLE);
    char buffer[1024];
    std::string pending;
    for (;;) {
        DWORD bytesRead = 0;
        if (::ReadFile(standardInput, buffer, sizeof(buffer), &bytesRead, nullptr) == FALSE
            || bytesRead == 0) {
            g_lines.push(std::nullopt); // EOF
            return;
        }
        pending.append(buffer, bytesRead);
        std::size_t newline = std::string::npos;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            // Tolerate a UTF-8 byte-order mark that some shells prepend when
            // piping into a native executable.
            if (line.size() >= 3
                && static_cast<unsigned char>(line[0]) == 0xEFU
                && static_cast<unsigned char>(line[1]) == 0xBBU
                && static_cast<unsigned char>(line[2]) == 0xBFU) {
                line.erase(0, 3);
            }
            g_lines.push(utf8ToWide(line));
        }
    }
}

} // namespace

void initialize()
{
    DWORD mode = 0;
    g_consoleOutput = ::GetConsoleMode(stdoutHandle(), &mode) != FALSE;
    // Console input in cooked mode transcodes through the input codepage;
    // align it with the UTF-8 bytes the reader thread decodes.
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleOutputCP(CP_UTF8);
}

void print(const std::wstring &line)
{
    writeOut(line + L"\r\n");
}

void printRaw(const std::wstring &text)
{
    writeOut(text);
}

void prompt()
{
    writeOut(L"McLite> ");
}

void startInput(std::function<void(const std::optional<std::wstring> &)> onLine)
{
    if (g_inputThread.joinable())
        return;
    g_onLine = std::move(onLine);
    g_inputThread = std::thread(inputThreadProc);
}

void stopInput()
{
    if (g_inputThread.joinable())
        g_inputThread.detach();
}

void drainInput()
{
    std::optional<std::wstring> line;
    while (g_lines.pop(line))
        g_onLine(line);
}

HANDLE inputEvent()
{
    return g_lines.event();
}

} // namespace cli::console
