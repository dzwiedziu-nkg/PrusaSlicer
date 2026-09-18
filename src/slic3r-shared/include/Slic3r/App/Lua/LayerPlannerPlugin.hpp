#pragma once

#include <string>
#include <vector>

#include "libslic3r/GCode/LayerPlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.layer_planner` plugin API implemented here.
 */
constexpr auto LAYER_PLANNER_API_VERSION = "1.0.0";

/**
 * @brief Builds a layer planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.layer_planner and
 * returns a strategy that forwards to its @c plan_layer() function. Returns an empty strategy
 * when no such plugin is installed, which prints each layer the way the settings arrange it.
 *
 * Reached from the G-code generator, a serial_in_order pipeline stage, so calls belonging to
 * one export are serialized and arrive in layer order: the returned strategy needs no lock, and
 * a plugin may carry state from one layer to the next.
 */
GCode::LayerPlanner::Strategy make_layer_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
