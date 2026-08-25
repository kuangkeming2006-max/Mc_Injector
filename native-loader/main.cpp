// McOverlayNativeLoader
//
// A deliberately conventional and visible Windows DLL loader. It uses only
// VirtualAllocEx/WriteProcessMemory/CreateRemoteThread with LoadLibraryW. The
// DLL remains present in the target's normal module list; there is no manual
// mapping, image unlinking, handle hiding, or anti-cheat bypass behavior.
//
// Command line:
//   McOverlayNativeLoader.exe <pid> <absolute-or-relative-dll> <utf8-options>
//
// Stable process exit codes (also printed with a symbolic name on stderr):
//   0  success                 2  invalid command line
//   3  invalid PID             4  invalid/inaccessible DLL path
//   5  invalid options         6  non-x64 target or DLL
//  10  OpenProcess failed     11  local LoadLibraryW resolution failed
//  12  loader owner missing   13  remote path allocation failed
//  14  remote path write      15  LoadLibrary thread creation
//  16  LoadLibrary timeout    17  LoadLibrary wait/query failure
//  18  LoadLibrary failed     19  loaded DLL not enumerable
//  20  local image mapping    21  McOverlay_Start not exported
//  22  invalid export RVA     23  remote options allocation failed
//  24  remote options write   25  start thread creation failed
//  26  start call timeout     27  start wait/query failure
//  28  McOverlay_Start returned a failure value
//  29  reserved (older builds rejected a resident agent here)

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kRemoteCallTimeoutMs = 15'000U;
constexpr std::size_t kMaximumOptionsBytes = 64U * 1024U;
constexpr unsigned kSnapshotAttempts = 12U;

enum class ExitCode : int {
    Success = 0,
    InvalidCommandLine = 2,
    InvalidPid = 3,
    InvalidDllPath = 4,
    InvalidOptions = 5,
    ArchitectureMismatch = 6,
    OpenProcessFailed = 10,
    ResolveLoadLibraryFailed = 11,
    LoaderOwnerMissing = 12,
    AllocatePathFailed = 13,
    WritePathFailed = 14,
    CreateLoadThreadFailed = 15,
    LoadThreadTimeout = 16,
    LoadThreadWaitFailed = 17,
    LoadLibraryFailed = 18,
    AgentModuleMissing = 19,
    MapAgentLocallyFailed = 20,
    StartExportMissing = 21,
    InvalidExportRva = 22,
    AllocateOptionsFailed = 23,
    WriteOptionsFailed = 24,
    CreateStartThreadFailed = 25,
    StartThreadTimeout = 26,
    StartThreadWaitFailed = 27,
    StartExportFailed = 28,
    ReservedResidentAgent = 29,
};

