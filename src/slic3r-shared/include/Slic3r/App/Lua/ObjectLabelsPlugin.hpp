#pragma once

#include <string>
#include <vector>

#include "libslic3r/GCode/ObjectLabels.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.object_labels` plugin API implemented here.
 */
constexpr auto OBJECT_LABELS_API_VERSION = "1.0.0";

/**
 * @brief Builds an object label strategy from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.object_labels
 * and returns a strategy that forwards to its @c label_region() function. Returns an
 * empty strategy when no such plugin is installed, which leaves every part of the print
 * that is not a model object unnamed, exactly as the stock slicer does.
 *
 * Each call builds its own Lua state, so strategies handed to prints that are exported
 * concurrently never share one. The returned strategy is not itself thread safe and
 * must be used by a single print.
 */
GCode::ObjectLabels::Strategy make_object_labels(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
