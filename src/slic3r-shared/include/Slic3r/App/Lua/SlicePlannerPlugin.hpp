#pragma once

#include <string>
#include <vector>

#include "libslic3r/SlicePlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.slice_planner` plugin API implemented here.
 */
constexpr auto SLICE_PLANNER_API_VERSION = "1.4.0";

/**
 * @brief Builds a slice planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.slice_planner and
 * returns a strategy that forwards to its @c plan_slice() function. Returns an empty strategy
 * when no such plugin is installed, which prints the outline the mesh gives.
 *
 * Layers of one object are offered in order and one at a time, but Print::process() slices the
 * objects of a plate in parallel, so the returned strategy serializes access to its Lua state
 * with a mutex. Two objects and a plugin installed used to be a crash.
 */
SlicePlanner::Strategy make_slice_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