const wchar_t* exitCodeName(const ExitCode code) noexcept
{
    switch (code) {
    case ExitCode::Success: return L"SUCCESS";
    case ExitCode::InvalidCommandLine: return L"INVALID_COMMAND_LINE";
    case ExitCode::InvalidPid: return L"INVALID_PID";
    case ExitCode::InvalidDllPath: return L"INVALID_DLL_PATH";
    case ExitCode::InvalidOptions: return L"INVALID_OPTIONS";
    case ExitCode::ArchitectureMismatch: return L"ARCHITECTURE_MISMATCH";
    case ExitCode::OpenProcessFailed: return L"OPEN_PROCESS_FAILED";
    case ExitCode::ResolveLoadLibraryFailed: return L"RESOLVE_LOAD_LIBRARY_FAILED";
    case ExitCode::LoaderOwnerMissing: return L"LOADER_OWNER_MISSING";
    case ExitCode::AllocatePathFailed: return L"ALLOCATE_PATH_FAILED";
    case ExitCode::WritePathFailed: return L"WRITE_PATH_FAILED";
    case ExitCode::CreateLoadThreadFailed: return L"CREATE_LOAD_THREAD_FAILED";
    case ExitCode::LoadThreadTimeout: return L"LOAD_THREAD_TIMEOUT";
    case ExitCode::LoadThreadWaitFailed: return L"LOAD_THREAD_WAIT_FAILED";
    case ExitCode::LoadLibraryFailed: return L"LOAD_LIBRARY_FAILED";
    case ExitCode::AgentModuleMissing: return L"AGENT_MODULE_MISSING";
    case ExitCode::MapAgentLocallyFailed: return L"MAP_AGENT_LOCALLY_FAILED";
    case ExitCode::StartExportMissing: return L"START_EXPORT_MISSING";
    case ExitCode::InvalidExportRva: return L"INVALID_EXPORT_RVA";
    case ExitCode::AllocateOptionsFailed: return L"ALLOCATE_OPTIONS_FAILED";
    case ExitCode::WriteOptionsFailed: return L"WRITE_OPTIONS_FAILED";
    case ExitCode::CreateStartThreadFailed: return L"CREATE_START_THREAD_FAILED";
    case ExitCode::StartThreadTimeout: return L"START_THREAD_TIMEOUT";
    case ExitCode::StartThreadWaitFailed: return L"START_THREAD_WAIT_FAILED";
    case ExitCode::StartExportFailed: return L"START_EXPORT_FAILED";
    case ExitCode::ReservedResidentAgent: return L"RESERVED_29";
    }
    return L"UNKNOWN";
}

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : m_value(std::exchange(other.m_value, nullptr))
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_value = std::exchange(other.m_value, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_value; }
    [[nodiscard]] bool valid() const noexcept
    {
        return m_value != nullptr && m_value != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE value = nullptr) noexcept
    {
        if (valid()) {
            ::CloseHandle(m_value);
        }
        m_value = value;
    }

private:
    HANDLE m_value = nullptr;
};

class LocalModule final {
public:
    LocalModule() noexcept = default;
    explicit LocalModule(HMODULE value) noexcept : m_value(value) {}
    ~LocalModule()
    {
        if (m_value != nullptr) {
            ::FreeLibrary(m_value);
        }
    }

    LocalModule(const LocalModule&) = delete;
    LocalModule& operator=(const LocalModule&) = delete;

    [[nodiscard]] HMODULE get() const noexcept { return m_value; }

private:
    HMODULE m_value = nullptr;
};

class RemoteAllocation final {
public:
    RemoteAllocation(HANDLE process, const SIZE_T size) noexcept
        : m_process(process),
          m_address(::VirtualAllocEx(process, nullptr, size,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
    {
    }

    ~RemoteAllocation()
    {
        if (m_address != nullptr) {
            ::VirtualFreeEx(m_process, m_address, 0U, MEM_RELEASE);
        }
    }

    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;

    [[nodiscard]] LPVOID get() const noexcept { return m_address; }

    // If a remote call times out, its parameter must remain alive. Leaking a
    // small buffer into that process is safer than freeing memory that an
    // unobserved live thread may still access; we never TerminateThread.
    void abandon() noexcept { m_address = nullptr; }

private:
    HANDLE m_process = nullptr;
    LPVOID m_address = nullptr;
};

struct ModuleRecord {
    std::uintptr_t base = 0U;
    std::size_t size = 0U;
    std::wstring moduleName;
    std::wstring path;
};

struct RemoteCallResult {
    enum class Stage {
        CreateThread,
        WaitForThread,
        QueryExitCode,
        Complete,
    } stage = Stage::CreateThread;

    bool threadCreated = false;
    bool completionObserved = false;
    bool completed = false;
    bool timedOut = false;
    DWORD exitValue = 0U;
    DWORD error = ERROR_SUCCESS;
};

std::wstring systemErrorText(const DWORD error)
{
    if (error == ERROR_SUCCESS) {
        return {};
    }

    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0U, reinterpret_cast<wchar_t*>(&buffer), 0U, nullptr);
    if (length == 0U || buffer == nullptr) {
        return L"Win32 error " + std::to_wstring(error);
    }

    std::wstring message(buffer, length);
    ::LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            std::iswspace(message.back()) != 0)) {
        message.pop_back();
    }
    return message;
}

