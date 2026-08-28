#include "WinOverlayManager.h"

#include <aclapi.h>
#include <tlhelp32.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace cli {
namespace {

constexpr UINT kAttachTimeoutMilliseconds = 15000;
constexpr UINT kDetachTimeoutMilliseconds = 2500;
constexpr UINT kFallbackGraceMilliseconds = 2200;
constexpr std::size_t kMaximumAgentMessageBytes = 64 * 1024;
constexpr unsigned long long kGameStateStaleAfterMilliseconds = 3500;

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
    if (::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                              static_cast<int>(text.size()),
                              wide.data(), size) != size) {
        return {};
    }
    return wide;
}

std::string wideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                              static_cast<int>(text.size()), utf8.data(),
                              needed, nullptr, nullptr) != needed) {
        return {};
    }
    return utf8;
}

std::wstring trim(std::wstring text)
{
    while (!text.empty() && iswspace(text.back()) != 0)
        text.pop_back();
    while (!text.empty() && iswspace(text.front()) != 0)
        text.erase(text.begin());
    return text;
}

std::string trim(std::string text)
{
    while (!text.empty()
           && static_cast<unsigned char>(text.back()) <= 0x20U) {
        text.pop_back();
    }
    while (!text.empty()
           && static_cast<unsigned char>(text.front()) <= 0x20U) {
        text.erase(text.begin());
    }
    return text;
}

bool parseI64(const std::string &token, long long &out)
{
    if (token.empty())
        return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool parseU64(const std::string &token, unsigned long long &out)
{
    if (token.empty())
        return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool parseDouble(const std::string &token, double &out)
{
    if (token.empty())
        return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

// The agent protocol is line-oriented; fields are separated by one or more
// ASCII spaces.
std::vector<std::string> splitFields(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (begin < line.size()) {
        while (begin < line.size() && line[begin] == ' ')
            ++begin;
        if (begin >= line.size())
            break;
        std::size_t end = line.find(' ', begin);
        if (end == std::string::npos)
            end = line.size();
        fields.push_back(line.substr(begin, end - begin));
        begin = end + 1;
    }
    return fields;
}

std::string encodeToken(const std::wstring &value)
{
    if (value.empty())
        return "-";
    const std::string utf8 = wideToUtf8(value);
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size());
    for (const unsigned char character : utf8) {
        const bool unreserved = (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '.' || character == '_'
            || character == '~';
        if (unreserved) {
            out += static_cast<char>(character);
        } else {
            out += '%';
            out += hex[character >> 4U];
            out += hex[character & 0xFU];
        }
    }
    return out;
}

std::wstring decodeToken(const std::string &token)
{
    if (token == "-")
        return {};
    std::string utf8;
    utf8.reserve(token.size());
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (token[index] == '%' && index + 2 < token.size()
            && isxdigit(static_cast<unsigned char>(token[index + 1])) != 0
            && isxdigit(static_cast<unsigned char>(token[index + 2])) != 0) {
            auto hexValue = [](const char character) -> unsigned {
                if (character >= '0' && character <= '9')
                    return static_cast<unsigned>(character - '0');
                if (character >= 'a' && character <= 'f')
                    return static_cast<unsigned>(character - 'a') + 10U;
                return static_cast<unsigned>(character - 'A') + 10U;
            };
            utf8 += static_cast<char>((hexValue(token[index + 1]) << 4U)
                                      | hexValue(token[index + 2]));
            index += 2;
        } else {
            utf8 += token[index];
        }
    }
    return utf8ToWide(utf8);
}

bool validPlayerName(const std::wstring &name)
{
    if (name.empty() || name.size() > 16)
        return false;
    return std::all_of(name.cbegin(), name.cend(), [](const wchar_t c) {
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')
            || (c >= L'0' && c <= L'9') || c == L'_';
    });
}

bool validTeamPrefix(const std::wstring &prefix)
{
    if (prefix.size() != 2 || prefix.at(0) != static_cast<wchar_t>(0x00A7))
        return false;
    const wchar_t color = prefix.at(1);
    return (color >= L'0' && color <= L'9') || (color >= L'a' && color <= L'f');
}

std::wstring normalizedRgbColor(const std::wstring &value)
{
    const std::wstring trimmed = trim(value);
    if (trimmed.size() != 7 || trimmed.at(0) != L'#')
        return {};
    bool hexadecimal = true;
    for (std::size_t index = 1; index < trimmed.size(); ++index) {
        const wchar_t character = trimmed.at(index);
        const bool digit = character >= L'0' && character <= L'9';
        const bool upper = character >= L'A' && character <= L'F';
        const bool lower = character >= L'a' && character <= L'f';
        if (!digit && !upper && !lower) {
            hexadecimal = false;
            break;
        }
    }
    if (!hexadecimal)
        return {};
    std::wstring normalized = trimmed;
    for (auto &character : normalized)
        character = towupper(character);
    return normalized;
}

uint32_t rgbFromHexColor(const std::wstring &value)
{
    const std::wstring normalized = normalizedRgbColor(value);
    if (normalized.empty())
        return 0U;
    uint32_t rgb = 0;
    for (std::size_t index = 1; index < normalized.size(); ++index) {
        const wchar_t character = normalized.at(index);
        uint32_t digit = 0;
        if (character >= L'0' && character <= L'9')
            digit = static_cast<uint32_t>(character - L'0');
        else
            digit = static_cast<uint32_t>(character - L'A') + 10U;
        rgb = (rgb << 4U) | digit;
    }
    return rgb & 0xFFFFFFU;
}

std::string randomHex32()
{
    // CryptGenRandom supplies the pipe token; the token authenticates the
    // agent handshake within a single Windows user session.
    std::array<unsigned char, 16> bytes{};
    HCRYPTPROV provider = 0;
    if (::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL,
                               CRYPT_VERIFYCONTEXT) != FALSE) {
        (void) ::CryptGenRandom(provider, static_cast<DWORD>(bytes.size()),
                                bytes.data());
        ::CryptReleaseContext(provider, 0);
    } else {
        for (auto &byte : bytes)
            byte = static_cast<unsigned char>(::GetTickCount64() >> 8U);
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (const unsigned char byte : bytes) {
        token += hex[byte >> 4U];
        token += hex[byte & 0xFU];
    }
    return token;
}

std::wstring quote(const std::wstring &argument)
{
    if (argument.empty())
        return L"\"\"";
    const bool needsQuotes = argument.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needsQuotes)
        return argument;
    std::wstring quoted = L"\"";
    for (const wchar_t character : argument) {
        if (character == L'"')
            quoted += L"\\\"";
        else
            quoted += character;
    }
    quoted += L'"';
    return quoted;
}

bool fileExists(const std::wstring &path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring windowsErrorText(DWORD error)
{
    wchar_t *buffer = nullptr;
    const DWORD size = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (size == 0 || buffer == nullptr)
        return L"Windows error " + std::to_wstring(error);
    std::wstring text(buffer, size);
    ::LocalFree(buffer);
    return trim(text);
}

bool modularRuntimeContainsAttach(const std::wstring &runtimeRoot)
{
    if (fileExists(runtimeRoot + L"\\jmods\\jdk.attach.jmod"))
        return true;

    // A bundled jlink image has no jmods directory. Its release metadata lists
    // the modules retained in lib/modules, so an arbitrary trimmed runtime is
    // not mistaken for an Attach-capable JDK image.
    const std::wstring releasePath = runtimeRoot + L"\\release";
    if (!fileExists(releasePath)
        || !fileExists(runtimeRoot + L"\\lib\\modules")) {
        return false;
    }
    const HANDLE file = ::CreateFileW(releasePath.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::string content;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (::ReadFile(file, buffer.data(),
                       static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE
            || read == 0) {
            break;
        }
        content.append(buffer.data(), read);
        if (content.size() > 65536)
            break;
    }
    ::CloseHandle(file);
    return content.find("jdk.attach") != std::string::npos;
}

// Named-pipe security descriptor equivalent to QLocalServer::UserAccessOption:
// only the current Windows user may connect.
SECURITY_ATTRIBUTES userOnlySecurityAttributes()
{
    static SECURITY_DESCRIPTOR descriptor;
    static PACL acl = nullptr;
    static bool built = false;
    if (!built) {
        built = true;
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE) {
            DWORD size = 0;
            (void) ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            if (size > 0) {
                std::vector<BYTE> buffer(size);
                auto *tokenUser =
                    reinterpret_cast<TOKEN_USER *>(buffer.data());
                if (::GetTokenInformation(token, TokenUser, tokenUser, size, &size)
                    != FALSE) {
                    EXPLICIT_ACCESSW access{};
                    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
                    access.grfAccessMode = SET_ACCESS;
                    access.grfInheritance = NO_INHERITANCE;
                    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
                    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
                    access.Trustee.ptstrName =
                        reinterpret_cast<LPWSTR>(tokenUser->User.Sid);
                    (void) ::SetEntriesInAclW(1, &access, nullptr, &acl);
                    ::InitializeSecurityDescriptor(&descriptor,
                                                   SECURITY_DESCRIPTOR_REVISION);
                    (void) ::SetSecurityDescriptorOwner(&descriptor,
                                                        tokenUser->User.Sid, FALSE);
                    (void) ::SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE);
                }
            }
            ::CloseHandle(token);
        }
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), &descriptor, FALSE};
    return attributes;
}

