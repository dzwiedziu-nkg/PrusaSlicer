#pragma once

#include <string>
#include <vector>

#include "libslic3r/GCode/IslandSequencing.hpp"

namespace Slic3r::App::Lua {

constexpr auto ISLAND_SEQUENCE_API_VERSION = "1.0.0";

/** @brief Builds the strategy supplied by a slicing.island_sequence plugin. */
GCode::IslandSequencing::Strategy make_island_sequence_strategy(
    const std::vector<std::string>& plugin_paths
);

} // namespace Slic3r::App::Lua
