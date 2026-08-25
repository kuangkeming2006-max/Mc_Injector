#include "ipc_client.h"

#include "include/mcoverlay/ControlProtocol.h"
#include "src/AgentLog.h"

#include <array>

namespace mcoverlay {
namespace {

bool waitOverlapped(HANDLE pipe,
                    OVERLAPPED& operation,
                    HANDLE stopEvent,
                    DWORD& transferred) noexcept
{
    const std::array<HANDLE, 2U> events{stopEvent, operation.hEvent};
    const DWORD wait = ::WaitForMultipleObjects(static_cast<DWORD>(events.size()),
                                                 events.data(), FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) {
        ::CancelIoEx(pipe, &operation);
        ::WaitForSingleObject(operation.hEvent, INFINITE);
        return false;
    }
    return wait == WAIT_OBJECT_0 + 1U &&
           ::GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
}

bool writeChunk(HANDLE pipe, const char* data, const DWORD size) noexcept
{
    if (size == 0U) {
        return true;
    }
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD written = 0U;
    BOOL result = ::WriteFile(pipe, data, size, &written, &operation);
    if (result == FALSE && ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD wait = ::WaitForSingleObject(event, 1000U);
        if (wait == WAIT_OBJECT_0) {
            result = ::GetOverlappedResult(pipe, &operation, &written, FALSE);
        } else {
            ::CancelIoEx(pipe, &operation);
            ::WaitForSingleObject(event, INFINITE);
            result = FALSE;
        }
    }
    ::CloseHandle(event);
    return result != FALSE && written == size;
}

} // namespace

IpcClient::IpcClient(std::wstring pipeName)
    : m_pipeName(std::move(pipeName))
{
}

IpcClient::~IpcClient()
{
    cancel();
}

HANDLE IpcClient::connect(HANDLE stopEvent) noexcept
{
    constexpr ULONGLONG kInitialConnectTimeoutMs = 15'000ULL;
    const ULONGLONG deadline = ::GetTickCount64() + kInitialConnectTimeoutMs;
    while (::WaitForSingleObject(stopEvent, 0U) == WAIT_TIMEOUT) {
        HANDLE pipe = ::CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0U, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            m_pipe.store(pipe, std::memory_order_release);
            return pipe;
        }

        const DWORD error = ::GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            log::error("CreateFileW failed for the controller pipe.");
        }
        if (::WaitForSingleObject(stopEvent, 250U) != WAIT_TIMEOUT) {
            break;
        }
        if (::GetTickCount64() >= deadline) {
            log::error("Controller pipe connection timed out after 15 seconds.");
            break;
        }
    }
    return INVALID_HANDLE_VALUE;
}

bool IpcClient::sendLine(const std::string_view line) noexcept
{
    std::lock_guard lock(m_writeMutex);
    const HANDLE pipe = m_pipe.load(std::memory_order_acquire);
    if (pipe == INVALID_HANDLE_VALUE || pipe == nullptr) {
        return false;
    }

    // sendLine is used by the real-time mailbox thread and is noexcept. Avoid
    // constructing a framed std::string here: an allocation failure in a
    // noexcept function would terminate the target JVM. The write mutex stays
    // held across the optional newline, so frames cannot interleave.
    if (line.size() > protocol::kMaximumLineBytes ||
        line.size() > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }
    if (!writeChunk(pipe, line.data(), static_cast<DWORD>(line.size()))) {
        return false;
    }
    static constexpr char newline = '\n';
    return (!line.empty() && line.back() == newline) ||
           writeChunk(pipe, &newline, 1U);
}

bool IpcClient::readLoop(HANDLE pipe,
                         HANDLE stopEvent,
                         const LineHandler& onLine) noexcept
{
    std::array<char, 512U> buffer{};
    std::string pending;
    while (::WaitForSingleObject(stopEvent, 0U) == WAIT_TIMEOUT) {
        HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            return false;
        }
        OVERLAPPED operation{};
        operation.hEvent = event;
        DWORD bytes = 0U;
        BOOL result = ::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                                 &bytes, &operation);
        if (result == FALSE && ::GetLastError() == ERROR_IO_PENDING) {
            result = waitOverlapped(pipe, operation, stopEvent, bytes) ? TRUE : FALSE;
        }
        ::CloseHandle(event);
        if (result == FALSE || bytes == 0U) {
            return false;
        }

        pending.append(buffer.data(), bytes);
        if (pending.size() > protocol::kMaximumLineBytes) {
            (void)sendLine("ERROR LINE_TOO_LONG command-exceeds-limit");
            return false;
        }
        std::size_t newline = std::string::npos;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0U, newline);
            pending.erase(0U, newline + 1U);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!onLine(line)) {
                return true;
            }
        }
    }
    return true;
}

void IpcClient::closeCurrent(const HANDLE expected) noexcept
{
    std::lock_guard lock(m_writeMutex);
    HANDLE current = expected;
    if (m_pipe.compare_exchange_strong(current, INVALID_HANDLE_VALUE,
                                       std::memory_order_acq_rel)) {
        ::CancelIoEx(expected, nullptr);
        ::CloseHandle(expected);
    }
}

void IpcClient::run(HANDLE stopEvent,
                    const EventHandler& onConnected,
                    const LineHandler& onLine,
                    const EventHandler& onClosing,
                    const EventHandler& onDisconnected) noexcept
{
    const HANDLE pipe = connect(stopEvent);
    if (pipe == INVALID_HANDLE_VALUE) {
        onClosing();
        onDisconnected();
        return;
    }

    onConnected();
    (void)readLoop(pipe, stopEvent, onLine);
    // Keep the authenticated pipe alive while the caller removes hooks and
    // releases renderer/JNI state. This provides a real cleanup-complete
    // boundary instead of making pipe disconnect race target shutdown.
    onClosing();
    closeCurrent(pipe);
    onDisconnected();
}

void IpcClient::cancel() noexcept
{
    const HANDLE pipe = m_pipe.load(std::memory_order_acquire);
    if (pipe != INVALID_HANDLE_VALUE && pipe != nullptr) {
        ::CancelIoEx(pipe, nullptr);
    }
}

} // namespace mcoverlay
