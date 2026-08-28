#pragma once

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cli {

struct HttpResponse
{
    int status = 0;
    std::vector<std::byte> body;
    std::map<std::wstring, std::wstring> headers; // lowercased names
    std::wstring error;                           // WinHTTP failure text
    bool tooLarge = false;
    bool canceled = false;
};

// One instance per worker thread. Synchronous WinHTTP GET with cooperative
// cancellation through the active-handle slot: the owner thread publishes the
// in-flight request handle, and another thread may claim it (exchange to
// nullptr) and WinHttpCloseHandle it, which aborts the blocked call.
class WinHttpClient
{
public:
    WinHttpClient();
    ~WinHttpClient();

    WinHttpClient(const WinHttpClient &) = delete;
    WinHttpClient &operator=(const WinHttpClient &) = delete;

    // Performs a GET. The body is capped at maxBytes; exceeding it aborts the
    // request and sets tooLarge. activeSlot/cancelFlag are optional.
    bool get(const std::wstring &url,
             const std::vector<std::pair<std::wstring, std::wstring>> &headers,
             uint64_t maxBytes,
             HttpResponse &response,
             std::atomic<HANDLE> *activeSlot,
             const std::atomic<bool> *cancelFlag);

    // Claims and closes the in-flight request handle from any thread.
    static bool cancelRequest(std::atomic<HANDLE> &activeSlot);

private:
    HINTERNET m_session = nullptr;
};

} // namespace cli
