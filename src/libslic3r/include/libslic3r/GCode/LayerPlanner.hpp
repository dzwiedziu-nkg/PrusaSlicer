#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "Slic3r/Domain/BoundingBox.hpp"
#include "Slic3r/Domain/GCodeExtrusionRole.hpp"

/**
 * @brief Extension point for the order in which the groups of a layer are printed.
 *
 * A layer is printed island by island, and each island's walls and fill in the order one print
 * setting - @c infill_first - names for the whole object. That is the wrong shape for anything
 * that depends on where in the layer the head has just been.
 *
 * The case that motivated the hook is the Benchy hull line. Prusa trace it to the height where
 * a part turns from sparse infill into solid layers: the material and the time per layer jump
 * and the wall running through that height cools differently above and below it. What they
 * found by hand in the G-code was that **printing the deck before the rest of the layer** helps
 * - the wall is then laid at a distance in time from the mass of solid beside it. That is an
 * ordering decision, and there was no way to ask for it.
 *
 * The same question serves a plugin that puts the infill first only on the layers that bridge,
 * one that prints a tapped boss before anything else touches it, and one that finishes an
 * island completely where the stock order would return to it later.
 *
 * A Strategy installed on the Print (Biz::Slicing::IPrint::layer_planner) is offered every
 * layer's groups and answers with the order to print them in. With no strategy installed the
 * stock order is used and the G-code is bit for bit the stock G-code.
 *
 * Note what a group is, because it is what makes an answer expressible: the walls of one island
 * and one run of its fill. The engine can print those in any order, including interleaved
 * between islands, because each becomes its own entry in what the G-code writer is handed.
 *
 * Threading: the strategy is invoked from the G-code layer generator, a
 * tbb::filter_mode::serial_in_order pipeline stage, so calls belonging to one Print are
 * serialized and **arrive in layer order** - which is what lets a plugin carry a running
 * picture of the part from one layer to the next and notice where it changes. Several Prints
 * may be exported at once, so a strategy must not be shared between them unless it is thread
 * safe.
 */
namespace Slic3r::GCode::LayerPlanner {

/** @brief What a group of extrusions is. */
enum class GroupKind
{
    /** @brief The wall loops of one island, in the order the perimeter generator left them. */
    Perimeters,
    /** @brief One run of an island's fill, which is one region's worth of it. */
    Fill
};

/** @brief What one role contributes to a group. */
struct RoleShare
{
    Domain::GCodeExtrusionRole role;
    /** @brief Length of this role's extrusions in the group, in millimetres. */
    double length;
    /** @brief What they extrude, in cubic millimetres. */
    double volume;
};

/** @brief One group of a layer's extrusions, as it is about to be collected. */
struct GroupInfo
{
    /**
     * @brief Which island of the layer it belongs to, counted in the order they are printed.
     *
     * Offered so that a strategy can keep an island's groups together, or deliberately not.
     */
    std::size_t island;
    GroupKind kind;
    /**
     * @brief The role carrying most of the group's length. See @c roles for the rest.
     *
     * A wall group is ExternalPerimeter or Perimeter depending on which of them is longer, and
     * a fill run is often one role throughout. Often, not always - which is why this is not the
     * whole answer.
     */
    Domain::GCodeExtrusionRole role;
    /** @brief Length of everything in the group, in millimetres. */
    double length;
    /** @brief What it extrudes, in cubic millimetres. */
    double volume;
    /** @brief Where it is, scaled, in the G-code (bed) frame - like the island order hook's. */
    BoundingBox bbox;
    /**
     * @brief What each role in the group contributes, longest first.
     *
     * A group is not always one role, and the case that matters is exactly the one the hook was
     * built for: where a deck grows inside a part, its solid infill and the sparse infill round
     * it belong to the same region and therefore to the same run, and the sparse is the longer
     * of the two. A strategy given only the dominant role would see no solid infill at all on
     * the layer where it appears, and so miss the transition it is looking for.
     */
    std::vector<RoleShare> roles;
};

/** @brief The layer the groups belong to. */
struct LayerContext
{
    std::size_t layer_id;
    /** @brief Height of the top of this layer, in millimetres. */
    double print_z;
    unsigned extruder_id;
};

/**
 * @brief Decides the order a layer's groups are printed in.
 *
 * Returns a permutation of [0, groups.size()) holding **positions into @p groups**. Anything
 * that is not such a permutation is treated as a failure and the stock order is used for that
 * layer, so a broken strategy can never lose an extrusion.
 */
using Strategy = std::function<
    std::vector<std::size_t>(const std::vector<GroupInfo>& groups, const LayerContext& context)>;

/**
 * @brief Runs @p strategy over @p groups and validates the answer.
 *
 * Falls back to the stock order - 0, 1, 2, ... - when no strategy is installed, when the layer
 * has fewer than two groups, and when the answer is not a permutation of them.
 */
std::vector<std::size_t> order_groups(
    const Strategy& strategy, const std::vector<GroupInfo>& groups, const LayerContext& context);

} // namespace Slic3r::GCode::LayerPlanner
