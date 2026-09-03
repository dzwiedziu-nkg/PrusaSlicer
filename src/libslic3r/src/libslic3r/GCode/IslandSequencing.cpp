#include "libslic3r/GCode/IslandSequencing.hpp"

#include <cmath>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "libslic3r/Layer.hpp"

namespace Slic3r::GCode::IslandSequencing {

namespace {

using PositionMap = std::unordered_map<std::size_t, std::size_t>;

std::vector<std::size_t> linked_positions(
    const LayerSlice::Links& links, const PositionMap* positions
)
{
    std::vector<std::size_t> result;
    if (positions == nullptr) {
        return result;
    }

    result.reserve(links.size());
    for (const LayerSlice::Link& link : links) {
        if (link.slice_idx < 0) {
            continue;
        }
        const auto it = positions->find(static_cast<std::size_t>(link.slice_idx));
        if (it != positions->end()) {
            result.push_back(it->second);
        }
    }
    return result;
}

bool valid_plan(const Plan& plan, const std::vector<LayerInfo>& layers)
{
    if (plan.steps.empty() || !std::isfinite(plan.wipe_distance)
        || !std::isfinite(plan.z_clearance) || plan.wipe_distance < 0.
        || plan.wipe_distance > 20. || plan.z_clearance < 0. || plan.z_clearance > 10.) {
        return false;
    }

    std::vector<std::vector<bool>> emitted;
    emitted.reserve(layers.size());
    for (const LayerInfo& layer : layers) {
        emitted.emplace_back(layer.islands.size(), false);
    }

    for (const PlanStep& step : plan.steps) {
        if (step.layer >= layers.size() || step.islands.empty()) {
            return false;
        }

        const LayerInfo& layer = layers[step.layer];
        for (const std::size_t island_position : step.islands) {
            if (island_position >= layer.islands.size()
                || emitted[step.layer][island_position]) {
                return false;
            }

            for (const std::size_t below : layer.islands[island_position].overlaps_below) {
                if (step.layer == 0 || below >= emitted[step.layer - 1].size()
                    || !emitted[step.layer - 1][below]) {
                    return false;
                }
            }
            emitted[step.layer][island_position] = true;
        }
    }

    for (const auto& layer : emitted) {
        for (const bool island_was_emitted : layer) {
            if (!island_was_emitted) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

std::vector<LayerInfo> describe_layers(
    const std::vector<const Layer*>& layers, const Domain::Point& instance_offset
)
{
    std::vector<PositionMap> positions(layers.size());
    for (std::size_t layer_position = 0; layer_position < layers.size(); ++layer_position) {
        const Layer& layer = *layers[layer_position];
        PositionMap& map = positions[layer_position];
        for (std::size_t island_position = 0;
             island_position < layer.lslice_indices_sorted_by_print_order.size();
             ++island_position) {
            map.emplace(
                layer.lslice_indices_sorted_by_print_order[island_position], island_position
            );
        }
    }

    std::vector<LayerInfo> result;
    result.reserve(layers.size());
    for (std::size_t layer_position = 0; layer_position < layers.size(); ++layer_position) {
        const Layer& layer = *layers[layer_position];
        LayerInfo layer_info{layer.id(), layer.print_z, layer.height, {}};
        layer_info.islands.reserve(layer.lslice_indices_sorted_by_print_order.size());

        for (const std::size_t index : layer.lslice_indices_sorted_by_print_order) {
            if (index >= layer.lslices_ex.size()) {
                continue;
            }
            const LayerSlice& slice = layer.lslices_ex[index];
            const BoundingBox bbox{
                slice.bbox.min + instance_offset,
                slice.bbox.max + instance_offset,
                slice.bbox.defined
            };
            layer_info.islands.push_back(IslandInfo{
                index,
                bbox,
                Domain::Point{(bbox.min + bbox.max) / 2},
                linked_positions(
                    slice.overlaps_below,
                    layer_position == 0 ? nullptr : &positions[layer_position - 1]
                ),
                linked_positions(
                    slice.overlaps_above,
                    layer_position + 1 == layers.size() ? nullptr : &positions[layer_position + 1]
                )
            });
        }
        result.push_back(std::move(layer_info));
    }
    return result;
}

std::optional<Plan> plan_islands(
    const Strategy& strategy,
    const std::vector<const Layer*>& layers,
    const Domain::Point& instance_offset,
    const PrintContext& context
)
{
    if (!strategy || layers.empty()) {
        return std::nullopt;
    }

    const std::vector<LayerInfo> layer_info = describe_layers(layers, instance_offset);
    std::optional<Plan> plan = strategy(layer_info, context);
    if (!plan.has_value()) {
        return std::nullopt;
    }
    if (!valid_plan(*plan, layer_info)) {
        SPDLOG_ERROR(
            "Island sequencing plugin returned an invalid or unsupported plan; "
            "falling back to normal layer-by-layer printing"
        );
        return std::nullopt;
    }
    return plan;
}

} // namespace Slic3r::GCode::IslandSequencing
