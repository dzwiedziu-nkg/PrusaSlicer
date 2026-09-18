#pragma once

#include <string>
#include <vector>

#include "libslic3r/GCode/LoopDirection.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.loop_direction` plugin API implemented here.
 */
constexpr auto LOOP_DIRECTION_API_VERSION = "1.0.0";

/**
 * @brief Builds a loop direction strategy from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.loop_direction and
 * returns a strategy that forwards to its @c plan_direction() function. Returns an empty
 * strategy when no such plugin is installed, which walks every loop the way the printer's
 * @c prefer_clockwise_movements says.
 *
 * Loops are written from the serialized G-code stage, in layer order, so the returned strategy
 * needs no lock of its own and a plugin may carry state from one layer to the next.
 */
GCode::LoopDirection::Strategy make_loop_direction(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
