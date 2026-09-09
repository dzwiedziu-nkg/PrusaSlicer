#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "Slic3r/Domain/ExPolygon.hpp"
#include "Slic3r/Domain/GCodeExtrusionRole.hpp"
#include "Slic3r/Domain/Polyline.hpp"

/**
 * @brief Extension point that lets an extra pass be run over an area already covered.
 *
 * Some surfaces are worth going over a second time at the same height, with the nozzle
 * barely extruding, to melt the ridges between the extrusions flat. The slicer already
 * does this for the top of an object, where it is called ironing. The case that motivated
 * the hook is the other one: the top of a support interface, which is the mould the
 * underside of the overhang above it is cast in - every ridge in the interface is copied
 * into the finished part.
 *
 * Which areas deserve the second pass, in what pattern and at what flow is a property of
 * the part and of what the print is for, not of the slicer, so a Strategy installed on the
 * Print (Biz::Slicing::IPrint::pass_planner) is offered every area as it is covered.
 * Returning std::nullopt runs no extra pass, so with no strategy installed the output is
 * bit for bit the stock output.
 *
 * The hook is deliberately not "iron a support interface". The same mechanism serves top
 * surface ironing in a pattern the slicer does not offer, a polishing pass over a bridge,
 * and a second layer interlocked with the first over a field of sparse extrusions.
 * SurfaceInfo says what covered the area and whether anything will be printed onto it, and
 * the strategy decides from that whether the area is one it cares about.
 *
 * The region is given in scaled coordinates in the object frame, already inset by half a
 * nozzle so a path running along its boundary still lands on material. Returned paths are
 * clipped to it, and an answer that lies entirely outside is discarded.
 *
 * Threading: like FillPlanner and unlike the G-code hooks, this one is called from the
 * parallel stages of slicing - generate_support_toolpaths() drives it from a
 * tbb::parallel_for over support layers. A Strategy is therefore called concurrently from
 * several threads at once and must be thread safe. An implementation backed by a runtime
 * that is not thread safe (a Lua state, for instance) has to serialize itself.
 */
namespace Slic3r::PassPlanner {

/** @brief One area the slicer has just covered. */
struct SurfaceInfo
{
    /** @brief What covered the area. */
    Domain::GCodeExtrusionRole role;
    /**
     * @brief The area that was covered, scaled, in the object frame.
     *
     * Already inset by half a nozzle diameter, so a pass may run right along the boundary
     * without hanging off the edge of what it is ironing.
     */
    const Domain::ExPolygon& region;
    std::size_t layer_id;
    /** @brief Height of the top of this layer, in mm. */
    double print_z;
    unsigned extruder_id;
    /** @brief Distance between two adjacent covering extrusions, in mm. */
    double spacing;
    /** @brief Width of the covering extrusions, in mm. */
    double extrusion_width;
    /** @brief Direction the covering extrusions run in, in radians. */
    double angle;
    /** @brief Height of the covering extrusions, in mm. */
    double layer_height;
    double nozzle_diameter;
    /**
     * @brief Whether another part of the print will be laid directly onto this area.
     *
     * True for the top of a support interface, which the object is printed against, and
     * false for the top of the object itself, where the pass is the last thing to touch
     * the surface. The distinction is what a strategy smoothing a mould filters on.
     */
    bool object_above;
};

/**
 * @brief How much of a full layer of material an extra pass may lay down.
 *
 * A pass over material that is already there adds to it, so more than a full layer at the
 * pass's own line spacing is over-extrusion however the paths are laid out; zero is a
 * pass that only reheats.
 */
constexpr double MIN_FLOW_RATIO = 0.;
constexpr double MAX_FLOW_RATIO = 1.;

/** @brief The flow the slicer's own ironing uses, and what a plan gets if it says nothing. */
constexpr double DEFAULT_FLOW_RATIO = 0.15;

/** @brief What an extra pass over one area consists of. */
struct Plan
{
    /** @brief The paths to extrude, scaled, in the frame of SurfaceInfo::region. */
    Domain::Polylines paths;
    /**
     * @brief Distance between two adjacent paths, in mm.
     *
     * Only the strategy knows it - a pass that spirals or fans out has no single spacing
     * at all - and the flow follows from it, so a plan whose paths are twice as close
     * together as it says will extrude twice as much as it meant to. 0 means the paths are
     * spaced like the extrusions they cover, SurfaceInfo::spacing.
     */
    double spacing{0.};
    /**
     * @brief Fraction of a full layer of material to lay down, MIN_ to MAX_FLOW_RATIO.
     *
     * The same quantity as the slicer's own @c ironing_flowrate: 1.0 deposits as much as a
     * normal extrusion at Plan::spacing would, which over material that is already there
     * doubles it. Anything outside the range is clamped into it.
     */
    double flow_ratio{DEFAULT_FLOW_RATIO};
};

/** @brief The extrusion an extra pass is laid down with, in mm and mm3/mm. */
struct PassFlow
{
    double mm3_per_mm;
    double width;
    double height;
};

/**
 * @brief Answers with the extra pass to run over the area, or nothing to run none.
 *
 * Coordinates are scaled, in the same frame as SurfaceInfo::region.
 */
using Strategy = std::function<std::optional<Plan>(const SurfaceInfo& surface)>;

/**
 * @brief Runs @p strategy over @p surface and validates the answer.
 *
 * Returns nothing - meaning no extra pass at all - when no strategy is installed, when the
 * strategy declines, and whenever the answer is unusable: empty, or lying outside
 * SurfaceInfo::region. Paths reaching over the boundary are clipped to it rather than
 * rejected, the way the stock fillers have their lines clipped.
 */
std::optional<Plan> plan_pass(const Strategy& strategy, const SurfaceInfo& surface);

/**
 * @brief Works out the extrusion a validated @p plan is laid down with.
 *
 * The same arithmetic the slicer's own ironing uses: a pass at Plan::flow_ratio of a full
 * layer, squeezed out of a nozzle of SurfaceInfo::nozzle_diameter over lines Plan::spacing
 * apart. Callers convert paths to extrusions with it so that every extra pass in the
 * slicer is fed the same way.
 */
PassFlow pass_flow(const SurfaceInfo& surface, const Plan& plan);

} // namespace Slic3r::PassPlanner