int fail(const ExitCode code,
         const std::wstring_view detail,
         const DWORD error = ERROR_SUCCESS)
{
    std::wcerr << L"NATIVE_LOADER_ERROR " << static_cast<int>(code) << L' '
               << exitCodeName(code) << L": " << detail;
    if (error != ERROR_SUCCESS) {
        std::wcerr << L" (Win32 " << error << L": "
                   << systemErrorText(error) << L')';
    }
    std::wcerr << L'\n';
    return static_cast<int>(code);
}

std::optional<DWORD> parsePid(const std::wstring_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0U;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        value = value * 10U + static_cast<unsigned>(character - L'0');
        if (value > std::numeric_limits<DWORD>::max()) {
            return std::nullopt;
        }
    }
    if (value == 0U) {
        return std::nullopt;
    }
    return static_cast<DWORD>(value);
}

std::wstring stripExtendedPrefix(std::wstring path)
{
    constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view extendedPrefix = L"\\\\?\\";
    if (path.size() >= uncPrefix.size() &&
        ::CompareStringOrdinal(path.data(), static_cast<int>(uncPrefix.size()),
                               uncPrefix.data(), static_cast<int>(uncPrefix.size()),
                               TRUE) == CSTR_EQUAL) {
        path = L"\\\\" + path.substr(uncPrefix.size());
    } else if (path.size() >= extendedPrefix.size() &&
               path.compare(0U, extendedPrefix.size(), extendedPrefix) == 0) {
        path.erase(0U, extendedPrefix.size());
    }
    return path;
}

