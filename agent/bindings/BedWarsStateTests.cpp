#include "BedWarsState.h"

#include <array>
#include <cassert>
#include <string_view>

using mcoverlay::bedwars::Team;

int main()
{
    constexpr std::array<std::string_view, 4U> english{
        "\xC2\xA7" "cR Red: ok",
        "\xC2\xA7" "9B Blue: ok YOU",
        "\xC2\xA7" "aG Green: ok",
        "\xC2\xA7" "eY Yellow: ok"};
    const auto englishResult = mcoverlay::bedwars::parseSidebar(english);
    assert(englishResult.valid);
    assert(englishResult.ownTeam == Team::Blue);
    assert(englishResult.distinctTeams == 4U);

    constexpr std::array<std::string_view, 4U> localized{
        "\xC2\xA7" "c\xC2\xA7lR red-localized",
        "\xC2\xA7" "9\xC2\xA7r\xC2\xA7lB blue-localized YOU",
        "\xC2\xA7" "aG green-localized",
        "\xC2\xA7" "eY yellow-localized"};
    const auto localizedResult = mcoverlay::bedwars::parseSidebar(localized);
    assert(localizedResult.valid);
    assert(localizedResult.ownTeam == Team::Blue);

    constexpr std::array<std::string_view, 1U> onlyYou{"unrelated YOU text"};
    assert(!mcoverlay::bedwars::parseSidebar(onlyYou).valid);
    constexpr std::array<std::string_view, 1U> oneTeam{"\xC2\xA7" "cR team"};
    assert(!mcoverlay::bedwars::parseSidebar(oneTeam).valid);
    constexpr std::array<std::string_view, 2U> noYou{
        "\xC2\xA7" "cR team", "\xC2\xA7" "9B team"};
    assert(!mcoverlay::bedwars::parseSidebar(noYou).valid);
    constexpr std::array<std::string_view, 3U> unrelatedYou{
        "\xC2\xA7" "cR team", "\xC2\xA7" "9B team", "plain YOU"};
    assert(!mcoverlay::bedwars::parseSidebar(unrelatedYou).valid);

    assert(mcoverlay::bedwars::parseRosterTeam("\xC2\xA7" "c[R] Alice") == Team::Red);
    assert(mcoverlay::bedwars::parseRosterTeam("\xC2\xA7" "9[B] Bob") == Team::Blue);
    assert(mcoverlay::bedwars::fromWoolMetadata(14U) == Team::Red);
    assert(mcoverlay::bedwars::fromWoolMetadata(11U) == Team::Blue);
    assert(mcoverlay::bedwars::fromWoolMetadata(2U) == Team::Unknown);
    return 0;
}
