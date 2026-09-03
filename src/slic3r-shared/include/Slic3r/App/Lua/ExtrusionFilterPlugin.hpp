#pragma once

#include <string>
#include <vector>

#include "libslic3r/GCode/ExtrusionFilter.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.extrusion_filter` plugin API implemented here.
 */
constexpr auto EXTRUSION_FILTER_API_VERSION = "1.0.0";

/**
 * @brief Builds an extrusion filter from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type
 * @c slicing.extrusion_filter and returns a predicate that forwards to its
 * @c keep_extrusion() function. Returns an empty predicate when no such plugin is
 * installed, which keeps every extrusion.
 *
 * Each call builds its own Lua state, so predicates handed to prints that are
 * exported concurrently never share one. The returned predicate is not itself thread
 * safe and must be used by a single print.
 */
GCode::ExtrusionFilter::Predicate make_extrusion_filter(
    const std::vector<std::string>& plugin_paths
);

} // namespace Slic3r::App::Lua