struct WindowSearch
{
    DWORD pid = 0;
    HWND best = nullptr;
    int score = -1;
};

std::wstring nativeWindowText(HWND window)
{
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0)
        return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(window, text.data(), length + 1);
    if (copied <= 0)
        return {};
    text.resize(static_cast<std::size_t>(copied));
    return text;
}

BOOL CALLBACK findTargetWindow(HWND window, LPARAM context)
{
    auto *search = reinterpret_cast<WindowSearch *>(context);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || ::IsWindowVisible(window) == FALSE)
        return TRUE;

    int score = ::GetWindowTextLengthW(window) > 0 ? 50 : 0;
    if (::GetWindow(window, GW_OWNER) == nullptr)
        score += 25;
    if ((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0)
        score += 15;
    if (score > search->score) {
        search->score = score;
        search->best = window;
    }
    return TRUE;
}

} // namespace

WinOverlayManager::WinOverlayManager()
{
    m_pipeStopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_menuHotkey = m_settings.menuHotkey();
    m_guiScaleIndex = m_settings.guiScaleIndex();
    m_features = m_settings.loadFeatures();
}

WinOverlayManager::~WinOverlayManager()
{
    shutdown();
}

void WinOverlayManager::shutdown()
{
    // Best-effort protocol write, then release local resources. The agent DLL
    // remains normally resident until JVM exit.
    if (m_authenticated)
        (void) writeAgentLine("DETACH");
    (void) closeSessionTransport();
    if (m_pipeStopEvent != nullptr) {
        ::CloseHandle(m_pipeStopEvent);
        m_pipeStopEvent = nullptr;
    }
}

bool WinOverlayManager::busy() const noexcept
{
    return m_state == State::Validating
        || m_state == State::StartingIpc
        || m_state == State::LaunchingAttachHelper
        || m_state == State::WaitingForAgent
        || m_state == State::Detaching;
}

void WinOverlayManager::setState(const State state)
{
    if (m_state == state)
        return;
    m_state = state;
    if (onStateChanged)
        onStateChanged();
}

void WinOverlayManager::setStatus(const std::wstring &status)
{
    if (m_status == status)
        return;
    m_status = status;
    if (onStatus)
        onStatus(status);
}

void WinOverlayManager::setRenderer(const std::wstring &renderer)
{
    if (m_renderer == renderer)
        return;
    m_renderer = renderer;
    if (onRenderer)
        onRenderer(renderer);
}

void WinOverlayManager::setError(const std::wstring &code,
                                 const std::wstring &detail)
{
    m_errorCode = code;
    m_errorDetail = detail;
    if (onError)
        onError(code, detail);
    setState(State::Error);
    setStatus(detail);
}

void WinOverlayManager::clearError()
{
    if (m_errorCode.empty() && m_errorDetail.empty())
        return;
    m_errorCode.clear();
    m_errorDetail.clear();
    if (onError)
        onError(L"", L"");
}

void WinOverlayManager::fail(const std::wstring &code, const std::wstring &detail)
{
    (void) closeSessionTransport();
    setRenderer({});
    resetGameState();
    setError(code, detail);
}

bool WinOverlayManager::writeAgentLine(const std::string &line)
{
    std::lock_guard<std::mutex> lock(m_pipeWriteMutex);
    const HANDLE pipe = m_pipeHandle.load(std::memory_order_acquire);
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr)
        return false;

    std::string framed = line;
    if (framed.empty() || framed.back() != '\n')
        framed += '\n';

    // The pipe reader thread blocks in a synchronous ReadFile on the same
    // handle; a synchronous WriteFile from this thread would serialize behind
    // it and deadlock while the agent waits for the command. The pipe is
    // created with FILE_FLAG_OVERLAPPED precisely so writes can proceed
    // concurrently through an overlapped operation with a bounded wait.
    const HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return false;
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD written = 0;
    BOOL ok = ::WriteFile(pipe, framed.data(),
                          static_cast<DWORD>(framed.size()), &written,
                          &operation);
    if (ok == FALSE && ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD wait = ::WaitForSingleObject(event, 1000);
        if (wait == WAIT_OBJECT_0) {
            ok = ::GetOverlappedResult(pipe, &operation, &written, FALSE);
        } else {
            ::CancelIoEx(pipe, &operation);
            ::WaitForSingleObject(event, INFINITE);
            ok = FALSE;
        }
    }
    ::CloseHandle(event);
    return ok != FALSE && written == framed.size();
}

void WinOverlayManager::startPipeServer(const std::wstring &pipeName)
{
    m_pipeName = pipeName;
    ::ResetEvent(m_pipeStopEvent);
    if (m_pipeThread.joinable())
        m_pipeThread.join();
    m_pipeThread = std::thread(&WinOverlayManager::pipeThreadLoop, this);
}

