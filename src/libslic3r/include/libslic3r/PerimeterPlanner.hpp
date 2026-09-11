#pragma once

#include <cstddef>
#include <functional>
#include <optional>

/**
 * @brief Extension point for deciding how many perimeters a layer gets.
 *
 * The wall count is a print setting, one number for the whole object, and that is the wrong
 * shape for several things people actually want. The case that motivated the hook is
 * OrcaSlicer's @c alternate_extra_wall: one extra wall on every other layer, so that the
 * infill's anchor points alternate between two radii and the infill ends up wedged vertically
 * between walls rather than always meeting the same seam. Measured on OrcaSlicer's own output,
 * that costs about 11 % more material on the layers that get the extra wall.
 *
 * Which layers deserve more wall is a property of the part and of what it is for, not of the
 * slicer, so a Strategy installed on the Print (Biz::Slicing::IPrint::perimeter_planner) is
 * offered every layer before its perimeters are generated. Returning std::nullopt leaves the
 * count the settings asked for, so with no strategy installed the output is bit for bit the
 * stock output.
 *
 * The hook is deliberately not "add a wall on alternate layers". The same mechanism serves
 * more wall through a band that will be tapped or threaded, more wall near the top and bottom
 * faces where the load goes, fewer wall in a tall thin feature that is only there for looks,
 * and more wall where a support is going to be prised off.
 *
 * Threading: called from the parallel stage that generates perimeters -
 * PrintObject::make_perimeters() drives it from a tbb::parallel_for over layers. A Strategy is
 * therefore called concurrently from several threads at once and must be thread safe. An
 * implementation backed by a runtime that is not thread safe (a Lua state, for instance) has
 * to serialize itself.
 */
namespace Slic3r::PerimeterPlanner {

/** @brief One layer's region, as it is about to have its perimeters generated. */
struct RegionInfo
{
    std::size_t layer_id;
    /** @brief Height of the top of this layer, in mm. */
    double print_z;
    double layer_height;
    unsigned extruder_id;
    /** @brief How many perimeters the settings ask for here. */
    int perimeters;
    /** @brief Width of a perimeter extrusion, in mm. */
    double perimeter_width;
    /** @brief Distance between two adjacent perimeters, in mm. */
    double perimeter_spacing;
    double nozzle_diameter;
};

/**
 * @brief How many perimeters a layer may be given.
 *
 * Zero is a layer with no wall at all, which the slicer already allows. The upper bound is not
 * a physical limit but a guard: a count that runs away fills the part with wall and takes the
 * slice with it, and a plugin arriving at one has made an arithmetic mistake rather than a
 * decision.
 */
constexpr int MIN_PERIMETERS = 0;
constexpr int MAX_PERIMETERS = 100;

/** @brief What to generate on this layer instead. */
struct Plan
{
    int perimeters{0};
};

using Strategy = std::function<std::optional<Plan>(const RegionInfo& region)>;

/**
 * @brief Runs @p strategy over @p region and validates the answer.
 *
 * Returns nothing - meaning use the count the settings ask for - when no strategy is installed,
 * when the strategy declines, and when it answers with the count it was already given. A count
 * outside MIN_ to MAX_PERIMETERS is clamped into it.
 */
std::optional<Plan> plan_perimeters(const Strategy& strategy, const RegionInfo& region);

} // namespace Slic3r::PerimeterPlanner
