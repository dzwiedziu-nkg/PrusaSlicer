#pragma once

#include <string>
#include <vector>

#include "libslic3r/SlicePlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.slice_planner` plugin API implemented here.
 */
constexpr auto SLICE_PLANNER_API_VERSION = "1.2.0";

/**
 * @brief Builds a slice planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.slice_planner and
 * returns a strategy that forwards to its @c plan_slice() function. Returns an empty strategy
 * when no such plugin is installed, which prints the outline the mesh gives.
 *
 * Layers are offered to the planner one at a time and never concurrently, so unlike the fill,
 * pass and perimeter planners the returned strategy needs no lock.
 */
SlicePlanner::Strategy make_slice_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
