#include "AgentOptions.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace mcoverlay {
namespace {

std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1U);
    }
    return value;
}

std::wstring utf8ToWide(const std::string_view value)
{
    if (value.empty()) {
        return {};
    }

    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), static_cast<int>(value.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}

bool validToken(const std::string_view token) noexcept
{
    return token.size() == 32U &&
           std::all_of(token.begin(), token.end(), [](const unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

} // namespace

AgentOptions parseAgentOptions(const char* const options)
{
    AgentOptions result;

    if (options == nullptr || *options == '\0') {
        return result;
    }

    const std::string_view all(options);
    std::size_t begin = 0U;
    while (begin < all.size()) {
        const std::size_t end = all.find(';', begin);
        const auto token = trim(all.substr(begin, end == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : end - begin));
        const std::size_t separator = token.find('=');
        if (separator != std::string_view::npos) {
            const auto key = trim(token.substr(0U, separator));
            const auto value = trim(token.substr(separator + 1U));
            if (key == "pipe") {
                result.pipeName = utf8ToWide(value);
            } else if (key == "token" && validToken(value)) {
                result.token.assign(value);
                std::transform(result.token.begin(), result.token.end(), result.token.begin(),
                               [](const unsigned char c) {
                                   return static_cast<char>(std::tolower(c));
                               });
            } else if (key == "protocol") {
                unsigned parsed = 0U;
                const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (converted.ec == std::errc{} && converted.ptr == value.data() + value.size()) {
                    result.protocol = parsed;
                }
            }
        }

        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }

    return result;
}

bool AgentOptions::valid() const noexcept
{
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    return protocol == 1U && token.size() == 32U && pipeName.size() > prefix.size() &&
           pipeName.starts_with(prefix);
}

} // namespace mcoverlay
