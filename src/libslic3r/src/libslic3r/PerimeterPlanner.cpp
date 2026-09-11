#include "libslic3r/PerimeterPlanner.hpp"

#include <algorithm>

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
    if (perimeters == region.perimeters) {
        // Answering with the count it was handed is the same as declining, and saying so here
        // keeps the slicer from taking an override path for no reason.
        return std::nullopt;
    }
    return Plan{perimeters};
}

} // namespace Slic3r::PerimeterPlanner
