#include "WinApiKeyStore.h"

#include "WinSettings.h"

#include <wincrypt.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <string>

namespace cli {
namespace {

// Same entropy and description the Qt ApiKeyStore uses, so a key saved by
// either front-end decrypts in the other.
constexpr unsigned char kEntropyBytes[]{
    0x4d, 0x63, 0x4f, 0x76, 0x65, 0x72, 0x6c, 0x61,
    0x79, 0x2d, 0x48, 0x79, 0x70, 0x69, 0x78, 0x65, 0x6c};

constexpr wchar_t kSubKey[] = L"hypixel";
constexpr wchar_t kValueName[] = L"protectedApiKey";

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
    (void) ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                 static_cast<int>(text.size()), utf8.data(),
                                 needed, nullptr, nullptr);
    return utf8;
}

std::wstring toBase64(const std::vector<unsigned char> &bytes)
{
    if (bytes.empty())
        return {};
    DWORD size = 0;
    if (::CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()),
                               CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                               nullptr, &size) == FALSE
        || size == 0) {
        return {};
    }
    std::wstring encoded(size, L'\0');
    if (::CryptBinaryToStringW(bytes.data(), static_cast<DWORD>(bytes.size()),
                               CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                               encoded.data(), &size) == FALSE) {
        return {};
    }
    encoded.resize(size);
    return encoded;
}

std::vector<unsigned char> fromBase64(const std::wstring &encoded)
{
    if (encoded.empty())
        return {};
    DWORD size = 0;
    if (::CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                               CRYPT_STRING_BASE64, nullptr, &size,
                               nullptr, nullptr) == FALSE
        || size == 0) {
        return {};
    }
    std::vector<unsigned char> bytes(size);
    if (::CryptStringToBinaryW(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                               CRYPT_STRING_BASE64, bytes.data(), &size,
                               nullptr, nullptr) == FALSE) {
        return {};
    }
    bytes.resize(size);
    return bytes;
}

} // namespace

void WinApiKeyStore::replace(std::string key)
{
    std::fill(m_key.begin(), m_key.end(), '\0');
    m_key = std::move(key);
}

std::vector<unsigned char> WinApiKeyStore::protect(const std::string &plainText)
{
    if (plainText.empty())
        return {};

    DATA_BLOB input{
        static_cast<DWORD>(plainText.size()),
        reinterpret_cast<BYTE *>(const_cast<char *>(plainText.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropyBytes)),
                      const_cast<BYTE *>(kEntropyBytes)};
    DATA_BLOB output{};
    if (::CryptProtectData(&input, L"Java Overlay Studio Hypixel key", &entropy,
                           nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                           &output) == FALSE) {
        return {};
    }
    std::vector<unsigned char> encrypted(output.pbData,
                                         output.pbData + output.cbData);
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return encrypted;
}

std::string WinApiKeyStore::unprotect(const std::vector<unsigned char> &cipherText)
{
    if (cipherText.empty())
        return {};

    DATA_BLOB input{static_cast<DWORD>(cipherText.size()),
                    const_cast<unsigned char *>(cipherText.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropyBytes)),
                      const_cast<BYTE *>(kEntropyBytes)};
    DATA_BLOB output{};
    if (::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &output) == FALSE) {
        return {};
    }
    std::string plainText(reinterpret_cast<const char *>(output.pbData),
                          static_cast<std::size_t>(output.cbData));
    ::SecureZeroMemory(output.pbData, output.cbData);
    ::LocalFree(output.pbData);
    return plainText;
}

void WinApiKeyStore::load()
{
    const char *environment = std::getenv("HYPIXEL_API_KEY");
    if (environment != nullptr) {
        std::string value(environment);
        while (!value.empty() && static_cast<unsigned char>(value.back()) <= 0x20U)
            value.pop_back();
        while (!value.empty() && static_cast<unsigned char>(value.front()) <= 0x20U)
            value.erase(value.begin());
        if (!value.empty()) {
            replace(value);
            m_status = L"Using HYPIXEL_API_KEY from the process environment";
            return;
        }
    }

    const std::wstring fullKey = std::wstring(WinSettings::kRootKey) + L"\\" + kSubKey;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, fullKey.c_str(), 0, KEY_READ, &key)
        != ERROR_SUCCESS) {
        m_status = L"No Hypixel API key configured";
        return;
    }

    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::string plain;
    if (::RegQueryValueExW(key, kValueName, nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS
        && type == REG_SZ) {
        plain = unprotect(fromBase64(buffer));
    }
    ::RegCloseKey(key);

    replace(std::move(plain));
    m_status = configured()
        ? L"API key is encrypted for this Windows user"
        : L"No Hypixel API key configured";
}

bool WinApiKeyStore::saveKey(const std::wstring &key)
{
    std::wstring trimmed = key;
    while (!trimmed.empty() && iswspace(trimmed.back()) != 0)
        trimmed.pop_back();
    while (!trimmed.empty() && iswspace(trimmed.front()) != 0)
        trimmed.erase(trimmed.begin());

    const std::string utf8 = wideToUtf8(trimmed);
    if (utf8.size() < 16 || utf8.size() > 256
        || std::any_of(utf8.cbegin(), utf8.cend(), [](const char value) {
               return static_cast<unsigned char>(value) <= 0x20U;
           })) {
        m_status = L"Enter a valid Hypixel application API key";
        return false;
    }

    const std::vector<unsigned char> encrypted = protect(utf8);
    if (encrypted.empty()) {
        m_status = L"Windows could not protect the API key";
        return false;
    }

    const std::wstring fullKey = std::wstring(WinSettings::kRootKey) + L"\\" + kSubKey;
    HKEY hKey = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, fullKey.c_str(), 0, nullptr, 0,
                          KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        m_status = L"The encrypted API key could not be saved";
        return false;
    }
    const std::wstring encoded = toBase64(encrypted);
    const LONG result = ::RegSetValueExW(
        hKey, kValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(encoded.c_str()),
        static_cast<DWORD>((encoded.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(hKey);
    if (result != ERROR_SUCCESS) {
        m_status = L"The encrypted API key could not be saved";
        return false;
    }

    replace(utf8);
    m_status = L"API key saved with Windows user encryption";
    return true;
}

void WinApiKeyStore::clearKey()
{
    const std::wstring fullKey = std::wstring(WinSettings::kRootKey) + L"\\" + kSubKey;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, fullKey.c_str(), 0, KEY_SET_VALUE, &key)
        == ERROR_SUCCESS) {
        (void) ::RegDeleteValueW(key, kValueName);
        ::RegCloseKey(key);
    }
    replace({});
    m_status = L"Stored API key removed";
}

} // namespace cli
