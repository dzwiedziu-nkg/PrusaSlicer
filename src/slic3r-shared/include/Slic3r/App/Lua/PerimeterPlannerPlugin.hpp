#pragma once

#include <string>
#include <vector>

#include "libslic3r/PerimeterPlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.perimeter_planner` plugin API implemented here.
 */
constexpr auto PERIMETER_PLANNER_API_VERSION = "1.0.0";

/**
 * @brief Builds a perimeter planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.perimeter_planner and
 * returns a strategy that forwards to its @c plan_perimeters() function. Returns an empty
 * strategy when no such plugin is installed, which uses the wall count the settings carry.
 *
 * Perimeters are generated from a parallel stage of slicing, so the returned strategy
 * serializes access to its Lua state with a mutex. It is therefore safe to call from several
 * threads, but it is a serialization point.
 */
PerimeterPlanner::Strategy make_perimeter_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
