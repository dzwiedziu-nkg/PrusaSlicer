#pragma once

#include <cstddef>
#include <functional>
#include <optional>

/**
 * @brief Extension point for deciding what walls a layer's region gets.
 *
 * Two decisions, because they are made in the same place and from the same knowledge: how many
 * perimeters there are, and whether the part of the region that hangs over air gets any at all.
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
 * The second decision arrived with counterbore holes. Where a hole narrows, the ring of material
 * left round the smaller hole is walled in mid-air - both loops of it, going round nothing -
 * although the ring itself is bridged. A plugin may ask for that part to be cut out of what the
 * perimeter generator is given and handed to the fill stage instead, with a band of held-up
 * material beside it to anchor the bridge on. See Unsupported.
 *
 * The hook is deliberately not "add a wall on alternate layers", nor "bridge a counterbore".
 * The same mechanism serves more wall through a band that will be tapped or threaded, more wall
 * near the top and bottom faces where the load goes, fewer wall in a tall thin feature that is
 * only there for looks, more wall where a support is going to be prised off - and, on the other
 * question, leaving a thin island to the fill rather than walling it, or keeping the wall off
 * an area a later pass is going to iron.
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
 * @brief What happens to the part of a region that hangs over air.
 *
 * The slicer walls a region whether or not the layer below holds it up, so where a hole narrows
 * - a counterbore is the case - the ring left round the smaller hole is walled in mid-air, both
 * loops of it, going round nothing. The material is bridged either way; it is the wall that has
 * nowhere to rest.
 */
enum class Unsupported
{
    /** @brief Wall it like anything else. The stock slicer, and the default. */
    Wall,
    /**
     * @brief Where the unsupported material touches a hole of the region, leave it to the fill.
     *
     * The area is cut out of what the perimeter generator is given and handed to the fill stage
     * instead, with a band of the supported material beside it so the bridge has something to
     * be anchored on. The wall then runs round the outside of that band, on material that is
     * held up. This is OrcaSlicer's @c counterbore_hole_bridging in its partially bridged mode.
     */
    FillHoles,
    /** @brief The same, wherever a region hangs over air, hole or no hole. */
    FillAll
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

/** @brief Upper bound on the anchor and the minimum width, in mm. A guard, not a limit. */
constexpr double MAX_ANCHOR = 100.;

/** @brief What to generate on this layer instead. */
struct Plan
{
    int perimeters{0};

    /** @brief What to do with the part of the region that hangs over air. */
    Unsupported unsupported{Unsupported::Wall};

    /**
     * @brief How far the fill may reach into held-up material for an anchor, in mm.
     *
     * 0 asks the slicer for its own answer, which is one perimeter spacing. A bridge whose ends
     * rest on nothing is no better than the wall this removes, so the band is not optional -
     * only its width is.
     */
    double unsupported_anchor{0.};

    /**
     * @brief Ignore unsupported pieces narrower than this, in mm.
     *
     * 0 asks the slicer for its own answer, which is one perimeter spacing. Without it every
     * sliver along a sloping wall would be cut out of the wall stage, which is a lot of
     * clipping for nothing.
     */
    double min_unsupported{0.};
};

using Strategy = std::function<std::optional<Plan>(const RegionInfo& region)>;

/**
 * @brief Runs @p strategy over @p region and validates the answer.
 *
 * Returns nothing - meaning use the count the settings ask for and wall everything - when no
 * strategy is installed, when the strategy declines, and when it answers with the count it was
 * already given and nothing else. A count outside MIN_ to MAX_PERIMETERS is clamped into it, and
 * so are the two distances, into 0..MAX_ANCHOR.
 */
std::optional<Plan> plan_perimeters(const Strategy& strategy, const RegionInfo& region);

} // namespace Slic3r::PerimeterPlanner