std::optional<std::wstring> fullPath(const std::wstring_view input)
{
    if (input.empty() || input.size() > 32'000U) {
        return std::nullopt;
    }

    const std::wstring source(input);
    const DWORD required = ::GetFullPathNameW(source.c_str(), 0U, nullptr, nullptr);
    if (required == 0U) {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
    const DWORD written = ::GetFullPathNameW(source.c_str(),
                                             static_cast<DWORD>(buffer.size()),
                                             buffer.data(), nullptr);
    if (written == 0U || written >= buffer.size()) {
        return std::nullopt;
    }
    return stripExtendedPrefix(std::wstring(buffer.data(), written));
}

std::optional<std::wstring> canonicalDllPath(const std::wstring_view input,
                                             DWORD& failureError)
{
    failureError = ERROR_SUCCESS;
    const auto absolute = fullPath(input);
    if (!absolute) {
        failureError = ::GetLastError();
        return std::nullopt;
    }

    const DWORD attributes = ::GetFileAttributesW(absolute->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        failureError = ::GetLastError();
        if (failureError == ERROR_SUCCESS) {
            failureError = ERROR_FILE_NOT_FOUND;
        }
        return std::nullopt;
    }

    UniqueHandle file(::CreateFileW(absolute->c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
    if (!file.valid()) {
        failureError = ::GetLastError();
        return std::nullopt;
    }

    DWORD required = ::GetFinalPathNameByHandleW(file.get(), nullptr, 0U,
                                                  FILE_NAME_NORMALIZED |
                                                      VOLUME_NAME_DOS);
    if (required == 0U) {
        // Some filesystems do not implement final-name lookup. The fully
        // qualified DOS path is still safe and deterministic for LoadLibraryW.
        return absolute;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
    const DWORD written = ::GetFinalPathNameByHandleW(
        file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0U || written >= buffer.size()) {
        failureError = ::GetLastError();
        return std::nullopt;
    }
    return stripExtendedPrefix(std::wstring(buffer.data(), written));
}

bool isAmd64PeImage(const std::wstring& path, DWORD& failureError)
{
    failureError = ERROR_SUCCESS;
    UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr));
    if (!file.valid()) {
        failureError = ::GetLastError();
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    DWORD bytesRead = 0U;
    if (!::ReadFile(file.get(), &dos, sizeof(dos), &bytesRead, nullptr) ||
        bytesRead != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0) {
        failureError = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    if (!::SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) {
        failureError = ::GetLastError();
        return false;
    }

    IMAGE_NT_HEADERS64 nt{};
    if (!::ReadFile(file.get(), &nt, sizeof(nt), &bytesRead, nullptr) ||
        bytesRead != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        failureError = ERROR_BAD_EXE_FORMAT;
        return false;
    }
    return true;
}

std::wstring baseName(const std::wstring_view path)
{
    const std::size_t separator = path.find_last_of(L"\\/");
    return std::wstring(separator == std::wstring_view::npos
                            ? path
                            : path.substr(separator + 1U));
}

bool equalInsensitive(const std::wstring_view left,
                      const std::wstring_view right) noexcept
{
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                  right.data(), static_cast<int>(right.size()),
                                  TRUE) == CSTR_EQUAL;
}

std::vector<ModuleRecord> enumerateModules(const DWORD pid, DWORD& failureError)
{
    failureError = ERROR_SUCCESS;
    UniqueHandle snapshot;
    for (unsigned attempt = 0U; attempt < kSnapshotAttempts; ++attempt) {
        snapshot.reset(::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
                                                     TH32CS_SNAPMODULE32,
                                                 pid));
        if (snapshot.valid()) {
            break;
        }
        failureError = ::GetLastError();
        if (failureError != ERROR_BAD_LENGTH) {
            return {};
        }
        ::Sleep(10U);
    }
    if (!snapshot.valid()) {
        return {};
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Module32FirstW(snapshot.get(), &entry)) {
        failureError = ::GetLastError();
        return {};
    }

    std::vector<ModuleRecord> result;
    do {
        ModuleRecord module;
        module.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        module.size = static_cast<std::size_t>(entry.modBaseSize);
        module.moduleName = entry.szModule;
        module.path = entry.szExePath;
        result.push_back(std::move(module));
        entry.dwSize = sizeof(entry);
    } while (::Module32NextW(snapshot.get(), &entry));

    const DWORD iterationError = ::GetLastError();
    if (iterationError != ERROR_NO_MORE_FILES) {
        failureError = iterationError;
        return {};
    }
    return result;
}

std::optional<ModuleRecord> findModuleByName(const DWORD pid,
                                             const std::wstring_view name,
                                             DWORD& failureError)
{
    const auto modules = enumerateModules(pid, failureError);
    if (failureError != ERROR_SUCCESS) {
        return std::nullopt;
    }
    for (const ModuleRecord& module : modules) {
        if (equalInsensitive(module.moduleName, name) ||
            equalInsensitive(baseName(module.path), name)) {
            return module;
        }
    }
    failureError = ERROR_MOD_NOT_FOUND;
    return std::nullopt;
}

std::optional<ModuleRecord> findModuleByPath(const DWORD pid,
                                             const std::wstring& canonicalPath,
                                             DWORD& failureError)
{
    const auto modules = enumerateModules(pid, failureError);
    if (failureError != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const std::wstring wantedName = baseName(canonicalPath);
    for (const ModuleRecord& module : modules) {
        if (const auto normalized = fullPath(module.path);
            normalized && equalInsensitive(*normalized, canonicalPath)) {
            return module;
        }

        // Never guess by basename. A resident module with the same filename
        // may be an older build with different export RVAs; calling into it
        // with addresses derived from the new file can crash the JVM.
        if (equalInsensitive(module.moduleName, wantedName) ||
            equalInsensitive(baseName(module.path), wantedName)) {
            failureError = ERROR_DUP_NAME;
            return std::nullopt;
        }
    }
    failureError = ERROR_MOD_NOT_FOUND;
    return std::nullopt;
}

bool targetIsAmd64(HANDLE process, bool& isAmd64, DWORD& failureError)
{
    failureError = ERROR_SUCCESS;
    isAmd64 = false;

    using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const FARPROC raw = kernel32 == nullptr
                            ? nullptr
                            : ::GetProcAddress(kernel32, "IsWow64Process2");
    IsWow64Process2Function isWow64Process2 = nullptr;
    static_assert(sizeof(isWow64Process2) == sizeof(raw));
    std::memcpy(&isWow64Process2, &raw, sizeof(raw));
    if (isWow64Process2 != nullptr) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine)) {
            failureError = ::GetLastError();
            return false;
        }
        isAmd64 = processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
                  nativeMachine == IMAGE_FILE_MACHINE_AMD64;
        return true;
    }

    SYSTEM_INFO information{};
    ::GetNativeSystemInfo(&information);
    BOOL wow64 = FALSE;
    if (!::IsWow64Process(process, &wow64)) {
        failureError = ::GetLastError();
        return false;
    }
    isAmd64 = wow64 == FALSE &&
              information.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
    return true;
}

