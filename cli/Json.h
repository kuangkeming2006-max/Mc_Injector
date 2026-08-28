#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cli::json {

// Minimal dependency-free JSON DOM. Hypixel and Minecraft Services responses
// are parsed with this instead of a heavyweight library so the lightweight
// injector stays C++/Win32-only. Strings are decoded to UTF-16 on parse.
struct Value
{
    enum class Type { Null, Boolean, Number, String, Array, Object } type = Type::Null;

    bool boolean = false;
    double number = 0.0;
    std::wstring string;
    std::vector<Value> array;
    std::vector<std::pair<std::wstring, Value>> object;

    [[nodiscard]] const Value *find(std::wstring_view key) const;
    [[nodiscard]] long long integer(long long fallback = 0) const;
    [[nodiscard]] std::wstring asString(std::wstring_view fallback = {}) const;
    [[nodiscard]] bool asBool(bool fallback = false) const;
};

// Parses UTF-8 JSON text. Returns false and fills `error` on failure.
bool parse(std::string_view utf8, Value &root, std::string &error);

} // namespace cli::json
