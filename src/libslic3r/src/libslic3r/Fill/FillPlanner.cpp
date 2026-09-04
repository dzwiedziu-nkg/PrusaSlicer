#include "libslic3r/Fill/FillPlanner.hpp"

#include <spdlog/spdlog.h>

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::FillPlanner {

namespace {

/** @brief Drops paths too short to extrude, left behind by clipping. */
constexpr double MIN_PATH_LENGTH = SCALED_EPSILON * 10.;

double length(const Domain::Polyline& path)
{
    double total = 0.;
    for (std::size_t i = 1; i < path.size(); ++i) {
        total += (path.points[i] - path.points[i - 1]).cast<double>().norm();
    }
    return total;
}

} // namespace

Domain::Polylines plan_fill(
    const Strategy& strategy, const SurfaceInfo& surface, Domain::Polylines stock
)
{
    if (!strategy) {
        return stock;
    }

    std::optional<Domain::Polylines> planned = strategy(surface);
    if (!planned.has_value()) {
        // The strategy had no opinion about this surface.
        return stock;
    }

    // Clip to the surface rather than rejecting paths that overshoot it. A strategy
    // works from the region it was handed and small overshoot at the boundary is
    // ordinary rounding, not a mistake; the stock fillers have their lines clipped the
    // same way.
    Domain::Polylines clipped{intersection_pl(*planned, surface.region)};
    std::erase_if(clipped, [](const Domain::Polyline& path) {
        return path.size() < 2 || length(path) < MIN_PATH_LENGTH;
    });

    if (clipped.empty()) {
        SPDLOG_WARN(
            "Fill planner produced nothing usable for a surface on layer {}, "
            "keeping the slicer's own paths",
            surface.layer_id
        );
        return stock;
    }
    return clipped;
}

} // namespace Slic3r::FillPlanner