std::optional<std::string> toUtf8(const std::wstring_view text)
{
    if (text.empty() ||
        text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    const int sourceLength = static_cast<int>(text.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                text.data(), sourceLength,
                                                nullptr, 0, nullptr, nullptr);
    if (required <= 0 || static_cast<std::size_t>(required) + 1U >
                             kMaximumOptionsBytes) {
        return std::nullopt;
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              text.data(), sourceLength, result.data(), required,
                              nullptr, nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

bool writeRemote(HANDLE process,
                 LPVOID destination,
                 const void* source,
                 const SIZE_T size,
                 DWORD& failureError)
{
    SIZE_T written = 0U;
    if (!::WriteProcessMemory(process, destination, source, size, &written) ||
        written != size) {
        failureError = ::GetLastError();
        if (failureError == ERROR_SUCCESS) {
            failureError = ERROR_WRITE_FAULT;
        }
        return false;
    }
    return true;
}

RemoteCallResult callRemote(HANDLE process,
                            const std::uintptr_t function,
                            LPVOID parameter)
{
    RemoteCallResult result;
    LPTHREAD_START_ROUTINE startRoutine = nullptr;
    static_assert(sizeof(startRoutine) == sizeof(function));
    std::memcpy(&startRoutine, &function, sizeof(startRoutine));

    UniqueHandle thread(::CreateRemoteThread(process, nullptr, 0U, startRoutine,
                                              parameter, 0U, nullptr));
    if (!thread.valid()) {
        result.error = ::GetLastError();
        return result;
    }

    result.threadCreated = true;
    result.stage = RemoteCallResult::Stage::WaitForThread;
    const DWORD wait = ::WaitForSingleObject(thread.get(), kRemoteCallTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        result.timedOut = true;
        result.error = ERROR_TIMEOUT;
        return result;
    }
    if (wait != WAIT_OBJECT_0) {
        result.error = ::GetLastError();
        return result;
    }
    result.completionObserved = true;
    result.stage = RemoteCallResult::Stage::QueryExitCode;
    if (!::GetExitCodeThread(thread.get(), &result.exitValue)) {
        result.error = ::GetLastError();
        return result;
    }
    result.completed = true;
    result.stage = RemoteCallResult::Stage::Complete;
    result.error = ERROR_SUCCESS;
    return result;
}

std::optional<std::pair<std::wstring, std::size_t>>
resolveLocalLoadLibrary(DWORD& failureError)
{
    failureError = ERROR_SUCCESS;
    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const FARPROC function = kernel32 == nullptr
                                 ? nullptr
                                 : ::GetProcAddress(kernel32, "LoadLibraryW");
    if (function == nullptr) {
        failureError = ::GetLastError();
        return std::nullopt;
    }

    std::uintptr_t functionAddress = 0U;
    static_assert(sizeof(functionAddress) == sizeof(function));
    std::memcpy(&functionAddress, &function, sizeof(functionAddress));

    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(functionAddress), &memory,
                       sizeof(memory)) != sizeof(memory) ||
        memory.AllocationBase == nullptr) {
        failureError = ::GetLastError();
        return std::nullopt;
    }
    const auto ownerBase = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    if (functionAddress < ownerBase) {
        failureError = ERROR_INVALID_ADDRESS;
        return std::nullopt;
    }

    std::vector<wchar_t> ownerPath(512U);
    DWORD length = 0U;
    for (;;) {
        length = ::GetModuleFileNameW(reinterpret_cast<HMODULE>(ownerBase),
                                      ownerPath.data(),
                                      static_cast<DWORD>(ownerPath.size()));
        if (length == 0U) {
            failureError = ::GetLastError();
            return std::nullopt;
        }
        if (length < ownerPath.size() - 1U) {
            break;
        }
        if (ownerPath.size() >= 32'768U) {
            failureError = ERROR_INSUFFICIENT_BUFFER;
            return std::nullopt;
        }
        ownerPath.resize(ownerPath.size() * 2U);
    }

    return std::pair(baseName(std::wstring_view(ownerPath.data(), length)),
                     static_cast<std::size_t>(functionAddress - ownerBase));
}

void printUsage()
{
    std::wcerr
        << L"Usage: McOverlayNativeLoader.exe <pid> <agent-dll-path> <options>\n"
        << L"Loads a normal, visible x64 DLL with remote LoadLibraryW and then "
           L"calls its exported McOverlay_Start(options).\n";
}

} // namespace

