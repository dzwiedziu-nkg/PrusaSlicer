#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "Slic3r/Domain/ExPolygon.hpp"
#include "Slic3r/Domain/GCodeExtrusionRole.hpp"
#include "Slic3r/Domain/Polyline.hpp"

/**
 * @brief Extension point that lets the fill paths of one surface be generated elsewhere.
 *
 * The stock fill patterns lay down straight lines in a single direction chosen per
 * surface, which is the right answer for most regions and a poor one for some. The case
 * that motivated this is a bridge over an annular gap: filled with parallel lines, the
 * lines near the inner island become long chords spanning the whole opening, while the
 * same area filled with spokes running from the inner anchor to the outer one has no
 * unsupported span longer than the width of the gap itself.
 *
 * Which of the two is wanted is a property of the part, not of the slicer, so a Strategy
 * installed on the Print (Biz::Slicing::IPrint::fill_planner) is offered every surface
 * before its paths are handed on. Returning std::nullopt keeps the paths the slicer
 * generated, so with no strategy installed the output is bit for bit the stock output.
 *
 * The region is given in scaled coordinates in the object frame, already grown over its
 * anchors, so paths may - and for bridges should - reach into the surrounding solid.
 * Returned paths are validated to lie inside it; a strategy answering with anything
 * outside has its answer discarded and the stock paths are used instead.
 *
 * Threading: unlike the G-code hooks, this one is called from Layer::make_fills(), which
 * PrintObject::infill() drives from a tbb::parallel_for over layers. A Strategy is
 * therefore called concurrently from several threads at once and must be thread safe.
 * An implementation backed by a runtime that is not thread safe (a Lua state, for
 * instance) has to serialize itself.
 */
namespace Slic3r::FillPlanner {

/** @brief One surface about to be filled. */
struct SurfaceInfo
{
    /** @brief What the resulting extrusions will be. */
    Domain::GCodeExtrusionRole role;
    /** @brief The area to fill, scaled, in the object frame, anchors included. */
    const Domain::ExPolygon& region;
    std::size_t layer_id;
    /** @brief Height of the top of this layer, in mm. */
    double print_z;
    unsigned extruder_id;
    /**
     * @brief Width of the bead the flow was worked out for, in mm.
     *
     * Not the distance between the stock pattern's lines: that is this divided by @c density,
     * and on sparse infill the two are far apart. They coincide on a solid surface, which is
     * where the first planner on this hook happened to work, so the two were confused for one
     * another until a sparse pattern needed them apart.
     */
    double spacing;
    /**
     * @brief How much of the surface the fill covers, 0 to 1.
     *
     * 1 on a solid, top or bridge surface; `fill_density` on sparse infill. A planner laying
     * its own lines wants @c spacing/density between them to put down what the slicer meant.
     */
    double density;
    /** @brief Direction the slicer chose, in radians. Negative when not a bridge. */
    double bridge_angle;
};

/** @brief How far from the slicer's own flow a strategy is allowed to ask to go. */
constexpr double MIN_FLOW_RATIO = 0.05;
constexpr double MAX_FLOW_RATIO = 5.;

/** @brief How one surface is to be filled. */
struct Plan
{
    /** @brief The paths to extrude, scaled, in the frame of SurfaceInfo::region. */
    Domain::Polylines paths;
    /**
     * @brief Multiplies the extrusion the slicer computed for this surface.
     *
     * The flow follows from the line spacing: the slicer works out how much plastic a
     * millimetre of path has to carry to cover the surface at SurfaceInfo::spacing. A
     * strategy that lays its paths out at some other spacing - a fan of spokes has no
     * single spacing at all - puts down the wrong amount unless the flow follows, and
     * only the strategy knows by how much. 1.0 keeps the flow the slicer computed;
     * anything outside MIN_FLOW_RATIO..MAX_FLOW_RATIO is clamped into it.
     */
    double flow_ratio{1.};
};

/**
 * @brief Answers with how to fill the surface, or nothing to keep the slicer's own paths.
 *
 * Coordinates are scaled, in the same frame as SurfaceInfo::region.
 */
using Strategy = std::function<std::optional<Plan>(const SurfaceInfo& surface)>;

/**
 * @brief Runs @p strategy over @p surface, falling back to @p stock.
 *
 * Returns @p stock at the stock flow when no strategy is installed, when the strategy
 * declines, and whenever the answer is unusable: empty, or reaching outside
 * SurfaceInfo::region.
 */
Plan plan_fill(const Strategy& strategy, const SurfaceInfo& surface, Domain::Polylines stock);

} // namespace Slic3r::FillPlanner
