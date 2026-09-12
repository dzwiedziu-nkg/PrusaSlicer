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
 * number is the most horizontal slope printable without support - one layer's outline may sit
 * at most @c layer_height/tan(theta) outside the one below it, and a run of layers held to
 * that is a slope of exactly that angle.
 *
 * There are two ways to hold them to it, and the plugin picks per layer:
 *
 *  - The **clip** takes the offending material off the upper layer, walking upward. The
 *    overhang is chamfered away and the part comes out a little smaller.
 *  - The **fill** puts material under it instead, walking downward, so the overhang is
 *    carried on a cone of new material down to whatever holds it up. The part comes out
 *    larger and nothing of the mesh is lost. This is what OrcaSlicer calls
 *    @c make_overhang_printable.
 *
 * The hook is deliberately neither of those by name. What it offers is a per-layer bound on
 * how far consecutive outlines may differ, and the interesting part of the decision - which
 * angle, at which heights, how much of the part may be given up or gained for it - is a
 * property of the part and of what it is for. The same mechanism serves a plugin that makes
 * only the first few millimetres self supporting so a part can be printed without a brim's
 * worth of support under its skirt, one that tightens the angle towards the top of a tall part
 * where a curled overhang would be hit by the nozzle on the way round, and one that leaves the
 * model alone below a given Z.
 *
 * Threading: called from PrintObject::slice(), once per layer, before anything is changed, so
 * every plugin sees the outlines the mesh gave. The two remedies are then applied as two
 * passes - every Clip layer bottom up, then every Fill layer top down - because each layer is
 * measured against the outline its neighbour was left with. Unlike the fill, pass and
 * perimeter planners this one is never entered concurrently, so an implementation backed by a
 * runtime that is not thread safe needs no lock of its own.
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
     * @brief The most material that may be cut away or added, in mm, or 0 for no limit.
     *
     * Measured as the largest disc that fits inside the piece in question. An overhang too big
     * to fit under it is left alone in one piece.
     *
     * Under the clip that is what keeps the clip from quietly eating a table top, and -
     * because the piece about to be removed is measured against the outline the layer below was
     * *left* with, not against the mesh - what keeps a slope shallower than theta from being
     * whittled down layer after layer. A slope like that falls at most this far behind the mesh
     * before it is handed back in full. Leaving it at 0 there means the clip is free to remove
     * an overhang of any size, which is the wrong thing for a plugin meant to chamfer an edge.
     *
     * Under the fill it bounds the cone instead, and 0 is the ordinary setting: the
     * whole point is usually to carry an overhang of any size, and a cone cannot run away in
     * any case because it terminates where it meets the part or the bed.
     */
    double max_overhang_width{0.};

};

/**
 * @brief What one layer asks for, at most one plan per remedy.
 *
 * A layer may ask for both, and asking for both is the interesting case: the clip is given a
 * size bound and takes the small overhangs off on the way up, and the fill then carries
 * whatever that bound told the clip to leave alone on the way down. One plugin can therefore
 * spend a sliver of the model on a 1 mm ledge and a cone on a 10 mm shelf, in the same print.
 *
 * They are two fields rather than a list because the two passes run in opposite directions and
 * a layer can only be walked once in each.
 */
struct Plans
{
    std::optional<Plan> clip;
    std::optional<Plan> fill;

    bool empty() const { return ! clip.has_value() && ! fill.has_value(); }
};

using Strategy = std::function<Plans(const LayerInfo& layer)>;

/**
 * @brief Runs @p strategy over @p layer and validates the answer.
 *
 * Drops a plan - leaving the outline exactly as the mesh gave it - when the strategy declines,
 * when it leaves @c max_overhang unbounded and when either distance is not a finite number.
 * Distances outside 0 to MAX_DISTANCE are clamped. Returns an empty Plans when no strategy is
 * installed.
 */
Plans plan_slice(const Strategy& strategy, const LayerInfo& layer);

} // namespace Slic3r::SlicePlanner
