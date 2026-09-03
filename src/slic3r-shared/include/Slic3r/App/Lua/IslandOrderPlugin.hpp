#pragma once

#include <string>
#include <vector>

#include "libslic3r/GCode/IslandOrdering.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.island_order` plugin API implemented here.
 *
 * A bundle asks for it through @c required_apis in its manifest.
 */
constexpr auto ISLAND_ORDER_API_VERSION = "1.0.0";

/**
 * @brief Builds an island ordering strategy from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type
 * @c slicing.island_order and returns a strategy that forwards to its
 * @c order_islands() function. Returns an empty strategy when no such plugin is
 * installed, which leaves the stock island order in place.
 *
 * Each call builds its own Lua state, so strategies handed to prints that are
 * exported concurrently never share one. The returned strategy is not itself thread
 * safe and must be used by a single print.
 */
GCode::IslandOrdering::Strategy make_island_order_strategy(
    const std::vector<std::string>& plugin_paths
);

} // namespace Slic3r::App::Lua
