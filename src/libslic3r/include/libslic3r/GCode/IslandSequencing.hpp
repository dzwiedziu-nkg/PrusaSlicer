#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Slic3r/Domain/BoundingBox.hpp"
#include "Slic3r/Domain/Point.hpp"

namespace Slic3r {
class Layer;
} // namespace Slic3r

namespace Slic3r::GCode::IslandSequencing {

/** @brief One island and its links to the adjacent object layers. */
struct IslandInfo
{
    /** Index into Layer::lslices_ex. */
    std::size_t index;
    BoundingBox bbox;
    Domain::Point centroid;
    /** Positions in LayerInfo::islands on the adjacent layers. */
    std::vector<std::size_t> overlaps_below;
    std::vector<std::size_t> overlaps_above;
};

/** @brief Geometry available to an island sequencing planner. */
struct LayerInfo
{
    std::size_t layer_id;
    double print_z;
    double height;
    std::vector<IslandInfo> islands;
};

/** @brief Information about the printer which affects a safe branch order. */
struct PrintContext
{
    std::string printer_model;
    /** Values used by PrusaSlicer's fallback sequential-print head geometry. */
    double extruder_clearance_radius{0.};
    double extruder_clearance_height{0.};
};

/** @brief A subset of one layer to emit as one scheduling step. */
struct PlanStep
{
    /** Position in the vector of LayerInfo passed to the strategy. */
    std::size_t layer;
    /** Positions in that LayerInfo::islands vector, in print order. */
    std::vector<std::size_t> islands;
};

/** @brief Complete schedule returned by a planner. */
struct Plan
{
    std::vector<PlanStep> steps;
    /** Maximum XY distance used while wiping before a downward Z transition. */
    double wipe_distance{2.0};
    /** Extra Z clearance used for the high XY move to the next branch. */
    double z_clearance{0.5};
    /** Set by the engine after validating the plan with a supported head model. */
    bool collision_checked{false};
};

using Strategy = std::function<std::optional<Plan>(
    const std::vector<LayerInfo>& layers, const PrintContext& context)>;

/** @brief Describes all object layers in bed coordinates. */
std::vector<LayerInfo> describe_layers(
    const std::vector<const Layer*>& layers,
    const Domain::Point& instance_offset
);

/**
 * @brief Runs and validates a sequencing strategy.
 *
 * A valid plan emits every island exactly once and never emits an island before
 * all the islands it overlaps on the layer below. Invalid plans are rejected and
 * leave the stock layer-by-layer schedule untouched.
 */
std::optional<Plan> plan_islands(
    const Strategy& strategy,
    const std::vector<const Layer*>& layers,
    const Domain::Point& instance_offset,
    const PrintContext& context
);

/** @brief Whether the engine has an implemented collision model for this printer. */
bool supports_collision_check(const PrintContext& context);

/**
 * @brief Checks a complete schedule against already printed, higher geometry.
 *
 * This currently implements the same three-slice fallback head model used by
 * PrusaSlicer's sequential-object arranger for Original Prusa CORE One.
 */
bool is_collision_free(
    const Plan& plan,
    const std::vector<LayerInfo>& layers,
    const PrintContext& context
);

} // namespace Slic3r::GCode::IslandSequencing