void WinOverlayManager::pipeThreadLoop()
{
    SECURITY_ATTRIBUTES security = userOnlySecurityAttributes();

    while (::WaitForSingleObject(m_pipeStopEvent, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = ::CreateNamedPipeW(
            m_pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 0, 0, 0, &security);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (::WaitForSingleObject(m_pipeStopEvent, 0) != WAIT_TIMEOUT)
                break;
            ::Sleep(50);
            continue;
        }
        m_pipeHandle.store(pipe, std::memory_order_release);

        const BOOL connected = ::ConnectNamedPipe(pipe, nullptr);
        if (connected == FALSE && ::GetLastError() != ERROR_PIPE_CONNECTED) {
            {
                std::lock_guard<std::mutex> lock(m_pipeWriteMutex);
                m_pipeHandle.store(INVALID_HANDLE_VALUE,
                                   std::memory_order_release);
            }
            ::CloseHandle(pipe);
            continue;
        }

        std::array<char, 512> buffer{};
        std::string pending;
        bool stopped = false;
        for (;;) {
            if (::WaitForSingleObject(m_pipeStopEvent, 0) != WAIT_TIMEOUT) {
                stopped = true;
                break;
            }
            DWORD read = 0;
            if (::ReadFile(pipe, buffer.data(),
                           static_cast<DWORD>(buffer.size()), &read,
                           nullptr) == FALSE
                || read == 0) {
                break;
            }
            pending.append(buffer.data(), read);
            if (pending.size() > kMaximumAgentMessageBytes) {
                PipeMessage message;
                message.lineTooLong = true;
                m_pipeQueue.push(std::move(message));
                pending.clear();
                break;
            }
            std::size_t newline = std::string::npos;
            while ((newline = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                if (!line.empty()) {
                    PipeMessage message;
                    message.line = std::move(line);
                    m_pipeQueue.push(std::move(message));
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_pipeWriteMutex);
            m_pipeHandle.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        }
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
        if (stopped)
            break;
        if (::WaitForSingleObject(m_pipeStopEvent, 0) == WAIT_TIMEOUT) {
            PipeMessage message;
            message.disconnected = true;
            m_pipeQueue.push(std::move(message));
        }
    }
}

void WinOverlayManager::processMessages()
{
    PipeMessage pipeMessage;
    while (m_pipeQueue.pop(pipeMessage)) {
        if (pipeMessage.lineTooLong) {
            fail(L"IPC_MESSAGE_TOO_LARGE",
                 L"The native agent exceeded the per-line IPC message limit.");
            continue;
        }
        if (pipeMessage.disconnected) {
            handleAgentDisconnected();
            continue;
        }
        handleAgentLine(pipeMessage.line);
    }

    HelperMessage helperMessage;
    while (m_helperQueue.pop(helperMessage))
        handleHelperMessage(helperMessage);
}

void WinOverlayManager::postDrain()
{
    if (m_pendingAttachPid == 0 || m_state != State::Detached || m_helperRunning)
        return;
    const uint32_t pid = m_pendingAttachPid;
    m_pendingAttachPid = 0;
    (void) attachToProcess(pid);
}

void WinOverlayManager::onTimer(const TimerId id)
{
    switch (id) {
    case TimerAttach:
        if (!m_authenticated) {
            fail(L"AGENT_HANDSHAKE_TIMEOUT",
                 L"No authenticated native-agent connection arrived within 15 seconds.");
        }
        break;
    case TimerFallbackGrace:
        if (!m_authenticated && m_state == State::WaitingForAgent
            && m_loaderKind == LoaderKind::JvmAttach
            && !m_nativeFallbackAttempted) {
            (void) startNativeLoaderFallback();
        }
        break;
    case TimerDetach:
        if (m_state == State::Detaching && !m_detachTransportComplete)
            completeDetach(true);
        break;
    case TimerTargetMonitor:
        monitorTarget();
        break;
    case TimerFreshness:
        refreshFreshness();
        break;
    case TimerFeatureStore:
        m_settings.storeFeatures(m_features);
        break;
    }
}

bool WinOverlayManager::attachToProcess(const uint32_t pid)
{
    if (pid == 0) {
        fail(L"INVALID_PID",
             L"Select a Java process before loading the native agent.");
        return false;
    }

    // Loading another process while a session is live is a queued transition:
    // finish the authenticated DETACH transaction first, then start the new
    // helper from a later main-loop iteration.
    if (m_state == State::Detaching) {
        m_pendingAttachPid = pid;
        setStatus(L"Finishing the previous detach before attaching to PID "
                  + std::to_wstring(pid) + L"...");
        return true;
    }
    if (m_state != State::Detached || m_targetPid != 0
        || m_pipeThread.joinable() || m_helperRunning) {
        m_pendingAttachPid = pid;
        beginDetach();
        return true;
    }
    (void) closeSessionTransport();
    m_pendingAttachPid = 0;

    clearError();
    setRenderer({});
    resetGameState();
    m_pipeToken.clear();
    m_agentDllPath.clear();
    m_agentOptions.clear();
    m_loaderKind = LoaderKind::None;
    m_nativeFallbackAttempted = false;
    if (cancelTimer)
        cancelTimer(TimerFallbackGrace);
    if (m_targetPid != 0 || !m_targetTitle.empty()) {
        m_targetPid = 0;
        m_targetTitle.clear();
    }
    setState(State::Validating);

    const std::wstring executablePath = targetExecutablePath(pid);
    if (executablePath.empty()) {
        fail(L"PROCESS_ACCESS_DENIED",
             L"Windows could not query the selected Java process. Run both programs at the same integrity level.");
        return false;
    }
    if (!targetArchitectureSupported(pid)) {
        fail(L"UNSUPPORTED_ARCHITECTURE",
             L"McOverlayAgent is x64 and can only be loaded into an x64 JVM.");
        return false;
    }

    const std::wstring agentDll = locateAgentDll();
    const std::wstring attachHelper = locateAttachHelper();
    const JavaRuntime java = locateJavaRuntime(executablePath);
    if (agentDll.empty()) {
        fail(L"AGENT_NOT_FOUND",
             L"McOverlayAgent.dll was not found beside the application.");
        return false;
    }
    if (attachHelper.empty()) {
        fail(L"ATTACH_HELPER_NOT_FOUND",
             L"McOverlayAttachHelper.jar was not found beside the application.");
        return false;
    }
    if (java.executable.empty()) {
        fail(L"ATTACH_JDK_NOT_FOUND",
             L"A JDK containing the jdk.attach API is required. Configure JAVA_HOME or install a current x64 JDK.");
        return false;
    }

    m_targetPid = pid;
    m_targetTitle = trim(targetWindowTitle(pid));
    if (m_targetTitle.empty())
        m_targetTitle = L"Minecraft 1.8.9 (PID " + std::to_wstring(pid) + L")";

    setState(State::StartingIpc);
    m_pipeToken = randomHex32();
    m_pipeName = L"\\\\.\\pipe\\McOverlay-" + std::to_wstring(pid)
        + L"-" + utf8ToWide(randomHex32());

    // Verify the pipe can be created before handing its name to the agent.
    {
        SECURITY_ATTRIBUTES security = userOnlySecurityAttributes();
        const HANDLE probe = ::CreateNamedPipeW(
            m_pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 0, 0, 0, &security);
        if (probe == INVALID_HANDLE_VALUE) {
            fail(L"IPC_LISTEN_FAILED", windowsErrorText(::GetLastError()));
            return false;
        }
        ::CloseHandle(probe);
    }
    startPipeServer(m_pipeName);

    m_agentOptions = "pipe=" + wideToUtf8(m_pipeName)
        + ";token=" + m_pipeToken + ";protocol=1";
    m_agentDllPath = agentDll;

    m_jvmAttachFallbackReason.clear();
    m_authenticated = false;
    m_loaderKind = LoaderKind::JvmAttach;
    setState(State::LaunchingAttachHelper);
    setStatus(L"Loading the JNI/JVMTI agent into " + m_targetTitle + L"...");

    std::wstring arguments;
    if (java.modular) {
        arguments = L"--add-modules jdk.attach -jar " + quote(attachHelper);
    } else {
        arguments = L"-cp " + quote(attachHelper + L";" + java.toolsJar)
            + L" com.mcoverlay.attach.AttachHelper";
    }
    arguments += L" " + std::to_wstring(pid) + L" " + quote(m_agentDllPath)
        + L" " + quote(utf8ToWide(m_agentOptions));

    if (!spawnHelper(java.executable, arguments, m_helperProcess,
                     m_helperReaper, m_helperQueue, m_helperKilled)) {
        fail(L"ATTACH_PROCESS_ERROR",
             L"Could not start the JVM Attach helper: "
                 + windowsErrorText(::GetLastError()));
        return false;
    }
    m_helperRunning = true;

    setState(State::WaitingForAgent);
    if (requestTimer)
        requestTimer(TimerAttach, kAttachTimeoutMilliseconds);
    if (requestTimer)
        requestTimer(TimerTargetMonitor, 1000);
    return true;
}

void WinOverlayManager::detach()
{
    m_pendingAttachPid = 0;
    beginDetach();
}

void WinOverlayManager::beginDetach()
{
    if (m_state == State::Detaching)
        return;

    const bool hasTransport = m_pipeThread.joinable() || !m_pipeName.empty();
    if (m_state == State::Detached && m_targetPid == 0
        && !hasTransport && !m_helperRunning) {
        return;
    }

    if (cancelTimer)
        cancelTimer(TimerAttach);
    m_detachTransportComplete = false;
    m_detachTimedOut = false;
    setState(State::Detaching);
    setStatus(L"Waiting for the native agent to restore its OpenGL hooks...");

    // Only an authenticated peer may receive controller commands. During an
    // incomplete attach there is no runtime whose shutdown can be confirmed,
    // so close that partial session immediately.
    if (m_authenticated && writeAgentLine("DETACH")) {
        if (requestTimer)
            requestTimer(TimerDetach, kDetachTimeoutMilliseconds);
        return;
    }
    completeDetach(false);
}

void WinOverlayManager::completeDetach(const bool timedOut)
{
    if (m_detachTransportComplete)
        return;

    m_detachTimedOut = timedOut;
    m_detachTransportComplete = true;
    (void) closeSessionTransport();

    m_targetPid = 0;
    m_targetTitle.clear();
    m_pipeToken.clear();
    setRenderer({});
    resetGameState();

    finalizeDetachedState();
}

void WinOverlayManager::finalizeDetachedState()
{
    m_detachTransportComplete = false;
    clearError();
    setState(State::Detached);
    setStatus(m_detachTimedOut
                  ? L"Native overlay detached after the agent response timed out"
                  : L"Native overlay is detached");
    m_detachTimedOut = false;
}

bool WinOverlayManager::closeSessionTransport()
{
    if (m_closingTransport)
        return !m_helperRunning;
    m_closingTransport = true;

    if (cancelTimer) {
        cancelTimer(TimerAttach);
        cancelTimer(TimerFallbackGrace);
        cancelTimer(TimerDetach);
        cancelTimer(TimerTargetMonitor);
        cancelTimer(TimerFeatureStore);
    }
    m_authenticated = false;
    m_loaderKind = LoaderKind::None;

    // Stop the pipe reader: signal the stop event and cancel its blocking
    // ReadFile, then join.
    if (m_pipeStopEvent != nullptr)
        ::SetEvent(m_pipeStopEvent);
    const HANDLE pipe = m_pipeHandle.load(std::memory_order_acquire);
    if (pipe != INVALID_HANDLE_VALUE && pipe != nullptr)
        ::CancelIoEx(pipe, nullptr);
    if (m_pipeThread.joinable())
        m_pipeThread.join();
    if (m_pipeStopEvent != nullptr)
        ::ResetEvent(m_pipeStopEvent);
    m_pipeName.clear();
    m_pipeHandle.store(INVALID_HANDLE_VALUE, std::memory_order_release);

    PipeMessage pipeMessage;
    while (m_pipeQueue.pop(pipeMessage)) {
    }

    // Kill a still-running helper, then join its reaper (the stderr pipe
    // reaches EOF as soon as the child terminates).
    const bool helperStopped = !m_helperRunning;
    if (m_helperRunning) {
        m_helperKilled.store(true);
        ::TerminateProcess(m_helperProcess, 1);
        if (m_helperReaper.joinable())
            m_helperReaper.join();
        if (m_helperProcess != nullptr) {
            ::CloseHandle(m_helperProcess);
            m_helperProcess = nullptr;
        }
        m_helperRunning = false;
    }
    HelperMessage helperMessage;
    while (m_helperQueue.pop(helperMessage)) {
    }

    m_agentDllPath.clear();
    m_agentOptions.clear();
    m_closingTransport = false;
    return helperStopped;
}

void WinOverlayManager::handleHelperMessage(const HelperMessage &message)
{
    if (!message.exited || message.killed)
        return;

    if (m_state == State::Detaching) {
        if (m_detachTransportComplete)
            finalizeDetachedState();
        return;
    }
    if (m_closingTransport || m_state == State::Detached
        || m_state == State::Error) {
        return;
    }
    // The helper process is only a launcher. Once HELLO has authenticated the
    // resident agent, its later exit status must not move an already-live
    // session back to WaitingForAgent or start a second bootstrap path.
    if (m_authenticated)
        return;

    // Some Forge 1.8.9 HotSpot builds surface a native Agent_OnAttach load as
    // AgentLoadException (helper exit 12), even though the same exact-path DLL
    // is safe to start through its explicit McOverlay_Start export.
    const bool recoverableJvmAttachFailure = message.exitCode == 10
        || message.exitCode == 12 || message.exitCode == 13;
    if (recoverableJvmAttachFailure && m_loaderKind == LoaderKind::JvmAttach
        && !m_nativeFallbackAttempted) {
        m_jvmAttachFallbackReason = trim(utf8ToWide(message.stderrText));
        setState(State::WaitingForAgent);
        setStatus(L"JVM Attach returned a compatibility error; waiting briefly for its asynchronous Agent handshake...");
        if (requestTimer)
            requestTimer(TimerFallbackGrace, kFallbackGraceMilliseconds);
        return;
    }

    if (message.exitCode != 0) {
        const bool nativeLoader = m_loaderKind == LoaderKind::NativeLoadLibrary;
        std::wstring detail = trim(utf8ToWide(message.stderrText));
        std::wstring resolved = detail.empty()
            ? (nativeLoader ? std::wstring(L"Native DLL loader")
                            : std::wstring(L"JVM Attach helper"))
                  + L" exited with code " + std::to_wstring(message.exitCode)
                  + L"."
            : detail;
        if (nativeLoader && !m_jvmAttachFallbackReason.empty()) {
            resolved = L"JVM Attach failed first: " + m_jvmAttachFallbackReason
                + L"\nNative fallback failed: " + resolved;
        }
        fail(nativeLoader ? L"NATIVE_DLL_LOAD_FAILED" : L"JVM_ATTACH_FAILED",
             resolved);
        return;
    }

    if (!m_authenticated) {
        setState(State::WaitingForAgent);
        setStatus(m_loaderKind == LoaderKind::NativeLoadLibrary
                      ? L"Windows loaded the native DLL; waiting for the authenticated agent handshake..."
                      : L"JVM accepted the DLL; waiting for the authenticated agent handshake...");
    }
}

void WinOverlayManager::handleAgentDisconnected()
{
    if (m_state == State::Detaching) {
        completeDetach(false);
        return;
    }
    // A few Forge 1.8.9 VMs complete asynchronous Agent_OnAttach and then tear
    // down that first control-pipe worker. Keep the authenticated server and
    // token alive: the bounded grace timer advances to McOverlay_Start exactly
    // once.
    if (m_state != State::Detached && m_state != State::Error
        && m_loaderKind == LoaderKind::JvmAttach
        && !m_nativeFallbackAttempted
        && targetProcessIsRunning(m_targetPid)) {
        m_authenticated = false;
        m_jvmAttachFallbackReason =
            L"The JVM Attach agent disconnected before the session stabilized.";
        setState(State::WaitingForAgent);
        setStatus(L"Forge agent pipe closed during startup; waiting briefly before native recovery...");
        if (requestTimer)
            requestTimer(TimerFallbackGrace, kFallbackGraceMilliseconds);
    } else if (m_state != State::Detached && m_state != State::Error) {
        fail(L"AGENT_DISCONNECTED",
             L"The native agent disconnected from its control pipe.");
    }
}

void WinOverlayManager::handleAgentLine(const std::string &line)
{
    const std::vector<std::string> fields = splitFields(trim(line));
    if (fields.empty())
        return;

    const std::string &type = fields.front();
    if (type == "HELLO") {
        unsigned long long reportedPid = 0;
        if (fields.size() < 4 || fields.at(1) != "1"
            || !parseU64(fields.at(2), reportedPid)
            || reportedPid != m_targetPid
            || fields.at(3) != m_pipeToken) {
            fail(L"AGENT_AUTHENTICATION_FAILED",
                 L"The native agent supplied an invalid protocol, PID, or token.");
            return;
        }

        m_authenticated = true;
        if (cancelTimer) {
            cancelTimer(TimerAttach);
            cancelTimer(TimerFallbackGrace);
        }
        setState(State::WaitingForOpenGL);
        setStatus(L"Native DLL loaded; waiting for Minecraft's first OpenGL frame...");
        sendStateSnapshot();
        if (onSessionReady)
            onSessionReady();
        return;
    }

    if (!m_authenticated)
        return;

    if (type == "HOOK_READY") {
        setRenderer(fields.size() > 1 ? utf8ToWide(fields.at(1)) : std::wstring{});
        setStatus(L"OpenGL presentation hook installed; waiting for a render context...");
    } else if (type == "RENDERER_READY") {
        setRenderer(fields.size() > 1 ? utf8ToWide(fields.at(1)) : std::wstring{});
        setState(State::Active);
        setStatus(L"Native " + (m_renderer.empty() ? L"OpenGL" : m_renderer)
                  + L" overlay is active inside Minecraft");
    } else if (type == "STATE_CHANGED") {
        if (fields.size() != 3 || (fields.at(1) != "0" && fields.at(1) != "1")
            || (fields.at(2) != "0" && fields.at(2) != "1")) {
            fail(L"AGENT_PROTOCOL_ERROR",
                 L"The native agent sent an invalid STATE_CHANGED message.");
            return;
        }
        m_overlayEnabled = fields.at(1) == "1";
        m_interactive = fields.at(2) == "1";
    } else if (type == "FEATURE_STATE_CHANGED") {
        if (fields.size() != 23)
            return;
        std::array<bool, 15> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (fields.at(index + 1) != "0" && fields.at(index + 1) != "1")
                return;
            values[index] = fields.at(index + 1) == "1";
        }
        long long defenseRadius = 0;
        long long threatRadius = 0;
        long long bedHotkey = 0;
        long long panelOpacity = 0;
        long long playerColor = 0;
        long long bedColor = 0;
        long long panelColor = 0;
        if (!parseI64(fields.at(16), defenseRadius)
            || !parseI64(fields.at(17), threatRadius)
            || !parseI64(fields.at(18), bedHotkey)
            || !parseI64(fields.at(19), panelOpacity)
            || !parseI64(fields.at(20), playerColor)
            || !parseI64(fields.at(21), bedColor)
            || !parseI64(fields.at(22), panelColor)) {
            return;
        }
        if (defenseRadius < 3 || defenseRadius > 10 || threatRadius < 3
            || threatRadius > 32 || bedHotkey < 8 || bedHotkey > 254
            || panelOpacity < 0 || panelOpacity > 100
            || playerColor < 0 || playerColor > 0xFFFFFF
            || bedColor < 0 || bedColor > 0xFFFFFF
            || panelColor < 0 || panelColor > 0xFFFFFF) {
            return;
        }

        m_features.espEnabled = values[0];
        m_features.entityEspEnabled = values[1];
        m_features.bedEspEnabled = values[2];
        m_features.espLabelsEnabled = values[3];
        m_features.hypixelPanelEnabled = values[4];
        m_features.bedThreatAlertsEnabled = values[5];
        m_features.bedDefensePanelEnabled = values[6];
        m_features.entityEspPlayersOnly = values[7];
        m_features.bedAutoRefreshEnabled = values[8];
        m_features.bedEspFilled = values[9];
        m_features.debugChatEnabled = values[10];
        m_features.showOwnBedDefenseInfo = values[11];
        m_features.showTeammateBoxes = values[12];
        m_features.bedDefenseHoldToShow = values[13];
        m_features.bedDefensePerspectiveScale = values[14];
        m_features.bedDefenseRadius = static_cast<int>(defenseRadius);
        m_features.bedThreatRadius = static_cast<int>(threatRadius);
        m_features.bedDefenseHotkey = static_cast<int>(bedHotkey);
        m_features.bedDefensePanelOpacity = static_cast<int>(panelOpacity);
        wchar_t color[16]{};
        swprintf(color, std::size(color), L"#%06llX",
                 static_cast<unsigned long long>(playerColor));
        m_features.playerEspColor = color;
        swprintf(color, std::size(color), L"#%06llX",
                 static_cast<unsigned long long>(bedColor));
        m_features.bedEspColor = color;
        swprintf(color, std::size(color), L"#%06llX",
                 static_cast<unsigned long long>(panelColor));
        m_features.bedDefensePanelColor = color;
        storeFeatureSettings();
    } else if (type == "BIND_CHANGED") {
        long long virtualKey = 0;
        if (fields.size() == 2 && parseI64(fields.at(1), virtualKey)
            && virtualKey >= 8 && virtualKey <= 254
            && m_menuHotkey != static_cast<int>(virtualKey)) {
            m_menuHotkey = static_cast<int>(virtualKey);
            m_settings.setMenuHotkey(m_menuHotkey);
            if (onMenuHotkeyChanged)
                onMenuHotkeyChanged(m_menuHotkey);
        }
    } else if (type == "GUI_SCALE_CHANGED") {
        long long index = 0;
        if (fields.size() == 2 && parseI64(fields.at(1), index)
            && index >= 0 && index <= 3
            && m_guiScaleIndex != static_cast<int>(index)) {
            m_guiScaleIndex = static_cast<int>(index);
            m_settings.setGuiScaleIndex(m_guiScaleIndex);
            if (onGuiScaleIndexChanged)
                onGuiScaleIndexChanged(m_guiScaleIndex);
        }
    } else if (type == "PLAYER_FOUND") {
        if (fields.size() != 3)
            return;
        const std::wstring playerName = decodeToken(fields.at(1));
        std::wstring teamPrefix = decodeToken(fields.at(2));
        std::transform(teamPrefix.begin(), teamPrefix.end(), teamPrefix.begin(),
                       [](const wchar_t c) { return towlower(c); });
        if (validPlayerName(playerName) && validTeamPrefix(teamPrefix)
            && onPlayerFound) {
            onPlayerFound(playerName, teamPrefix);
        }
    } else if (type == "MATCH_STATE") {
        if (fields.size() != 2
            || (fields.at(1) != "0" && fields.at(1) != "1")) {
            return;
        }
        const bool active = fields.at(1) == "1";
        if (m_matchActive != active) {
            m_matchActive = active;
            if (onMatchState)
                onMatchState(active);
        }
    } else if (type == "PLAYER_STATUS") {
        if (fields.size() != 2)
            return;
        const std::wstring name = decodeToken(fields.at(1));
        if (validPlayerName(name) && name != m_playerName) {
            m_playerName = name;
            if (onPlayerName)
                onPlayerName(name);
        }
    } else if (type == "HYPIXEL_QUERY") {
        if (fields.size() != 2)
            return;
        const std::wstring playerId = decodeToken(fields.at(1));
        if (validPlayerName(playerId) && onHypixelQuery)
            onHypixelQuery(playerId);
    } else if (type == "DETACH_COMPLETE") {
        if (fields.size() == 1 && m_state == State::Detaching)
            completeDetach(false);
    } else if (type == "GAME_STATE") {
        // Protocol v1:
        // GAME_STATE 1 seq unixMs valid hp maxHp entityId x y z
        //            loadedEntities bedCount mappingPct statePct
        if (fields.size() != 15 || fields.at(1) != "1")
            return;

        unsigned long long sequence = 0;
        long long timestamp = 0;
        double health = 0.0;
        double maxHealth = 0.0;
        long long entityId = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        long long entities = 0;
        long long beds = 0;
        const bool validToken = fields.at(4) == "0" || fields.at(4) == "1";
        const bool available = fields.at(4) == "1";
        const bool numericOk = parseU64(fields.at(2), sequence)
            && parseI64(fields.at(3), timestamp)
            && parseDouble(fields.at(5), health)
            && parseDouble(fields.at(6), maxHealth)
            && parseI64(fields.at(7), entityId)
            && parseDouble(fields.at(8), x)
            && parseDouble(fields.at(9), y)
            && parseDouble(fields.at(10), z)
            && parseI64(fields.at(11), entities)
            && parseI64(fields.at(12), beds);

        const bool finiteNumbers = std::isfinite(health)
            && std::isfinite(maxHealth) && std::isfinite(x)
            && std::isfinite(y) && std::isfinite(z);
        const bool sensibleRanges = health >= -2048.0 && health <= 1000000.0
            && maxHealth >= 0.0 && maxHealth <= 1000000.0
            && std::abs(x) <= 100000000.0 && std::abs(y) <= 100000000.0
            && std::abs(z) <= 100000000.0 && entities >= 0
            && entities <= 10000000 && beds >= 0 && beds <= 10000000;
        if (!numericOk || sequence == 0 || timestamp < 0 || !validToken
            || !finiteNumbers || !sensibleRanges) {
            return;
        }

        // Never surface stale or attacker-supplied gameplay values from a
        // valid=0 frame: every numeric placeholder must be zero.
        if (!available && (health != 0.0 || maxHealth != 0.0
                           || entityId != 0 || x != 0.0 || y != 0.0
                           || z != 0.0 || entities != 0 || beds != 0)) {
            return;
        }

        // Ignore delayed/reordered samples within one authenticated session.
        if (m_game.received && sequence <= m_game.sequence)
            return;

        m_game.received = true;
        m_game.available = available;
        m_game.stale = false;
        m_game.health = health;
        m_game.maxHealth = maxHealth;
        m_game.entityId = static_cast<int>(entityId);
        m_game.x = x;
        m_game.y = y;
        m_game.z = z;
        m_game.loadedEntities = static_cast<int>(entities);
        m_game.bedCount = static_cast<int>(beds);
        m_game.mappingProfile = decodeToken(fields.at(13));
        m_game.mappingState = decodeToken(fields.at(14));
        if (m_game.mappingState.empty()) {
            m_game.mappingState =
                available ? L"ready" : L"unavailable";
        }
        m_game.timestampMs = static_cast<unsigned long long>(timestamp);
        m_lastGameStateReceiptTick = ::GetTickCount64();
        m_game.sequence = sequence;
        if (onTelemetry)
            onTelemetry();
    } else if (type == "STATUS") {
        const std::size_t separator = line.find(' ');
        if (separator != std::string::npos)
            setStatus(utf8ToWide(line.substr(separator + 1)));
    } else if (type == "ERROR") {
        const std::wstring code = fields.size() > 1 ? utf8ToWide(fields.at(1))
                                                    : std::wstring{};
        const std::size_t firstSpace = line.find(' ');
        const std::size_t detailStart = firstSpace == std::string::npos
            ? std::string::npos
            : line.find(' ', firstSpace + 1);
        const std::wstring detail = detailStart == std::string::npos
            ? L"The native agent reported an error."
            : utf8ToWide(line.substr(detailStart + 1));
        fail(code.empty() ? L"AGENT_ERROR" : code, detail);
    }
}

void WinOverlayManager::sendStateSnapshot()
{
    if (!m_authenticated)
        return;
    (void) writeAgentLine(std::string("STATE ")
                          + (m_overlayEnabled ? "1 " : "0 ")
                          + (m_interactive ? "1" : "0"));
    sendFeatureSnapshot();
    sendBindSnapshot();
    sendGuiScaleSnapshot();
}

void WinOverlayManager::sendFeatureSnapshot()
{
    if (!m_authenticated)
        return;
    const auto boolean = [](const bool value) {
        return value ? "1 " : "0 ";
    };
    std::string line = "FEATURE_STATE ";
    line += boolean(m_features.espEnabled);
    line += boolean(m_features.entityEspEnabled);
    line += boolean(m_features.bedEspEnabled);
    line += boolean(m_features.espLabelsEnabled);
    line += boolean(m_features.hypixelPanelEnabled);
    line += boolean(m_features.bedThreatAlertsEnabled);
    line += boolean(m_features.bedDefensePanelEnabled);
    line += boolean(m_features.entityEspPlayersOnly);
    line += boolean(m_features.bedAutoRefreshEnabled);
    line += boolean(m_features.bedEspFilled);
    line += boolean(m_features.debugChatEnabled);
    line += boolean(m_features.showOwnBedDefenseInfo);
    line += boolean(m_features.showTeammateBoxes);
    line += boolean(m_features.bedDefenseHoldToShow);
    line += boolean(m_features.bedDefensePerspectiveScale);
    line += std::to_string(std::clamp(m_features.bedDefenseRadius, 3, 10)) + ' ';
    line += std::to_string(std::clamp(m_features.bedThreatRadius, 3, 32)) + ' ';
    line += std::to_string(std::clamp(m_features.bedDefenseHotkey, 8, 254)) + ' ';
    line += std::to_string(std::clamp(m_features.bedDefensePanelOpacity, 0, 100)) + ' ';
    line += std::to_string(rgbFromHexColor(m_features.playerEspColor)) + ' ';
    line += std::to_string(rgbFromHexColor(m_features.bedEspColor)) + ' ';
    line += std::to_string(rgbFromHexColor(m_features.bedDefensePanelColor));
    (void) writeAgentLine(line);
}

void WinOverlayManager::sendBindSnapshot()
{
    if (!m_authenticated)
        return;
    (void) writeAgentLine("BIND " + std::to_string(std::clamp(m_menuHotkey, 8, 254)));
}

void WinOverlayManager::sendGuiScaleSnapshot()
{
    if (!m_authenticated)
        return;
    (void) writeAgentLine("GUI_SCALE "
                          + std::to_string(std::clamp(m_guiScaleIndex, 0, 3)));
}

void WinOverlayManager::storeFeatureSettings()
{
    if (requestTimer)
        requestTimer(TimerFeatureStore, 300);
}

void WinOverlayManager::setMenuHotkey(const int virtualKey)
{
    if (virtualKey < 8 || virtualKey > 254 || m_menuHotkey == virtualKey)
        return;
    m_menuHotkey = virtualKey;
    m_settings.setMenuHotkey(m_menuHotkey);
    if (onMenuHotkeyChanged)
        onMenuHotkeyChanged(m_menuHotkey);
    sendBindSnapshot();
}

void WinOverlayManager::setGuiScaleIndex(const int index)
{
    const int bounded = std::clamp(index, 0, 3);
    if (m_guiScaleIndex == bounded)
        return;
    m_guiScaleIndex = bounded;
    m_settings.setGuiScaleIndex(m_guiScaleIndex);
    if (onGuiScaleIndexChanged)
        onGuiScaleIndexChanged(m_guiScaleIndex);
    sendGuiScaleSnapshot();
}

void WinOverlayManager::refreshBedCache()
{
    if (!m_authenticated)
        return;
    (void) writeAgentLine("BED_RESCAN");
    setStatus(L"Immediate bed-cache refresh requested");
}

void WinOverlayManager::publishHypixelResult(
    const int state, const std::wstring &uuid, const std::wstring &displayName,
    const long long wins, const long long losses, const long long finalKills,
    const long long finalDeaths, const long long bedsBroken,
    const long long bedsLost, const double winRate, const double fkdr,
    const std::wstring &status)
{
    if (!m_authenticated)
        return;
    auto formatDouble = [](const double value) {
        char buffer[40]{};
        snprintf(buffer, std::size(buffer), "%.9g",
                 std::isfinite(value) ? value : 0.0);
        return std::string(buffer);
    };
    const std::string line =
        "HYPIXEL_RESULT " + std::to_string(std::clamp(state, 0, 3)) + ' '
        + encodeToken(uuid) + ' ' + encodeToken(displayName) + ' '
        + std::to_string(wins) + ' ' + std::to_string(losses) + ' '
        + std::to_string(finalKills) + ' ' + std::to_string(finalDeaths) + ' '
        + std::to_string(bedsBroken) + ' ' + std::to_string(bedsLost) + ' '
        + formatDouble(winRate) + ' ' + formatDouble(fkdr) + ' '
        + encodeToken(status);
    (void) writeAgentLine(line);
}

void WinOverlayManager::publishPlayerStats(const std::wstring &playerName,
                                           const std::wstring &teamPrefix,
                                           const int stars,
                                           const double fkdr,
                                           const int level)
{
    if (!m_authenticated)
        return;
    std::wstring team = teamPrefix;
    std::transform(team.begin(), team.end(), team.begin(),
                   [](const wchar_t c) { return towlower(c); });
    if (!validPlayerName(playerName) || !validTeamPrefix(team) || stars < 0
        || level < 0 || !std::isfinite(fkdr) || fkdr < 0.0) {
        return;
    }
    char buffer[40]{};
    snprintf(buffer, std::size(buffer), "%.9g", fkdr);
    (void) writeAgentLine("STATS " + encodeToken(playerName) + ' '
                          + encodeToken(team) + ' ' + std::to_string(stars)
                          + ' ' + buffer + ' ' + std::to_string(level));
}

void WinOverlayManager::publishPlayerStatsError(const std::wstring &playerName,
                                                const std::wstring &reason)
{
    if (!m_authenticated)
        return;
    if (!validPlayerName(playerName))
        return;

    // Collapse whitespace and bound the reason the way the Qt controller does.
    std::wstring safe;
    bool pendingSpace = false;
    for (const wchar_t character : reason) {
        if (iswspace(character) != 0) {
            pendingSpace = !safe.empty();
            continue;
        }
        if (pendingSpace && !safe.empty()) {
            safe += L' ';
        }
        pendingSpace = false;
        safe += character;
        if (safe.size() >= 96)
            break;
    }
    if (safe.empty())
        return;
    (void) writeAgentLine("STATS_ERROR " + encodeToken(playerName) + ' '
                          + encodeToken(safe));
}

void WinOverlayManager::monitorTarget()
{
    if (m_targetPid != 0 && !targetProcessIsRunning(m_targetPid)) {
        const uint32_t exitedPid = m_targetPid;
        m_pendingAttachPid = 0;
        m_detachTimedOut = false;
        m_detachTransportComplete = false;
        setState(State::Detaching);
        completeDetach(false);
        if (m_state == State::Detached) {
            setStatus(L"Target JVM exited; native overlay detached");
        } else {
            setStatus(L"Target JVM exited; closing the attach helper...");
        }
        if (onTargetExited)
            onTargetExited(exitedPid);
    }
}

void WinOverlayManager::refreshFreshness()
{
    if (!m_game.received || m_game.stale)
        return;
    if (::GetTickCount64() - m_lastGameStateReceiptTick
        <= kGameStateStaleAfterMilliseconds) {
        return;
    }
    m_game.stale = true;
    if (onTelemetry)
        onTelemetry();
}

void WinOverlayManager::resetGameState()
{
    const bool changed = m_game.received || m_game.available || m_game.stale
        || m_game.health != 0.0 || m_game.maxHealth != 0.0
        || m_game.entityId != 0 || m_game.x != 0.0 || m_game.y != 0.0
        || m_game.z != 0.0 || m_game.loadedEntities != 0
        || m_game.bedCount != 0 || !m_game.mappingProfile.empty()
        || m_game.mappingState != L"waiting" || m_game.timestampMs != 0
        || m_game.sequence != 0;

    m_game = GameSnapshot{};
    m_lastGameStateReceiptTick = 0;

    if (!m_playerName.empty()) {
        m_playerName.clear();
        if (onPlayerName)
            onPlayerName(L"");
    }
    if (m_matchActive) {
        m_matchActive = false;
        if (onMatchState)
            onMatchState(false);
    }
    if (changed && onTelemetry)
        onTelemetry();
}

bool WinOverlayManager::spawnHelper(const std::wstring &executable,
                                    const std::wstring &arguments,
                                    HANDLE &process,
                                    std::thread &reaper,
                                    WaitQueue<HelperMessage> &queue,
                                    std::atomic<bool> &killedFlag)
{
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (::CreatePipe(&readEnd, &writeEnd, &security, 0) == FALSE)
        return false;
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;

    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = quote(executable) + L" " + arguments;
    if (::CreateProcessW(nullptr, commandLine.data(), nullptr,
                         nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                         &startup, &processInfo) == FALSE) {
        ::CloseHandle(readEnd);
        ::CloseHandle(writeEnd);
        return false;
    }
    ::CloseHandle(writeEnd);
    ::CloseHandle(processInfo.hThread);
    process = processInfo.hProcess;
    killedFlag.store(false);
    reaper = std::thread(helperReaperProc, processInfo.hProcess, readEnd,
                         &queue, &killedFlag);
    return true;
}

void WinOverlayManager::helperReaperProc(HANDLE process, HANDLE stderrPipe,
                                         WaitQueue<HelperMessage> *queue,
                                         std::atomic<bool> *killedFlag)
{
    std::string output;
    std::array<char, 512> buffer{};
    while (output.size() < 16 * 1024) {
        DWORD read = 0;
        if (::ReadFile(stderrPipe, buffer.data(),
                       static_cast<DWORD>(buffer.size()), &read,
                       nullptr) == FALSE
            || read == 0) {
            break;
        }
        output.append(buffer.data(), read);
    }
    ::CloseHandle(stderrPipe);

    ::WaitForSingleObject(process, INFINITE);
    DWORD exitCode = 0;
    (void) ::GetExitCodeProcess(process, &exitCode);
    ::CloseHandle(process);

    HelperMessage message;
    message.exited = true;
    message.exitCode = exitCode;
    message.killed = killedFlag->load();
    message.stderrText = std::move(output);
    queue->push(std::move(message));
}

bool WinOverlayManager::startNativeLoaderFallback()
{
    m_nativeFallbackAttempted = true;
    if (cancelTimer)
        cancelTimer(TimerFallbackGrace);
    if (!targetHasLoadedModule(m_targetPid, L"jvm.dll")
        || trim(targetWindowTitle(m_targetPid)).empty()) {
        fail(L"NATIVE_LOADER_TARGET_REJECTED",
             L"JVM Attach is unavailable, but the selected process does not expose both "
             L"a loaded jvm.dll and a visible game window. The DLL was not loaded.");
        return false;
    }
    const std::wstring nativeLoader = locateNativeLoader();
    if (nativeLoader.empty()) {
        fail(L"NATIVE_LOADER_NOT_FOUND",
             L"Runtime JVM Attach is unavailable, and McOverlayNativeLoader.exe was not found beside the application.");
        return false;
    }
    if (m_agentDllPath.empty() || m_agentOptions.empty() || m_targetPid == 0) {
        fail(L"NATIVE_LOADER_INVALID_SESSION",
             L"The native loader session was incomplete; select the process and try again.");
        return false;
    }

    m_loaderKind = LoaderKind::NativeLoadLibrary;
    setState(State::LaunchingAttachHelper);
    setStatus(L"JVM Attach did not complete; retrying through the visible native DLL export...");
    const std::wstring arguments = std::to_wstring(m_targetPid) + L" "
        + quote(m_agentDllPath) + L" " + quote(utf8ToWide(m_agentOptions));
    if (!spawnHelper(nativeLoader, arguments, m_helperProcess, m_helperReaper,
                     m_helperQueue, m_helperKilled)) {
        fail(L"NATIVE_LOADER_PROCESS_ERROR",
             L"Could not start McOverlayNativeLoader.exe: "
                 + windowsErrorText(::GetLastError()));
        return false;
    }
    m_helperRunning = true;

    // Give the fallback a full handshake window instead of consuming the
    // remainder of the failed Attach attempt's timer.
    if (requestTimer)
        requestTimer(TimerAttach, kAttachTimeoutMilliseconds);
    setState(State::WaitingForAgent);
    return true;
}

std::wstring WinOverlayManager::executableDir()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(),
                                              static_cast<DWORD>(path.size()));
    if (length == 0)
        return {};
    std::wstring directory(path.data(), length);
    const std::size_t separator = directory.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
        directory.resize(separator);
    return directory;
}