int wmain(const int argc, wchar_t* argv[])
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--help") {
        printUsage();
        return static_cast<int>(ExitCode::Success);
    }
    if (argc != 4) {
        printUsage();
        return fail(ExitCode::InvalidCommandLine,
                    L"expected PID, DLL path, and agent options");
    }

    const auto pid = parsePid(argv[1]);
    if (!pid) {
        return fail(ExitCode::InvalidPid,
                    L"PID must be an unsigned, non-zero decimal process ID");
    }

    DWORD error = ERROR_SUCCESS;
    const auto agentPath = canonicalDllPath(argv[2], error);
    if (!agentPath) {
        return fail(ExitCode::InvalidDllPath,
                    L"the agent DLL path is not an accessible regular file", error);
    }

    if (!isAmd64PeImage(*agentPath, error)) {
        return fail(ExitCode::ArchitectureMismatch,
                    L"the agent DLL is not a valid x64 PE image", error);
    }

    const auto options = toUtf8(argv[3]);
    if (!options) {
        return fail(ExitCode::InvalidOptions,
                    L"options must be non-empty valid UTF-16 and at most 65535 UTF-8 bytes");
    }

    constexpr DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                             PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                             PROCESS_VM_READ | SYNCHRONIZE;
    UniqueHandle process(::OpenProcess(access, FALSE, *pid));
    if (!process.valid()) {
        return fail(ExitCode::OpenProcessFailed,
                    L"cannot open the target; run both applications at the same integrity level",
                    ::GetLastError());
    }

    bool amd64 = false;
    if (!targetIsAmd64(process.get(), amd64, error)) {
        return fail(ExitCode::ArchitectureMismatch,
                    L"could not determine the target architecture", error);
    }
    if (!amd64) {
        return fail(ExitCode::ArchitectureMismatch,
                    L"the target is not a native x64 process");
    }

    const auto localLoadLibrary = resolveLocalLoadLibrary(error);
    if (!localLoadLibrary) {
        return fail(ExitCode::ResolveLoadLibraryFailed,
                    L"could not resolve the local LoadLibraryW owner module and RVA",
                    error);
    }

    const auto remoteOwner = findModuleByName(*pid, localLoadLibrary->first, error);
    if (!remoteOwner) {
        return fail(ExitCode::LoaderOwnerMissing,
                    L"the module that owns LoadLibraryW is absent from the target",
                    error);
    }
    if (localLoadLibrary->second >= remoteOwner->size ||
        remoteOwner->base > std::numeric_limits<std::uintptr_t>::max() -
                                localLoadLibrary->second) {
        return fail(ExitCode::ResolveLoadLibraryFailed,
                    L"the LoadLibraryW RVA is outside the target owner module");
    }
    const std::uintptr_t remoteLoadLibrary =
        remoteOwner->base + localLoadLibrary->second;

    // A resident module is a recoverable session, not an injection error.
    // McOverlay_Start serializes startup and replaces the previous runtime
    // after restoring its hooks, so browsing away in the controller can later
    // establish a fresh authenticated pipe without restarting Minecraft.
    // findModuleByPath requires the exact canonical image path; this prevents
    // using an RVA from a different on-disk build that merely shares a name.
    auto remoteAgent = findModuleByPath(*pid, *agentPath, error);
    if (!remoteAgent && error != ERROR_MOD_NOT_FOUND) {
        return fail(ExitCode::AgentModuleMissing,
                    L"could not determine whether the agent is already loaded",
                    error);
    }
    if (!remoteAgent) {
        const SIZE_T pathBytes = (agentPath->size() + 1U) * sizeof(wchar_t);
        RemoteAllocation remotePath(process.get(), pathBytes);
        if (remotePath.get() == nullptr) {
            return fail(ExitCode::AllocatePathFailed,
                        L"VirtualAllocEx failed for the DLL path", ::GetLastError());
        }
        if (!writeRemote(process.get(), remotePath.get(), agentPath->c_str(),
                         pathBytes, error)) {
            return fail(ExitCode::WritePathFailed,
                        L"WriteProcessMemory failed for the DLL path", error);
        }

        const RemoteCallResult loadCall =
            callRemote(process.get(), remoteLoadLibrary, remotePath.get());
        if (!loadCall.completionObserved && loadCall.threadCreated) {
            // WAIT_FAILED does not prove that the remote thread stopped. Keep
            // its parameter alive for the same reason as a normal timeout.
            remotePath.abandon();
        }
        if (loadCall.timedOut) {
            return fail(ExitCode::LoadThreadTimeout,
                        L"remote LoadLibraryW did not finish within 15 seconds");
        }
        if (!loadCall.threadCreated) {
            return fail(ExitCode::CreateLoadThreadFailed,
                        L"CreateRemoteThread failed for LoadLibraryW",
                        loadCall.error);
        }
        if (!loadCall.completed) {
            return fail(ExitCode::LoadThreadWaitFailed,
                        loadCall.stage == RemoteCallResult::Stage::QueryExitCode
                            ? L"GetExitCodeThread failed for LoadLibraryW"
                            : L"WaitForSingleObject failed for LoadLibraryW",
                        loadCall.error);
        }

        // GetExitCodeThread is DWORD-sized even for x64 threads, so an HMODULE
        // returned by LoadLibraryW is truncated. Never reconstruct a pointer
        // from it: enumerate after completion and retain the full uintptr_t
        // module base reported by Toolhelp.
        remoteAgent = findModuleByPath(*pid, *agentPath, error);
        if (!remoteAgent) {
            const ExitCode code = loadCall.exitValue == 0U
                                      ? ExitCode::LoadLibraryFailed
                                      : ExitCode::AgentModuleMissing;
            return fail(code,
                        L"the agent DLL is absent from the target module list after LoadLibraryW",
                        error);
        }
    }

    LocalModule localAgent(::LoadLibraryExW(agentPath->c_str(), nullptr,
                                             DONT_RESOLVE_DLL_REFERENCES));
    if (localAgent.get() == nullptr) {
        return fail(ExitCode::MapAgentLocallyFailed,
                    L"LoadLibraryExW(DONT_RESOLVE_DLL_REFERENCES) failed for the agent",
                    ::GetLastError());
    }
    const FARPROC localStart = ::GetProcAddress(localAgent.get(), "McOverlay_Start");
    if (localStart == nullptr) {
        return fail(ExitCode::StartExportMissing,
                    L"the agent DLL does not export McOverlay_Start",
                    ::GetLastError());
    }

    MEMORY_BASIC_INFORMATION startMemory{};
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(localStart), &startMemory,
                       sizeof(startMemory)) != sizeof(startMemory) ||
        startMemory.AllocationBase != localAgent.get()) {
        return fail(ExitCode::InvalidExportRva,
                    L"McOverlay_Start is not owned by the mapped agent image");
    }

    std::uintptr_t localStartAddress = 0U;
    static_assert(sizeof(localStartAddress) == sizeof(localStart));
    std::memcpy(&localStartAddress, &localStart, sizeof(localStartAddress));
    const auto localAgentBase = reinterpret_cast<std::uintptr_t>(localAgent.get());
    if (localStartAddress < localAgentBase) {
        return fail(ExitCode::InvalidExportRva,
                    L"McOverlay_Start resolved outside the local agent image");
    }
    const std::size_t startRva =
        static_cast<std::size_t>(localStartAddress - localAgentBase);
    if (startRva >= remoteAgent->size ||
        remoteAgent->base > std::numeric_limits<std::uintptr_t>::max() - startRva) {
        return fail(ExitCode::InvalidExportRva,
                    L"McOverlay_Start RVA is outside the remote agent image");
    }
    const std::uintptr_t remoteStart = remoteAgent->base + startRva;

    std::vector<char> optionBytes(options->begin(), options->end());
    optionBytes.push_back('\0');
    RemoteAllocation remoteOptions(process.get(), optionBytes.size());
    if (remoteOptions.get() == nullptr) {
        return fail(ExitCode::AllocateOptionsFailed,
                    L"VirtualAllocEx failed for the UTF-8 options", ::GetLastError());
    }
    if (!writeRemote(process.get(), remoteOptions.get(), optionBytes.data(),
                     optionBytes.size(), error)) {
        return fail(ExitCode::WriteOptionsFailed,
                    L"WriteProcessMemory failed for the UTF-8 options", error);
    }

    const RemoteCallResult startCall =
        callRemote(process.get(), remoteStart, remoteOptions.get());
    if (!startCall.completionObserved && startCall.threadCreated) {
        remoteOptions.abandon();
    }
    if (startCall.timedOut) {
        return fail(ExitCode::StartThreadTimeout,
                    L"McOverlay_Start did not finish within 15 seconds");
    }
    if (!startCall.threadCreated) {
        return fail(ExitCode::CreateStartThreadFailed,
                    L"CreateRemoteThread failed for McOverlay_Start",
                    startCall.error);
    }
    if (!startCall.completed) {
        return fail(ExitCode::StartThreadWaitFailed,
                    startCall.stage == RemoteCallResult::Stage::QueryExitCode
                        ? L"GetExitCodeThread failed for McOverlay_Start"
                        : L"WaitForSingleObject failed for McOverlay_Start",
                    startCall.error);
    }
    if (startCall.exitValue != 0U) {
        return fail(ExitCode::StartExportFailed,
                    L"McOverlay_Start returned " +
                        std::to_wstring(startCall.exitValue));
    }

    std::wcout << L"NATIVE_LOADER_OK pid=" << *pid << L" module=0x"
               << std::hex << remoteAgent->base << L" start=0x" << remoteStart
               << std::dec << L'\n';
    return static_cast<int>(ExitCode::Success);
}
