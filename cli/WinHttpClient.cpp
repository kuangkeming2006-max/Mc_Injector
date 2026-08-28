#include "WinHttpClient.h"

#include <algorithm>
#include <cwchar>
#include <string>

namespace cli {
namespace {

// Parses "Name: Value" pairs from the raw CRLF header block into a map with
// lowercased names.
std::map<std::wstring, std::wstring> parseRawHeaders(const std::wstring &raw)
{
    std::map<std::wstring, std::wstring> headers;
    std::size_t begin = 0;
    while (begin < raw.size()) {
        const std::size_t end = raw.find(L"\r\n", begin);
        const std::wstring line = raw.substr(begin, end == std::wstring::npos
                                                         ? std::wstring::npos
                                                         : end - begin);
        const std::size_t separator = line.find(L':');
        if (separator != std::wstring::npos) {
            std::wstring name = line.substr(0, separator);
            std::wstring value = line.substr(separator + 1);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](wchar_t c) { return towlower(c); });
            while (!value.empty() && iswspace(value.front()) != 0)
                value.erase(value.begin());
            while (!value.empty() && iswspace(value.back()) != 0)
                value.pop_back();
            headers[name] = std::move(value);
        }
        if (end == std::wstring::npos)
            break;
        begin = end + 2;
    }
    return headers;
}

} // namespace

WinHttpClient::WinHttpClient()
    : m_session(::WinHttpOpen(L"JavaOverlayStudio/1.0 (McInjectorLite)",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0))
{
}

WinHttpClient::~WinHttpClient()
{
    if (m_session != nullptr)
        ::WinHttpCloseHandle(m_session);
}

bool WinHttpClient::cancelRequest(std::atomic<HANDLE> &activeSlot)
{
    const HANDLE handle = activeSlot.exchange(nullptr);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;
    ::WinHttpCloseHandle(handle);
    return true;
}

bool WinHttpClient::get(const std::wstring &url,
                        const std::vector<std::pair<std::wstring, std::wstring>> &headers,
                        const uint64_t maxBytes,
                        HttpResponse &response,
                        std::atomic<HANDLE> *activeSlot,
                        const std::atomic<bool> *cancelFlag)
{
    response = {};
    if (m_session == nullptr) {
        response.error = L"WinHttpOpen failed";
        return false;
    }
    ::WinHttpSetTimeouts(m_session, 10'000, 10'000, 10'000, 10'000);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0,
                          &components) == FALSE) {
        response.error = L"Invalid URL";
        return false;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS
        && components.nScheme != INTERNET_SCHEME_HTTP) {
        response.error = L"Unsupported URL scheme";
        return false;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    const HINTERNET connection = ::WinHttpConnect(m_session, host.c_str(),
                                                  components.nPort, 0);
    if (connection == nullptr) {
        response.error = L"WinHttpConnect failed";
        return false;
    }

    const wchar_t *path = components.lpszUrlPath;
    DWORD pathLength = components.dwUrlPathLength + components.dwExtraInfoLength;
    std::wstring pathBuffer;
    if (path == nullptr || pathLength == 0) {
        path = L"/";
        pathLength = 1;
    } else if (components.lpszUrlPath != nullptr
               && components.dwExtraInfoLength > 0
               && pathLength > components.dwUrlPathLength) {
        // lpszUrlPath is not null-terminated when extra info follows.
        pathBuffer.assign(components.lpszUrlPath, pathLength);
        path = pathBuffer.c_str();
    }

    HINTERNET request = ::WinHttpOpenRequest(
        connection, L"GET", path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (request == nullptr) {
        ::WinHttpCloseHandle(connection);
        response.error = L"WinHttpOpenRequest failed";
        return false;
    }

    std::wstring headerBlock;
    for (const auto &header : headers) {
        headerBlock += header.first;
        headerBlock += L": ";
        headerBlock += header.second;
        headerBlock += L"\r\n";
    }
    if (!headerBlock.empty()
        && ::WinHttpAddRequestHeaders(request, headerBlock.c_str(),
                                      static_cast<DWORD>(headerBlock.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD) == FALSE) {
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        response.error = L"WinHttpAddRequestHeaders failed";
        return false;
    }

    // Publish the request handle so another thread can claim and close it.
    if (activeSlot != nullptr)
        activeSlot->store(request, std::memory_order_release);
    const auto stillOurs = [&]() {
        return (activeSlot == nullptr || activeSlot->load() == request)
            && (cancelFlag == nullptr || cancelFlag->load() == false);
    };

    if (!stillOurs()) {
        response.canceled = true;
        if (activeSlot != nullptr)
            activeSlot->compare_exchange_strong(request, nullptr);
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        return false;
    }

    BOOL ok = ::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0,
                                   static_cast<DWORD_PTR>(0));
    if (!stillOurs()) {
        response.canceled = true;
        if (activeSlot != nullptr)
            activeSlot->compare_exchange_strong(request, nullptr);
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        return false;
    }
    if (ok == FALSE) {
        response.error = L"WinHttpSendRequest failed";
        if (activeSlot != nullptr)
            activeSlot->compare_exchange_strong(request, nullptr);
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        return false;
    }

    ok = ::WinHttpReceiveResponse(request, nullptr);
    if (!stillOurs()) {
        response.canceled = true;
        if (activeSlot != nullptr)
            activeSlot->compare_exchange_strong(request, nullptr);
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        return false;
    }
    if (ok == FALSE) {
        response.error = L"WinHttpReceiveResponse failed";
        if (activeSlot != nullptr)
            activeSlot->compare_exchange_strong(request, nullptr);
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connection);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    (void) ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE
                                           | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status,
                                 &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    DWORD rawSize = 0;
    if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                              WINHTTP_HEADER_NAME_BY_INDEX,
                              WINHTTP_NO_OUTPUT_BUFFER, &rawSize,
                              WINHTTP_NO_HEADER_INDEX) == FALSE
        && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        std::wstring raw(rawSize / sizeof(wchar_t), L'\0');
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                  WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                  &rawSize, WINHTTP_NO_HEADER_INDEX) != FALSE) {
            raw.resize(rawSize / sizeof(wchar_t));
            response.headers = parseRawHeaders(raw);
        }
    }

    std::vector<std::byte> body;
    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request, &available) == FALSE)
            break;
        if (available == 0)
            break;

        const uint64_t remaining = maxBytes > body.size() ? maxBytes - body.size() : 0;
        const DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(
            available, std::max<uint64_t>(1, remaining + 1)));
        std::vector<std::byte> chunk(toRead);
        DWORD read = 0;
        if (::WinHttpReadData(request, chunk.data(), toRead, &read) == FALSE)
            break;
        body.insert(body.end(), chunk.begin(), chunk.begin() + read);
        if (body.size() > maxBytes) {
            response.tooLarge = true;
            body.clear();
            break;
        }
        if (!stillOurs()) {
            response.canceled = true;
            body.clear();
            break;
        }
    }

    response.body = std::move(body);

    // Release the slot if we still own the handle; otherwise the cancelling
    // thread owns it and will close it.
    if (activeSlot != nullptr && activeSlot->load() == request) {
        activeSlot->store(nullptr, std::memory_order_release);
        ::WinHttpCloseHandle(request);
    }
    ::WinHttpCloseHandle(connection);
    return response.status >= 200 && response.status < 300
        && !response.tooLarge && !response.canceled;
}

} // namespace cli
