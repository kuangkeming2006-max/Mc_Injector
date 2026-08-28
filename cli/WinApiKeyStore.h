#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace cli {

// Stores the Hypixel application key with Windows DPAPI, in the same registry
// location the Qt dashboard uses. The clear-text value is never written to
// disk; callers can only replace/clear it and ask whether a usable key exists.
// HYPIXEL_API_KEY remains a non-persistent process-environment override.
class WinApiKeyStore
{
public:
    WinApiKeyStore() { load(); }
    ~WinApiKeyStore() { replace({}); }

    WinApiKeyStore(const WinApiKeyStore &) = delete;
    WinApiKeyStore &operator=(const WinApiKeyStore &) = delete;

    [[nodiscard]] bool configured() const noexcept { return !m_key.empty(); }
    [[nodiscard]] const std::string &apiKey() const noexcept { return m_key; }
    [[nodiscard]] const std::wstring &statusMessage() const noexcept { return m_status; }

    bool saveKey(const std::wstring &key);
    void clearKey();

private:
    void load();
    void replace(std::string key);
    [[nodiscard]] static std::vector<unsigned char> protect(const std::string &plainText);
    [[nodiscard]] static std::string unprotect(const std::vector<unsigned char> &cipherText);

    std::string m_key;
    std::wstring m_status;
};

} // namespace cli
