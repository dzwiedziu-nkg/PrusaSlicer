#include "libslic3r/PerimeterPlanner.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace Slic3r::PerimeterPlanner {

std::optional<Plan> plan_perimeters(const Strategy& strategy, const RegionInfo& region)
{
    if (!strategy) {
        return std::nullopt;
    }

    std::optional<Plan> planned = strategy(region);
    if (!planned.has_value()) {
        // The strategy was happy with what the settings ask for.
        return std::nullopt;
    }

    int perimeters = planned->perimeters;
    if (perimeters < MIN_PERIMETERS || perimeters > MAX_PERIMETERS) {
        SPDLOG_WARN(
            "Perimeter planner asked for {} perimeters on layer {}, which is outside {}..{}; "
            "using the nearest allowed count",
            perimeters,
            region.layer_id,
            MIN_PERIMETERS,
            MAX_PERIMETERS
        );
        perimeters = std::clamp(perimeters, MIN_PERIMETERS, MAX_PERIMETERS);
    }
    Plan plan = *planned;
    plan.perimeters = perimeters;
    for (double* distance : {&plan.unsupported_anchor, &plan.min_unsupported}) {
        if (!std::isfinite(*distance) || *distance < 0. || *distance > MAX_ANCHOR) {
            SPDLOG_WARN(
                "Perimeter planner asked for a distance of {} mm on layer {}, which is outside "
                "0..{}; using the nearest allowed one",
                *distance,
                region.layer_id,
                MAX_ANCHOR
            );
            *distance = std::isfinite(*distance) ? std::clamp(*distance, 0., MAX_ANCHOR) : 0.;
        }
    }
    if (perimeters == region.perimeters && plan.unsupported == Unsupported::Wall) {
        // Answering with the count it was handed, and asking for nothing else, is the same as
        // declining - and saying so here keeps the slicer off an override path for no reason.
        return std::nullopt;
    }
    return plan;
}

} // namespace Slic3r::PerimeterPlanner
