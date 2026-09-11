#pragma once

#include <cstddef>
#include <functional>
#include <optional>

/**
 * @brief Extension point for bounding how far a layer's outline may reach past the one below.
 *
 * Every layer of a sliced object is an outline cut from the mesh at one height, and the slicer
 * prints what the mesh says regardless of whether the printer can hold it up. Where the mesh
 * juts sideways the result is a 90 degree overhang: an extrusion laid onto air, which curls,
 * drags on the nozzle and generally needs support underneath it.
 *
 * The case that motivated the hook is chamfering those overhangs away. Given an angle - the
 * one @c support_material_threshold is expressed in, where 90 degrees is vertical and the
 * number is the most horizontal slope printable without support - a layer's outline may grow
 * by at most @c layer_height/tan(theta) over the layer below, and clipping each layer to that,
 * walking upward, is exactly a chamfer of that angle. It appears only where the model juts
 * out and costs nothing anywhere else.
 *
 * The hook is deliberately not "chamfer overhangs". What it offers is a per-layer bound on the
 * silhouette, and the interesting part of the decision - which angle, at which heights, how
 * much of the part may be given up for it - is a property of the part and of what it is for.
 * The same mechanism serves a plugin that makes only the first few millimetres self supporting
 * so a part can be printed without a brim's worth of support under its skirt, one that
 * tightens the angle towards the top of a tall part where a curled overhang would be hit by
 * the nozzle on the way round, and one that leaves the model alone below a given Z.
 *
 * What it cannot do is add material: a clip only ever removes. A plugin that wants the outline
 * to grow is asking for a different hook.
 *
 * Threading: called from PrintObject::slice(), once per layer, strictly in order from the
 * bottom up, because each layer is measured against the outline the layer below was left with.
 * Unlike the fill, pass and perimeter planners this one is never entered concurrently, so an
 * implementation backed by a runtime that is not thread safe needs no lock of its own.
 */
namespace Slic3r::SlicePlanner {

/** @brief One layer, as it comes off the mesh and before anything is generated from it. */
struct LayerInfo
{
    std::size_t layer_id;
    /** @brief Height of the top of this layer, in mm. */
    double print_z;
    /** @brief Height this layer's outline was cut from the mesh at, in mm. */
    double slice_z;
    double layer_height;
    /** @brief Height of the whole object, in mm, so a plugin can act on a fraction of it. */
    double object_height;
    /** @brief Area of this layer's outline, in mm^2. */
    double area;
    /** @brief How many separate islands this layer's outline falls into. */
    std::size_t islands;
};

/**
 * @brief No bound - the layer's outline is whatever the mesh says, which is the stock slicer.
 *
 * The default, so that a plugin answering with a table that says nothing about @c max_overhang
 * changes nothing. Growth of zero is a legal and very different answer: it means the outline
 * may not widen at all.
 */
constexpr double UNBOUNDED = -1.;

/**
 * @brief Upper bound on both distances, in mm.
 *
 * Not a physical limit but a guard. Either number running away is an arithmetic mistake in the
 * plugin rather than a decision, and a silhouette clipped by a number with no relation to the
 * part is not something that should reach the printer quietly.
 */
constexpr double MAX_DISTANCE = 1000.;

/** @brief What this layer's outline is allowed to be. */
struct Plan
{
    /**
     * @brief How far the outline may reach beyond the layer below, in mm.
     *
     * For a chamfer of theta degrees from horizontal this is @c layer_height/tan(theta). The
     * plugin computes it rather than handing over an angle, because the layer height is not
     * constant and because a bound in millimetres is the more general thing to ask for.
     */
    double max_overhang{UNBOUNDED};

    /**
     * @brief The most material that may be cut away, in mm, or 0 for no limit.
     *
     * Measured as the largest disc that fits inside the piece about to be removed. An overhang
     * too big to fit under it is left alone in one piece, which is what keeps the clip from
     * quietly eating a table top, and - because the piece about to be removed is measured
     * against the outline the layer below was *left* with, not against the mesh - what keeps a
     * slope shallower than theta from being whittled down layer after layer. A slope like that
     * falls at most this far behind the mesh before it is handed back in full.
     *
     * Leaving it at 0 means the clip is free to remove an overhang of any size. That is what a
     * plugin deliberately reshaping a part wants and is the wrong thing for a plugin meant to
     * chamfer an edge.
     */
    double max_overhang_width{0.};
};

using Strategy = std::function<std::optional<Plan>(const LayerInfo& layer)>;

/**
 * @brief Runs @p strategy over @p layer and validates the answer.
 *
 * Returns nothing - meaning leave the outline exactly as the mesh gave it - when no strategy is
 * installed, when the strategy declines, when it leaves @c max_overhang unbounded and when
 * either distance is not a finite number. Distances outside 0 to MAX_DISTANCE are clamped.
 */
std::optional<Plan> plan_slice(const Strategy& strategy, const LayerInfo& layer);

} // namespace Slic3r::SlicePlanner
