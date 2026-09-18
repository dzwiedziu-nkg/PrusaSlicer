#pragma once

#include <cstddef>
#include <functional>
#include <optional>

/**
 * @brief Extension point for the direction a closed wall loop is walked in.
 *
 * Every perimeter is a closed loop, and the slicer has to pick an end to start from and a way
 * round. Stock PrusaSlicer walks all of them the same way - counter clockwise seen from above,
 * or clockwise for the whole print when the printer's @c prefer_clockwise_movements is set -
 * and that is the only control there is over it.
 *
 * Which way round a particular loop is walked is not always a matter of indifference. The case
 * that motivated the hook is OrcaSlicer's @c overhang_reverse: on a wall that hangs over air,
 * alternating the direction on every other layer leaves the stresses in consecutive layers
 * pulling against each other rather than all the same way, which measurably improves steep
 * overhangs and reduces warping in materials that shrink. The same mechanism serves a plugin
 * that walks a tall thin feature the other way every layer to keep it from leaning, one that
 * matches a hole's direction to its contour's where the seam shows, and one that picks the
 * direction that starts nearest the head to save a travel.
 *
 * A Strategy installed on the Print (Biz::Slicing::IPrint::loop_direction_strategy) is offered
 * every closed perimeter loop as it is about to be written, and answers with the direction it
 * should be walked in or with nothing. With no strategy installed nothing is asked and the
 * G-code is bit for bit the stock G-code.
 *
 * The hook is deliberately not "reverse on even layers". It asks which way round *this* loop
 * goes, and the interesting part - on which layers, for which walls, and on what evidence - is
 * a property of the part and of the material rather than of the slicer.
 *
 * Note that this is the direction the nozzle travels, not the orientation the loop is stored
 * with: the engine reverses the stored loop when the two disagree, exactly as it already does
 * for @c prefer_clockwise_movements, and the seam, the wipe and the smoothed path all follow.
 *
 * Threading: called from the G-code layer generator, a tbb::filter_mode::serial_in_order
 * pipeline stage, so calls belonging to one Print are serialized and arrive in layer order -
 * which makes it safe to delegate to a runtime that is not thread safe, and lets a strategy
 * carry state from one layer to the next. Several Prints may be exported at once, so a
 * strategy must not be shared between them unless it is thread safe.
 */
namespace Slic3r::GCode::LoopDirection {

/** @brief Which way round a loop is walked, seen from above. */
enum class Direction
{
    Cw,
    Ccw
};

/** @brief One closed wall loop, as it is about to be written to G-code. */
struct LoopInfo
{
    std::size_t layer_id;
    /** @brief Height of the top of this layer, in mm. */
    double print_z;
    unsigned extruder_id;
    /** @brief True for the loop that shows on the outside of the part. */
    bool external;
    /**
     * @brief True when the loop runs round a hole rather than round the outside.
     *
     * Taken from the orientation the loop is stored with, which is the same test the stock
     * G-code writer makes.
     */
    bool hole;
    /** @brief 0 is the outermost wall and the count goes inward; -1 when the slicer recorded
     *         none, which happens for a thin wall the generator could not place in the stack. */
    int perimeter_index;
    /** @brief Length of the loop, in mm. */
    double length;
    /**
     * @brief How much of this loop lies outside the outline of the layer below, in mm.
     *
     * Measured against that outline as it is, with no allowance for the width of the bead -
     * which is the same test OrcaSlicer's @c overhang_reverse makes at its default threshold,
     * where their @c threshold-0.5*width works out to exactly zero.
     *
     * Deliberately **not** the slicer's own overhang perimeter marking. That compares the loop
     * against the layer below grown by half a nozzle, so it says no on any slope the nozzle
     * still partly overlaps: on a 31 degree wall at 0.2 mm layers the outline steps out 0.33 mm
     * a layer, the grown lower layer reaches 0.2 mm further than the real one, and nothing is
     * ever marked - while OrcaSlicer reverses every layer of it. It is also only computed at
     * all when @c overhangs is enabled in the print settings, and this must not depend on that.
     *
     * Note that it is a *length*, not a depth. A plugin wanting OrcaSlicer's non-default
     * thresholds, which move the outline the loop is compared against, cannot have them here.
     */
    double overhang_length;
    /**
     * @brief The same, summed over every loop of the island this one belongs to.
     *
     * Offered because the decision is usually about the island and not about the loop: a wall
     * stack whose outermost loop hangs over air wants all of its loops turned together, and a
     * plugin asked one loop at a time could not otherwise know.
     */
    double island_overhang_length;
    /** @brief The direction the engine would walk this loop in if nobody said otherwise. */
    Direction stock;
};

using Strategy = std::function<std::optional<Direction>(const LoopInfo& loop)>;

/**
 * @brief Runs @p strategy over @p loop and validates the answer.
 *
 * Returns nothing - meaning walk the loop the way the engine chose - when no strategy is
 * installed, when the strategy declines, and when it answers with the direction it was already
 * handed. That last case is what keeps a plugin that agrees with the slicer from sending it
 * down an override path for no reason.
 */
std::optional<Direction> plan_direction(const Strategy& strategy, const LoopInfo& loop);

} // namespace Slic3r::GCode::LoopDirection
