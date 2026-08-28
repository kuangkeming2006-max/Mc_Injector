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

Team rosterTagTeam(const char colour, const std::string_view tag) noexcept
{
    // Hypixel's live roster component is not guaranteed to keep the team
    // colour active immediately before the short [R]/[B]/... token. Lunar and
    // several HUD transformers insert a reset/rank colour in that position.
    // The original detector therefore classified the explicit tag itself and
    // did not require the surrounding colour to agree. Keep that behaviour:
    // an explicit, whitelisted Bed Wars tag is safe evidence, while unrelated
    // rank tags such as [MVP+] still fail closed.
    if (tag.size() == 1U) {
        const char marker = static_cast<char>(std::toupper(
            static_cast<unsigned char>(tag.front())));
        switch (marker) {
        case 'R': return Team::Red;
        case 'B': return Team::Blue;
        case 'G':
            // Hypixel historically used S for Gray; accept a gray-coloured G
            // variant without making an ordinary G ambiguous.
            return fromFormatCode(colour) == Team::Gray ? Team::Gray : Team::Green;
        case 'Y': return Team::Yellow;
        case 'A': return Team::Aqua;
        case 'W': return Team::White;
        case 'P': return Team::Pink;
        case 'S': return Team::Gray;
        default: return Team::Unknown;
        }
    }
    std::array<char, 7U> upper{};
    if (tag.empty() || tag.size() >= upper.size()) return Team::Unknown;
    for (std::size_t index = 0U; index < tag.size(); ++index) {
        upper[index] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(tag[index])));
    }
    const std::string_view normalized(upper.data(), tag.size());
    if (normalized == "RED") return Team::Red;
    if (normalized == "BLUE") return Team::Blue;
    if (normalized == "GREEN") return Team::Green;
    if (normalized == "YELLOW") return Team::Yellow;
    if (normalized == "AQUA") return Team::Aqua;
    if (normalized == "WHITE") return Team::White;
    if (normalized == "PINK") return Team::Pink;
    if (normalized == "GRAY" || normalized == "GREY") return Team::Gray;
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

Team fromLeatherRgb(const std::uint32_t rgb) noexcept
{
    const int red = static_cast<int>((rgb >> 16U) & 0xFFU);
    const int green = static_cast<int>((rgb >> 8U) & 0xFFU);
    const int blue = static_cast<int>(rgb & 0xFFU);
    if (red > green * 2 && red > blue * 2) return Team::Red;
    if (blue * 2 > red * 3 && blue * 2 > green * 3) return Team::Blue;
    if (green * 2 > red * 3 && green * 2 > blue * 3) return Team::Green;
    if (red > blue * 2 && green > blue * 2 && red > 150 && green > 150)
        return Team::Yellow;
    if (green * 2 > red * 3 && blue * 2 > red * 3 && green > 100 && blue > 100)
        return Team::Aqua;
    if (red > 200 && green > 200 && blue > 200) return Team::White;
    if (red > 150 && blue > 150 && green < 150) return Team::Pink;
    if (red < 100 && green < 100 && blue < 100) return Team::Gray;
    return Team::Unknown;
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

Team parseRosterTeamTag(const std::string_view formattedName) noexcept
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
        if (close == std::string_view::npos || close - offset < 2U ||
            close - offset > 7U) continue;
        const Team team = rosterTagTeam(
            activeColour, formattedName.substr(offset + 1U, close - offset - 1U));
        if (team != Team::Unknown) return team;
    }
    return Team::Unknown;
}

Team parseRosterTeam(const std::string_view formattedName) noexcept
{
    const Team tagged = parseRosterTeamTag(formattedName);
    if (tagged != Team::Unknown) return tagged;
    Team firstRecognizedColour = Team::Unknown;
    for (std::size_t offset = 0U; offset < formattedName.size(); ++offset) {
        if (!isSection(formattedName, offset)) continue;
        const char code = static_cast<char>(std::tolower(
            static_cast<unsigned char>(formattedName[offset + 2U])));
        if (isColourCode(code)) {
            firstRecognizedColour = fromFormatCode(code);
            break;
        }
        offset += 2U;
    }
    // Some transformed clients omit the short [R]/[B] marker and colour the
    // complete player name instead. This fallback is only consumed after the
    // Sidebar match gate is already stable, so rank colours in a lobby cannot
    // accidentally start team detection.
    return firstRecognizedColour;
}

ThreatClassification classifyArmorThreat(const Team ownTeam,
                                          const bool chestplatePresent,
                                          const Team armorTeam) noexcept
{
    if (!chestplatePresent || armorTeam == Team::Unknown ||
        ownTeam == Team::Unknown) {
        return ThreatClassification::UnknownThreat;
    }
    return armorTeam == ownTeam ? ThreatClassification::Teammate
                                : ThreatClassification::Enemy;
}

} // namespace mcoverlay::bedwars