std::wstring WinOverlayManager::locateAgentDll()
{
    const std::wstring directory = executableDir();
    const std::vector<std::wstring> candidates = {
        directory + L"\\agent\\McOverlayAgent.dll",
        directory + L"\\McOverlayAgent.dll",
        directory + L"\\..\\agent\\McOverlayAgent.dll"};
    for (const auto &candidate : candidates) {
        if (fileExists(candidate))
            return candidate;
    }
    return {};
}

std::wstring WinOverlayManager::locateAttachHelper()
{
    const std::wstring directory = executableDir();
    const std::vector<std::wstring> candidates = {
        directory + L"\\tools\\McOverlayAttachHelper.jar",
        directory + L"\\McOverlayAttachHelper.jar",
        directory + L"\\..\\attach-helper\\McOverlayAttachHelper.jar"};
    for (const auto &candidate : candidates) {
        if (fileExists(candidate))
            return candidate;
    }
    return {};
}

std::wstring WinOverlayManager::locateNativeLoader()
{
    const std::wstring directory = executableDir();
    const std::vector<std::wstring> candidates = {
        directory + L"\\tools\\McOverlayNativeLoader.exe",
        directory + L"\\McOverlayNativeLoader.exe",
        directory + L"\\..\\native-loader\\McOverlayNativeLoader.exe"};
    for (const auto &candidate : candidates) {
        if (fileExists(candidate))
            return candidate;
    }
    return {};
}

