#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace mcoverlay::bedwars {

// One canonical representation is shared by Sidebar parsing, bed ownership,
// player classification and rendering. The enum deliberately does not expose
// Minecraft formatting codes as business state.
enum class Team : std::uint8_t {
    Unknown,
    Red,
    Blue,
    Green,
    Yellow,
    Aqua,
    White,
    Pink,
    Gray
};

struct SidebarSnapshot final {
    std::uint16_t teamsMask = 0U;
    Team ownTeam = Team::Unknown;
    std::uint8_t distinctTeams = 0U;
    std::uint8_t youRows = 0U;
    bool valid = false;
};

enum class ThreatClassification : std::uint8_t {
    Teammate,
    Enemy,
    UnknownThreat
};

[[nodiscard]] char formatCode(Team team) noexcept;
[[nodiscard]] Team fromFormatCode(char code) noexcept;
[[nodiscard]] Team fromWoolMetadata(std::uint8_t metadata) noexcept;
[[nodiscard]] Team fromLeatherRgb(std::uint32_t rgb) noexcept;
[[nodiscard]] std::uint8_t teamIndex(Team team) noexcept;

// Parses the final Sidebar strings after ScorePlayerTeam prefix/suffix have
// been combined. It is language independent: only the active formatting
// colour, the one-letter team marker and a word-bounded YOU marker matter.
[[nodiscard]] SidebarSnapshot parseSidebar(
    std::span<const std::string_view> formattedLines) noexcept;

// Player display names use a different shape, normally "§c[R] name".
// parseRosterTeamTag accepts only a whitelisted explicit Bed Wars team tag and
// deliberately tolerates rank/reset colours inserted by transformed clients.
// parseRosterTeam additionally falls back to the first team colour, but only
// after a match has already been confirmed.
[[nodiscard]] Team parseRosterTeamTag(std::string_view formattedName) noexcept;
[[nodiscard]] Team parseRosterTeam(std::string_view formattedName) noexcept;
[[nodiscard]] ThreatClassification classifyArmorThreat(
    Team ownTeam, bool chestplatePresent, Team armorTeam) noexcept;

} // namespace mcoverlay::bedwars
