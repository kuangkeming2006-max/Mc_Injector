#include "BedWarsState.h"

#include <array>
#include <cctype>

namespace mcoverlay::bedwars {
namespace {

constexpr bool isSection(const std::string_view text, const std::size_t offset) noexcept
{
    return offset + 2U < text.size() &&
           static_cast<unsigned char>(text[offset]) == 0xC2U &&
           static_cast<unsigned char>(text[offset + 1U]) == 0xA7U;
}

constexpr bool isColourCode(const char code) noexcept
{
    return (code >= '0' && code <= '9') || (code >= 'a' && code <= 'f');
}

Team markerTeam(const char colour, const char marker) noexcept
{
    const char lowerColour = static_cast<char>(
        std::tolower(static_cast<unsigned char>(colour)));
    const char upperMarker = static_cast<char>(
        std::toupper(static_cast<unsigned char>(marker)));
    if (lowerColour == 'c' && upperMarker == 'R') return Team::Red;
    if (lowerColour == '9' && upperMarker == 'B') return Team::Blue;
    if (lowerColour == 'a' && upperMarker == 'G') return Team::Green;
    if (lowerColour == 'e' && upperMarker == 'Y') return Team::Yellow;
    if (lowerColour == 'b' && upperMarker == 'A') return Team::Aqua;
    if (lowerColour == 'f' && upperMarker == 'W') return Team::White;
    if (lowerColour == 'd' && upperMarker == 'P') return Team::Pink;
    if (lowerColour == '7' && (upperMarker == 'S' || upperMarker == 'G'))
        return Team::Gray;
    return Team::Unknown;
}

bool containsYouWord(const std::string_view line) noexcept
{
    constexpr std::string_view token{"YOU"};
    std::array<char, 256U> visible{};
    std::size_t length = 0U;
    for (std::size_t offset = 0U; offset < line.size() && length < visible.size(); ++offset) {
        if (isSection(line, offset)) {
            offset += 2U;
            continue;
        }
        visible[length++] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(line[offset])));
    }
    const std::string_view plain(visible.data(), length);
    std::size_t offset = plain.find(token);
    while (offset != std::string_view::npos) {
        const bool leftBoundary = offset == 0U ||
            !std::isalnum(static_cast<unsigned char>(plain[offset - 1U]));
        const std::size_t right = offset + token.size();
        const bool rightBoundary = right == plain.size() ||
            !std::isalnum(static_cast<unsigned char>(plain[right]));
        if (leftBoundary && rightBoundary) return true;
        offset = plain.find(token, offset + 1U);
    }
    return false;
}

Team parseSidebarRow(const std::string_view line) noexcept
{
    char activeColour = '\0';
    std::size_t offset = 0U;
    while (offset < line.size()) {
        if (isSection(line, offset)) {
            const char code = static_cast<char>(std::tolower(
                static_cast<unsigned char>(line[offset + 2U])));
            if (isColourCode(code)) activeColour = code;
            offset += 3U;
            continue;
        }
        const unsigned char current = static_cast<unsigned char>(line[offset]);
        if (std::isspace(current) != 0) {
            ++offset;
            continue;
        }
        // A team marker must be the first visible character after leading
        // formatting. This prevents arbitrary R/B/G letters in translations
        // or status text from being treated as team rows.
        return markerTeam(activeColour, line[offset]);
    }
    return Team::Unknown;
}

} // namespace

char formatCode(const Team team) noexcept
{
    switch (team) {
    case Team::Red: return 'c';
    case Team::Blue: return '9';
    case Team::Green: return 'a';
    case Team::Yellow: return 'e';
    case Team::Aqua: return 'b';
    case Team::White: return 'f';
    case Team::Pink: return 'd';
    case Team::Gray: return '7';
    case Team::Unknown: break;
    }
    return 'u';
}

Team fromFormatCode(const char code) noexcept
{
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(code)))) {
    case 'c': return Team::Red;
    case '9': return Team::Blue;
    case 'a': return Team::Green;
    case 'e': return Team::Yellow;
    case 'b': return Team::Aqua;
    case 'f': return Team::White;
    case 'd': return Team::Pink;
    case '7': return Team::Gray;
    default: return Team::Unknown;
    }
}

Team fromWoolMetadata(const std::uint8_t metadata) noexcept
{
    switch (metadata & 0x0FU) {
    case 14U: return Team::Red;
    case 11U: return Team::Blue;
    case 13U: return Team::Green;
    case 4U: return Team::Yellow;
    case 9U: return Team::Aqua;
    case 0U: return Team::White;
    case 6U: return Team::Pink;
    case 7U: return Team::Gray;
    default: return Team::Unknown;
    }
}

std::uint8_t teamIndex(const Team team) noexcept
{
    const auto value = static_cast<std::uint8_t>(team);
    return value == 0U ? 0xFFU : static_cast<std::uint8_t>(value - 1U);
}

SidebarSnapshot parseSidebar(
    const std::span<const std::string_view> formattedLines) noexcept
{
    SidebarSnapshot result{};
    Team youTeam = Team::Unknown;
    for (const std::string_view line : formattedLines) {
        const Team team = parseSidebarRow(line);
        if (team == Team::Unknown) continue;
        const std::uint8_t index = teamIndex(team);
        if (index < 8U) result.teamsMask |= static_cast<std::uint16_t>(1U << index);
        if (containsYouWord(line)) {
            ++result.youRows;
            youTeam = team;
        }
    }
    for (std::uint16_t mask = result.teamsMask; mask != 0U; mask >>= 1U)
        result.distinctTeams += static_cast<std::uint8_t>(mask & 1U);
    result.valid = result.distinctTeams >= 2U && result.youRows == 1U;
    result.ownTeam = result.valid ? youTeam : Team::Unknown;
    return result;
}

Team parseRosterTeam(const std::string_view formattedName) noexcept
{
    char activeColour = '\0';
    for (std::size_t offset = 0U; offset < formattedName.size(); ++offset) {
        if (isSection(formattedName, offset)) {
            const char code = static_cast<char>(std::tolower(
                static_cast<unsigned char>(formattedName[offset + 2U])));
            if (isColourCode(code)) activeColour = code;
            offset += 2U;
            continue;
        }
        if (formattedName[offset] != '[' || offset + 2U >= formattedName.size()) continue;
        const std::size_t close = formattedName.find(']', offset + 1U);
        if (close == std::string_view::npos || close - offset != 2U) continue;
        const Team team = markerTeam(activeColour, formattedName[offset + 1U]);
        if (team != Team::Unknown) return team;
    }
    return Team::Unknown;
}

} // namespace mcoverlay::bedwars