WinOverlayManager::JavaRuntime WinOverlayManager::locateJavaRuntime(
    const std::wstring &targetExecutable)
{
    std::vector<std::wstring> candidates;
    candidates.push_back(executableDir() + L"\\runtime\\bin\\java.exe");

    const wchar_t *javaHome = _wgetenv(L"JAVA_HOME");
    if (javaHome != nullptr && *javaHome != L'\0')
        candidates.push_back(std::wstring(javaHome) + L"\\bin\\java.exe");

    std::array<wchar_t, 32768> found{};
    const DWORD foundLength = ::SearchPathW(nullptr, L"java.exe", L".exe",
                                            static_cast<DWORD>(found.size()),
                                            found.data(), nullptr);
    if (foundLength > 0 && foundLength < found.size())
        candidates.push_back(std::wstring(found.data(), foundLength));

    const std::size_t separator = targetExecutable.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
        candidates.push_back(targetExecutable.substr(0, separator)
                             + L"\\java.exe");

    for (const std::wstring &candidate : candidates) {
        if (!fileExists(candidate))
            continue;

        // java.exe lives in <jdk>/bin; the runtime root is two levels up.
        const std::size_t binSeparator = candidate.find_last_of(L"\\/");
        if (binSeparator == std::wstring::npos)
            continue;
        const std::wstring runtimeRoot =
            candidate.substr(0, binSeparator) + L"\\..";

        const std::wstring toolsJar = runtimeRoot + L"\\lib\\tools.jar";
        if (modularRuntimeContainsAttach(runtimeRoot))
            return {candidate, {}, true};
        if (fileExists(toolsJar))
            return {candidate, toolsJar, false};
    }
    return {};
}

