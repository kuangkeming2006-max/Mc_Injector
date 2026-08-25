#pragma once

#include <string_view>

namespace mcoverlay::log {

void info(std::string_view message) noexcept;
void error(std::string_view message) noexcept;

} // namespace mcoverlay::log

