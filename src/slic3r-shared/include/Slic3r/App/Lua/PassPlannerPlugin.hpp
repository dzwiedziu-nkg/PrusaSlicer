#pragma once

#include <string>
#include <vector>

#include "libslic3r/PassPlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.pass_planner` plugin API implemented here.
 */
constexpr auto PASS_PLANNER_API_VERSION = "1.0.0";

/**
 * @brief Builds a pass planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.pass_planner and
 * returns a strategy that forwards to its @c plan_pass() function. Returns an empty
 * strategy when no such plugin is installed, which runs no extra pass anywhere.
 *
 * Like the fill planner and unlike the G-code hooks this one is called from a parallel
 * stage of slicing, so the returned strategy serializes access to its Lua state with a
 * mutex. It is therefore safe to call from several threads, but it is a serialization
 * point.
 */
PassPlanner::Strategy make_pass_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
