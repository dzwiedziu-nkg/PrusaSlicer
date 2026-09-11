#pragma once

#include <string>
#include <vector>

#include "libslic3r/ResumePlanner.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Version of the `slicing.resume_planner` plugin API implemented here.
 */
constexpr auto RESUME_PLANNER_API_VERSION = "1.0.0";

/**
 * @brief Builds a resume planner from the installed slicing plugin.
 *
 * Scans @p plugin_paths for a bundle holding a plugin of type @c slicing.resume_planner and
 * returns a strategy that forwards to its @c plan_resume() function. Returns an empty strategy
 * when no such plugin is installed, which resumes from an interruption exactly as the stock
 * slicer does.
 *
 * Called from G-code generation, which is single threaded, so unlike the pass planner this one
 * needs no lock of its own.
 */
ResumePlanner::Strategy make_resume_planner(const std::vector<std::string>& plugin_paths);

} // namespace Slic3r::App::Lua
