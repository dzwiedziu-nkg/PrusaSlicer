#include "libslic3r/SlicePlanner.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace Slic3r::SlicePlanner {

namespace {

/** @brief Clamps @p value into 0..MAX_DISTANCE, saying so once if it was outside. */
bool sane(double& value, const char* name, std::size_t layer_id)
{
    if (!std::isfinite(value)) {
        SPDLOG_WARN(
            "Slice planner asked for a {} that is not a finite number on layer {}; "
            "leaving the outline alone",
            name,
            layer_id
        );
        return false;
    }
    if (value < 0. || value > MAX_DISTANCE) {
        SPDLOG_WARN(
            "Slice planner asked for a {} of {} mm on layer {}, which is outside 0..{}; "
            "using the nearest allowed distance",
            name,
            value,
            layer_id,
            MAX_DISTANCE
        );
        value = std::clamp(value, 0., MAX_DISTANCE);
    }
    return true;
}

} // namespace

std::optional<Plan> plan_slice(const Strategy& strategy, const LayerInfo& layer)
{
    if (!strategy) {
        return std::nullopt;
    }

    std::optional<Plan> planned = strategy(layer);
    if (!planned.has_value()) {
        // The strategy was happy with the outline the mesh gave.
        return std::nullopt;
    }
    if (planned->max_overhang < 0.) {
        // So is a strategy that answered without naming a bound. Saying so here keeps the
        // slicer from taking the clipping path for no reason. A NaN is not caught by this
        // comparison and falls through to sane() below, which rejects it.
        return std::nullopt;
    }

    Plan plan = *planned;
    if (!sane(plan.max_overhang, "growth", layer.layer_id)
        || !sane(plan.max_overhang_width, "removal width", layer.layer_id)) {
        return std::nullopt;
    }
    return plan;
}

} // namespace Slic3r::SlicePlanner