std::wstring WinOverlayManager::targetExecutablePath(const uint32_t pid)
{
    const ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, pid));
    if (!process.valid())
        return {};

    std::array<wchar_t, 32768> path{};
    DWORD length = static_cast<DWORD>(path.size());
    if (::QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)
        == FALSE) {
        return {};
    }
    return std::wstring(path.data(), length);
}

std::wstring WinOverlayManager::targetWindowTitle(const uint32_t pid)
{
    WindowSearch search{pid, nullptr, -1};
    ::EnumWindows(findTargetWindow, reinterpret_cast<LPARAM>(&search));
    return nativeWindowText(search.best);
}

bool WinOverlayManager::targetArchitectureSupported(const uint32_t pid)
{
    const ScopedHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, pid));
    if (!process.valid())
        return false;

    using IsWow64Process2Function = BOOL(WINAPI *)(HANDLE, USHORT *, USHORT *);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Function>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"),
                         "IsWow64Process2"));
    if (isWow64Process2 != nullptr) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (isWow64Process2(process.get(), &processMachine, &nativeMachine)
            == FALSE) {
            return false;
        }
        return processMachine == IMAGE_FILE_MACHINE_UNKNOWN
            && nativeMachine == IMAGE_FILE_MACHINE_AMD64;
    }

    BOOL wow64 = FALSE;
    return ::IsWow64Process(process.get(), &wow64) != FALSE && wow64 == FALSE
        && sizeof(void *) == 8;
}

