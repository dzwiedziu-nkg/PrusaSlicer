#include "libslic3r/GCode/IslandOrdering.hpp"

#include <numeric>

#include "libslic3r/Layer.hpp"
#include "Slic3r/Log.hpp"

namespace Slic3r::GCode::IslandOrdering {

namespace {

double distance(const Point& a, const Point& b)
{
    // Cast before taking the norm: coord_t is scaled, the squares overflow the integer type.
    return (a - b).cast<double>().norm();
}

/** @brief True if @p order is a permutation of [0, count). */
bool is_permutation(const std::vector<std::size_t>& order, const std::size_t count)
{
    if (order.size() != count) {
        return false;
    }

    std::vector<bool> seen(count, false);
    for (const std::size_t position : order) {
        if (position >= count || seen[position]) {
            return false;
        }
        seen[position] = true;
    }
    return true;
}

} // namespace

std::vector<IslandInfo> describe_islands(
    const Layer& layer,
    const Point& instance_offset,
    const std::vector<std::size_t>* selected_indices
)
{
    std::vector<IslandInfo> islands;
    const std::vector<std::size_t>& indices = selected_indices == nullptr ?
        layer.lslice_indices_sorted_by_print_order : *selected_indices;
    islands.reserve(indices.size());

    for (const std::size_t index : indices) {
        if (index >= layer.lslices_ex.size()) {
            continue;
        }

        // lslices are in the instance local frame; shift them so that a strategy sees
        // the same coordinates as the head position.
        const BoundingBox& local = layer.lslices_ex[index].bbox;
        const BoundingBox bbox{local.min + instance_offset, local.max + instance_offset, local.defined};
        islands.push_back(IslandInfo{index, bbox, Point{(bbox.min + bbox.max) / 2}});
    }
    return islands;
}

double travel_estimate(
    const std::vector<IslandInfo>& islands,
    const std::vector<std::size_t>& order,
    const std::optional<Point>& start
)
{
    if (order.empty()) {
        return 0.;
    }

    double cost = start ? distance(*start, islands[order.front()].centroid) : 0.;
    for (std::size_t i = 1; i < order.size(); ++i) {
        cost += distance(islands[order[i - 1]].centroid, islands[order[i]].centroid);
    }
    return cost;
}

std::vector<std::size_t> order_islands(
    const Strategy& strategy,
    const Layer& layer,
    const Point& instance_offset,
    const unsigned extruder_id,
    const std::optional<Point>& head_position,
    const std::vector<std::size_t>* selected_indices
)
{
    const std::vector<std::size_t>& stock_order = selected_indices == nullptr ?
        layer.lslice_indices_sorted_by_print_order : *selected_indices;

    // Nothing to decide for a single island, and no strategy means stock behaviour.
    if (!strategy || stock_order.size() < 2) {
        return stock_order;
    }

    const std::vector<IslandInfo> islands = describe_islands(layer, instance_offset, selected_indices);
    if (islands.size() < 2) {
        return stock_order;
    }

    const LayerContext context{layer.id(), layer.print_z, extruder_id, head_position};

    const std::vector<std::size_t> positions = strategy(islands, context);
    if (!is_permutation(positions, islands.size())) {
        SPDLOG_WARN(
            "Island ordering strategy returned an invalid permutation for layer {} ({} islands); "
            "falling back to the stock order",
            layer.id(),
            islands.size()
        );
        return stock_order;
    }

    // Translate positions in `islands` back to lslice indices.
    std::vector<std::size_t> result;
    result.reserve(positions.size());
    for (const std::size_t position : positions) {
        result.push_back(islands[position].index);
    }
    return result;
}

} // namespace Slic3r::GCode::IslandOrdering
