#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace mcoverlay {

class IpcClient final {
public:
    using LineHandler = std::function<bool(std::string_view)>;
    using EventHandler = std::function<void()>;

    explicit IpcClient(std::wstring pipeName);
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    // Blocks on the agent's control thread. Returning false from onLine asks
    // the client loop to finish after processing that line (DETACH).
    void run(HANDLE stopEvent,
             const EventHandler& onConnected,
             const LineHandler& onLine,
             const EventHandler& onClosing,
             const EventHandler& onDisconnected) noexcept;

    [[nodiscard]] bool sendLine(std::string_view line) noexcept;
    void cancel() noexcept;

private:
    [[nodiscard]] HANDLE connect(HANDLE stopEvent) noexcept;
    [[nodiscard]] bool readLoop(HANDLE pipe,
                                HANDLE stopEvent,
                                const LineHandler& onLine) noexcept;
    void closeCurrent(HANDLE expected) noexcept;

    std::wstring m_pipeName;
    std::atomic<HANDLE> m_pipe{INVALID_HANDLE_VALUE};
    std::mutex m_writeMutex;
};

} // namespace mcoverlay
