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
    /** @brief Distance between two adjacent fill lines, in mm. */
    double spacing;
    /** @brief Direction the slicer chose, in radians. Negative when not a bridge. */
    double bridge_angle;
};

/**
 * @brief Answers with the paths to extrude, or nothing to keep the slicer's own.
 *
 * Coordinates are scaled, in the same frame as SurfaceInfo::region.
 */
using Strategy = std::function<std::optional<Domain::Polylines>(const SurfaceInfo& surface)>;

/**
 * @brief Runs @p strategy over @p surface, falling back to @p stock.
 *
 * Returns @p stock unchanged when no strategy is installed, when the strategy declines,
 * and whenever the answer is unusable: empty, or reaching outside SurfaceInfo::region.
 */
Domain::Polylines plan_fill(
    const Strategy& strategy, const SurfaceInfo& surface, Domain::Polylines stock
);

} // namespace Slic3r::FillPlanner
