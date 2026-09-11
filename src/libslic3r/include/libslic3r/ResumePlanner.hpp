#pragma once

#include <cstddef>
#include <functional>
#include <optional>

/**
 * @brief Extension point for getting the nozzle back into a known state after an interruption.
 *
 * A print can be stopped in the middle by things the slicer puts there on purpose: a pause to
 * drop a nut into a pocket, a colour change, a tool change. While it is stopped the nozzle is
 * hot and still, and it drips. What comes back is a melt of unknown temperature, unknown
 * pressure and unknown volume, and the slicer's answer to that is the prime in
 * @c color_change_gcode - 0.3 mm of filament, which is 0.72 mm3 against a melt zone of fifteen
 * to forty.
 *
 * Two things follow on the print. The first extrusions come out starved, which shows on a
 * small object because the external perimeter is reached before the flow has settled. And
 * whatever dripped lands on the first thing the nozzle touches, which - since the deferred
 * custom G-code is emitted after the travel, at the first extrusion point - is a wall.
 *
 * What to do about it is a property of the part and of the printer, not of the slicer: how
 * much plastic is enough depends on the hot end, and whether it is worth the filament depends
 * on how visible the first perimeter is. So a Strategy installed on the Print
 * (Biz::Slicing::IPrint::resume_planner) is offered every interruption, and returning
 * std::nullopt does nothing at all, leaving the output exactly as the stock slicer's.
 *
 * The hook is deliberately not "purge after a pause". A strategy may equally decide to spend
 * nothing on a large object whose first perimeter is hidden, or to ask for more when the
 * interruption is a colour change and the old colour has to be driven out. What the slicer
 * offers is the moment and the room; what to do with it is the plugin's.
 *
 * Threading: called from G-code generation, one layer at a time, on one thread.
 */
namespace Slic3r::ResumePlanner {

/** @brief What stopped the print. */
enum class Interruption { Pause, ColorChange, ToolChange, Template, Custom };

/** @brief One interruption, and the layer the print resumes on. */
struct ResumeInfo
{
    Interruption kind;
    std::size_t layer_id;
    /** @brief Height of the top of the layer the print resumes on, in mm. */
    double print_z;
    unsigned extruder_id;
    double layer_height;
    double nozzle_diameter;
    /**
     * @brief Room the layer's own sparse infill leaves, in mm2.
     *
     * Where a purge can go without standing proud of the layer: inside the part, at this Z,
     * needing no tower and no space on the bed. Zero on a layer that is solid throughout, and
     * a strategy that asks for more than fits simply gets what fits.
     */
    double spare_area;
    /**
     * @brief How much plastic that room would hold at the layer's own flow, in mm3.
     *
     * The same quantity as @ref spare_area seen the way a strategy wants it, so that asking
     * for "a melt zone, if there is room" does not need the plugin to know the flow.
     */
    double spare_volume;
};

/** @brief What to do before the layer's own work starts. */
struct Plan
{
    /**
     * @brief Plain extrusion to put through the nozzle first, in mm3.
     *
     * Laid in the room the layer's own infill leaves, and made the layer's *first* extrusion,
     * which is the point of it: the printer then comes back from the interruption to the purge
     * patch instead of to a perimeter, drips there, purges there, and only then goes to work.
     * A melt zone is 15-40 mm3, which is the scale at which this replaces what was in the
     * nozzle. More than the layer has room for gives what it has.
     */
    double purge_volume{0.};
};

using Strategy = std::function<std::optional<Plan>(const ResumeInfo& resume)>;

/**
 * @brief Runs @p strategy over @p resume and validates the answer.
 *
 * Returns nothing - meaning resume exactly as the stock slicer would - when no strategy is
 * installed, when the strategy declines, and when it asks for nothing usable.
 */
std::optional<Plan> plan_resume(const Strategy& strategy, const ResumeInfo& resume);

} // namespace Slic3r::ResumePlanner
