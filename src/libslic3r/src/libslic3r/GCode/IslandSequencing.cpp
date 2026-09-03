#include "libslic3r/GCode/IslandSequencing.hpp"

#include <cmath>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "libslic3r/Layer.hpp"

namespace Slic3r::GCode::IslandSequencing {

bool supports_collision_check(const PrintContext& context)
{
    // Start deliberately narrowly. Other printers keep the existing explicit
    // unchecked-collision warning until their carriage geometry is implemented.
    return context.printer_model == "COREONE"
        && std::isfinite(context.extruder_clearance_radius)
        && std::isfinite(context.extruder_clearance_height)
        && context.extruder_clearance_radius > 0.
        && context.extruder_clearance_height > 1.;
}

namespace {

bool intervals_within(
    const Domain::coord_t a_min,
    const Domain::coord_t a_max,
    const Domain::coord_t b_min,
    const Domain::coord_t b_max,
    const Domain::coord_t clearance
)
{
    return a_max + clearance >= b_min && b_max + clearance >= a_min;
}

bool head_intersects(
    const IslandInfo& target,
    const IslandInfo& obstacle,
    const double obstacle_above_nozzle,
    const PrintContext& context
)
{
    if (obstacle_above_nozzle <= EPSILON) {
        return false;
    }

    // This mirrors ArrangeHelper.cpp's fallback geometry: a 10 x 10 mm nozzle
    // slice at Z=0, a square clearance-radius slice at Z=1 mm, and an X gantry
    // spanning the bed at extruder_clearance_height.
    const double clearance_mm = obstacle_above_nozzle < 1.
        ? 5.
        : context.extruder_clearance_radius;
    const Domain::coord_t clearance = scaled(clearance_mm);
    const bool y_intersects = intervals_within(
        target.bbox.min.y(), target.bbox.max.y(),
        obstacle.bbox.min.y(), obstacle.bbox.max.y(), clearance
    );
    if (!y_intersects) {
        return false;
    }

    if (obstacle_above_nozzle >= context.extruder_clearance_height) {
        // The X gantry is conservatively treated as spanning the whole bed.
        return true;
    }
    return intervals_within(
        target.bbox.min.x(), target.bbox.max.x(),
        obstacle.bbox.min.x(), obstacle.bbox.max.x(), clearance
    );
}

} // namespace

bool is_collision_free(
    const Plan& plan,
    const std::vector<LayerInfo>& layers,
    const PrintContext& context
)
{
    if (!supports_collision_check(context)) {
        return false;
    }

    struct EmittedIsland
    {
        const IslandInfo* island;
        double print_z;
    };
    std::vector<EmittedIsland> emitted;

    for (const PlanStep& step : plan.steps) {
        if (step.layer >= layers.size()) {
            return false;
        }
        const LayerInfo& layer = layers[step.layer];
        for (const std::size_t island_position : step.islands) {
            if (island_position >= layer.islands.size()) {
                return false;
            }
            const IslandInfo& target = layer.islands[island_position];
            for (const EmittedIsland& obstacle : emitted) {
                if (head_intersects(
                        target,
                        *obstacle.island,
                        obstacle.print_z - layer.print_z,
                        context
                    )) {
                    return false;
                }
            }
            emitted.push_back(EmittedIsland{&target, layer.print_z});
        }
    }
    return true;
}

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
    // This is an engine verdict, never planner-supplied metadata.
    plan->collision_checked = false;
    if (!valid_plan(*plan, layer_info)) {
        SPDLOG_ERROR(
            "Island sequencing plugin returned an invalid or unsupported plan; "
            "falling back to normal layer-by-layer printing"
        );
        return std::nullopt;
    }
    if (supports_collision_check(context)) {
        if (!is_collision_free(*plan, layer_info, context)) {
            SPDLOG_ERROR(
                "Island sequencing plugin returned a plan which collides with the "
                "CORE One print-head geometry; falling back to normal layer order"
            );
            return std::nullopt;
        }
        plan->collision_checked = true;
    }
    return plan;
}

} // namespace Slic3r::GCode::IslandSequencing
