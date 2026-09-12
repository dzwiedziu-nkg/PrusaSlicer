#pragma once

#include <string>
#include <vector>

#include "libslic3r/Fill/FillPlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.fill_planner` plugin API implemented here.
 */
constexpr auto FILL_PLANNER_API_VERSION = "1.2.0";

/**
 * @brief Builds a fill planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.fill_planner
 * and returns a strategy that forwards to its @c plan_fill() function. Returns an empty
 * strategy when no such plugin is installed, which keeps every stock fill path.
 *
 * Unlike the G-code hooks this one is called from Layer::make_fills(), which runs
 * concurrently over layers, so the returned strategy serializes access to its Lua state
 * with a mutex. It is therefore safe to call from several threads, but it is a
 * serialization point: a plugin that answers for every surface of a large print will
 * hold up the parallel infill stage.
 */
FillPlanner::Strategy make_fill_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