bool WinOverlayManager::targetProcessIsRunning(const uint32_t pid)
{
    const ScopedHandle process(::OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (process.valid())
        return ::WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT;

    // A transient access-denied result must not be interpreted as process
    // death. Toolhelp still lets us distinguish a live PID from an exited one.
    const ScopedHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid())
        return true;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot.get(), &entry) == FALSE)
        return true;
    do {
        if (entry.th32ProcessID == pid)
            return true;
    } while (::Process32NextW(snapshot.get(), &entry) != FALSE);
    return false;
}

bool WinOverlayManager::targetHasLoadedModule(const uint32_t pid,
                                              const wchar_t *moduleName)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const HANDLE rawSnapshot = ::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (rawSnapshot == INVALID_HANDLE_VALUE) {
            if (::GetLastError() == ERROR_BAD_LENGTH)
                continue;
            return false;
        }

        const ScopedHandle snapshot(rawSnapshot);
        MODULEENTRY32W module{};
        module.dwSize = sizeof(module);
        if (::Module32FirstW(snapshot.get(), &module) == FALSE)
            return false;
        do {
            if (_wcsicmp(module.szModule, moduleName) == 0)
                return true;
        } while (::Module32NextW(snapshot.get(), &module) != FALSE);
        return false;
    }
    return false;
}

} // namespace cli
