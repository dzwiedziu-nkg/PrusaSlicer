#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "Slic3r/Domain/BoundingBox.hpp"
#include "Slic3r/Domain/Point.hpp"

namespace Slic3r {
class Layer;
} // namespace Slic3r

/**
 * @brief Extension point for the order in which the islands of a layer are printed.
 *
 * A layer of a print object is made of one or more disjoint islands (Layer::lslices).
 * By default their print order is baked in at slicing time by Layer::make_slices(),
 * which chains them by the shortest traverse without knowing where the print head
 * actually is when the layer begins. The head therefore tends to return to the same
 * island at the start of every layer, which costs travel on prints that split into
 * many islands.
 *
 * A Strategy installed on the Print (Biz::Slicing::IPrint::island_ordering_strategy)
 * is consulted during G-code export instead, where the head position is known.
 * With no strategy installed the stock order is returned unchanged, so the default
 * behaviour is bit for bit the same as without this extension point.
 *
 * Threading: the strategy is invoked from the G-code layer generator, which is a
 * tbb::filter_mode::serial_in_order pipeline stage. Calls belonging to one Print are
 * therefore serialized and arrive in layer order, which makes it safe to delegate to
 * a runtime that is not itself thread safe (a Lua state, for instance). Note that
 * several Print instances may be exported concurrently, so a strategy must not be
 * shared between them unless it is thread safe.
 */
namespace Slic3r::GCode::IslandOrdering {

/**
 * @brief One printable island of the current layer.
 *
 * Coordinates are scaled and expressed in the G-code (bed) frame, the same frame as
 * LayerContext::head_position, so that a strategy can reason about travel without
 * knowing about instance offsets.
 */
struct IslandInfo
{
    /** Index into Layer::lslices and Layer::lslices_ex. */
    std::size_t index;
    BoundingBox bbox;
    Domain::Point centroid;
};

/**
 * @brief The layer the islands belong to, and where the head is when it starts.
 */
struct LayerContext
{
    std::size_t layer_id;
    /** Print Z of the layer, in millimetres. */
    double print_z;
    unsigned extruder_id;
    /**
     * Head position when the islands of this layer are about to be emitted.
     * Empty at the very start of a print, before anything has been extruded.
     */
    std::optional<Domain::Point> head_position;
};

/**
 * @brief Decides the print order of a layer's islands.
 *
 * Returns a permutation of [0, islands.size()) holding *positions into `islands`*,
 * not lslice indices. Returning anything that is not such a permutation is treated
 * as a failure and the stock order is used for that layer instead.
 */
using Strategy = std::function<
    std::vector<std::size_t>(const std::vector<IslandInfo>& islands, const LayerContext& context)>;

/**
 * @brief Describes @p layer's islands in the G-code frame, in stock print order.
 */
std::vector<IslandInfo> describe_islands(
    const Layer& layer,
    const Domain::Point& instance_offset,
    const std::vector<std::size_t>* selected_indices = nullptr
);

/**
 * @brief Runs @p strategy and returns lslice indices in print order.
 *
 * Falls back to Layer::lslice_indices_sorted_by_print_order when @p strategy is empty,
 * when the layer has fewer than two islands, or when the strategy returns an invalid
 * permutation.
 */
std::vector<std::size_t> order_islands(
    const Strategy& strategy,
    const Layer& layer,
    const Domain::Point& instance_offset,
    unsigned extruder_id,
    const std::optional<Domain::Point>& head_position,
    const std::vector<std::size_t>* selected_indices = nullptr
);

/**
 * @brief Total head travel between island centroids for @p order, in scaled units.
 *
 * Provided so that a strategy author can score an ordering with the same measure the
 * engine uses. @p order holds positions into @p islands, as returned by a Strategy.
 */
double travel_estimate(
    const std::vector<IslandInfo>& islands,
    const std::vector<std::size_t>& order,
    const std::optional<Domain::Point>& start
);

} // namespace Slic3r::GCode::IslandOrdering
