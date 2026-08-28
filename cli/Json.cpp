#include "Json.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <limits>

namespace cli::json {
namespace {

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

void appendUtf8(std::string &out, const unsigned codePoint)
{
    if (codePoint <= 0x7FU) {
        out += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7FFU) {
        out += static_cast<char>(0xC0U | (codePoint >> 6U));
        out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    } else if (codePoint <= 0xFFFFU) {
        out += static_cast<char>(0xE0U | (codePoint >> 12U));
        out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (codePoint >> 18U));
        out += static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    }
}

unsigned hexDigit(const char character)
{
    if (character >= '0' && character <= '9')
        return static_cast<unsigned>(character - '0');
    if (character >= 'a' && character <= 'f')
        return static_cast<unsigned>(character - 'a') + 10U;
    if (character >= 'A' && character <= 'F')
        return static_cast<unsigned>(character - 'A') + 10U;
    return 0xFFFFFFFFU;
}

struct Parser
{
    std::string_view text;
    std::size_t pos = 0;
    std::string error;

    void skipWhitespace()
    {
        while (pos < text.size()) {
            const char character = text[pos];
            if (character != ' ' && character != '\t' && character != '\n'
                && character != '\r') {
                break;
            }
            ++pos;
        }
    }

    bool fail(const char *message)
    {
        error = message;
        return false;
    }

    bool parseValue(Value &out)
    {
        skipWhitespace();
        if (pos >= text.size())
            return fail("unexpected end of input");

        switch (text[pos]) {
        case 'n':
            return parseLiteral("null", Value::Type::Null, out);
        case 't':
            if (!parseLiteral("true", Value::Type::Boolean, out))
                return false;
            out.boolean = true;
            return true;
        case 'f':
            return parseLiteral("false", Value::Type::Boolean, out);
        case '"':
            out.type = Value::Type::String;
            return parseString(out.string);
        case '[':
            return parseArray(out);
        case '{':
            return parseObject(out);
        default:
            return parseNumber(out);
        }
    }

    bool parseLiteral(const char *literal, const Value::Type type, Value &out)
    {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text.substr(pos, length) != literal)
            return fail("invalid literal");
        pos += length;
        out.type = type;
        return true;
    }

    bool parseNumber(Value &out)
    {
        const std::size_t begin = pos;
        if (pos < text.size() && text[pos] == '-')
            ++pos;
        while (pos < text.size()) {
            const char character = text[pos];
            if ((character >= '0' && character <= '9') || character == '.'
                || character == 'e' || character == 'E' || character == '+'
                || character == '-') {
                ++pos;
            } else {
                break;
            }
        }
        if (pos == begin)
            return fail("invalid number");
        const std::string slice(text.substr(begin, pos - begin));
        char *end = nullptr;
        const double number = std::strtod(slice.c_str(), &end);
        if (end == slice.c_str() || *end != '\0' || !std::isfinite(number))
            return fail("invalid number");
        out.type = Value::Type::Number;
        out.number = number;
        return true;
    }

    bool parseString(std::wstring &out)
    {
        if (pos >= text.size() || text[pos] != '"')
            return fail("expected string");
        ++pos;

        std::string raw;
        while (true) {
            if (pos >= text.size())
                return fail("unterminated string");
            const char character = text[pos++];
            if (character == '"')
                break;
            if (character == '\\') {
                if (pos >= text.size())
                    return fail("invalid escape");
                const char escape = text[pos++];
                switch (escape) {
                case '"': raw += '"'; break;
                case '\\': raw += '\\'; break;
                case '/': raw += '/'; break;
                case 'b': raw += '\b'; break;
                case 'f': raw += '\f'; break;
                case 'n': raw += '\n'; break;
                case 'r': raw += '\r'; break;
                case 't': raw += '\t'; break;
                case 'u': {
                    if (pos + 4 > text.size())
                        return fail("invalid unicode escape");
                    unsigned codePoint = (hexDigit(text[pos]) << 12U)
                        | (hexDigit(text[pos + 1]) << 8U)
                        | (hexDigit(text[pos + 2]) << 4U)
                        | hexDigit(text[pos + 3]);
                    if (codePoint == 0xFFFFFFFFU)
                        return fail("invalid unicode escape");
                    pos += 4;
                    if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
                        // High surrogate: a low surrogate must follow.
                        if (pos + 6 > text.size() || text[pos] != '\\'
                            || text[pos + 1] != 'u') {
                            return fail("invalid surrogate pair");
                        }
                        const unsigned low = (hexDigit(text[pos + 2]) << 12U)
                            | (hexDigit(text[pos + 3]) << 8U)
                            | (hexDigit(text[pos + 4]) << 4U)
                            | hexDigit(text[pos + 5]);
                        if (low < 0xDC00U || low > 0xDFFFU)
                            return fail("invalid surrogate pair");
                        pos += 6;
                        codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U)
                            + (low - 0xDC00U);
                    } else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
                        return fail("lone low surrogate");
                    }
                    appendUtf8(raw, codePoint);
                    break;
                }
                default:
                    return fail("invalid escape");
                }
            } else if (static_cast<unsigned char>(character) < 0x20U) {
                return fail("control character in string");
            } else {
                raw += character;
            }
        }
        out = utf8ToWide(raw);
        return true;
    }

    bool parseArray(Value &out)
    {
        ++pos; // '['
        out.type = Value::Type::Array;
        skipWhitespace();
        if (pos < text.size() && text[pos] == ']') {
            ++pos;
            return true;
        }
        while (true) {
            Value element;
            if (!parseValue(element))
                return false;
            out.array.push_back(std::move(element));
            skipWhitespace();
            if (pos >= text.size())
                return fail("unterminated array");
            if (text[pos] == ']') {
                ++pos;
                return true;
            }
            if (text[pos] != ',')
                return fail("expected ',' or ']' in array");
            ++pos;
        }
    }

    bool parseObject(Value &out)
    {
        ++pos; // '{'
        out.type = Value::Type::Object;
        skipWhitespace();
        if (pos < text.size() && text[pos] == '}') {
            ++pos;
            return true;
        }
        while (true) {
            skipWhitespace();
            std::wstring key;
            if (!parseString(key))
                return false;
            skipWhitespace();
            if (pos >= text.size() || text[pos] != ':')
                return fail("expected ':' in object");
            ++pos;
            Value element;
            if (!parseValue(element))
                return false;
            out.object.emplace_back(std::move(key), std::move(element));
            skipWhitespace();
            if (pos >= text.size())
                return fail("unterminated object");
            if (text[pos] == '}') {
                ++pos;
                return true;
            }
            if (text[pos] != ',')
                return fail("expected ',' or '}' in object");
            ++pos;
        }
    }
};

} // namespace

const Value *Value::find(const std::wstring_view key) const
{
    if (type != Type::Object)
        return nullptr;
    for (const auto &entry : object) {
        if (entry.first == key)
            return &entry.second;
    }
    return nullptr;
}

long long Value::integer(const long long fallback) const
{
    if (type == Type::Number) {
        if (!std::isfinite(number)
            || number < static_cast<double>(std::numeric_limits<long long>::min())
            || number > static_cast<double>(std::numeric_limits<long long>::max())) {
            return fallback;
        }
        return static_cast<long long>(number);
    }
    if (type == Type::String) {
        wchar_t *end = nullptr;
        const long long parsed = std::wcstoll(string.c_str(), &end, 10);
        if (end != string.c_str() && *end == L'\0')
            return parsed;
    }
    return fallback;
}

std::wstring Value::asString(const std::wstring_view fallback) const
{
    return type == Type::String ? string : std::wstring(fallback);
}

bool Value::asBool(const bool fallback) const
{
    if (type == Type::Boolean)
        return boolean;
    if (type == Type::Number)
        return number != 0.0;
    if (type == Type::String) {
        if (string == L"true")
            return true;
        if (string == L"false")
            return false;
    }
    return fallback;
}

bool parse(const std::string_view utf8, Value &root, std::string &error)
{
    Parser parser{utf8, 0, {}};
    if (!parser.parseValue(root)) {
        error = parser.error;
        return false;
    }
    parser.skipWhitespace();
    if (parser.pos != parser.text.size()) {
        error = "trailing content after JSON document";
        return false;
    }
    return true;
}

} // namespace cli::json
