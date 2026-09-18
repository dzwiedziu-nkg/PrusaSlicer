#include "libslic3r/Fill/FillPlanner.hpp"

#include <algorithm>
#include <cmath>

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

/** @brief Clamps a flow ratio into the allowed range, saying so once if it was outside. */
double checked_flow_ratio(const double asked, const SurfaceInfo& surface)
{
    // A ratio outside the range is an arithmetic slip in the strategy rather than an
    // intention; clamping keeps a stray zero or NaN from emptying the extruder.
    if (std::isfinite(asked) && asked >= MIN_FLOW_RATIO && asked <= MAX_FLOW_RATIO) {
        return asked;
    }
    SPDLOG_WARN(
        "Fill planner asked for a flow ratio of {} on layer {}, which is outside {}..{}; "
        "using the nearest allowed value",
        asked, surface.layer_id, MIN_FLOW_RATIO, MAX_FLOW_RATIO
    );
    return std::isfinite(asked) ? std::clamp(asked, MIN_FLOW_RATIO, MAX_FLOW_RATIO) : 1.;
}

/** @brief Keeps a speed the role can be printed at, or 0 to leave the role's own. */
double checked_speed(const double asked, const SurfaceInfo& surface)
{
    if (asked == 0. || (std::isfinite(asked) && asked >= MIN_SPEED && asked <= MAX_SPEED)) {
        return asked;
    }
    SPDLOG_WARN(
        "Fill planner asked for {} mm/s on layer {}, which is outside {}..{}; using the speed "
        "the role asks for",
        asked, surface.layer_id, MIN_SPEED, MAX_SPEED
    );
    return 0.;
}

} // namespace

Plan plan_fill(const Strategy& strategy, const SurfaceInfo& surface, Domain::Polylines stock)
{
    if (!strategy) {
        return Plan{std::move(stock)};
    }

    std::optional<Plan> planned = strategy(surface);
    if (!planned.has_value()) {
        // The strategy had no opinion about this surface.
        return Plan{std::move(stock)};
    }

    // A plan with no paths keeps the slicer's own and changes only how they are printed. It is
    // the difference between "lay these lines" and "lay your lines, thinner": the second needs
    // no geometry from the plugin and keeps the links between the lines, which the first loses.
    //
    // A surface the slicer lays with a width per point is always in that case: this contract
    // has one width for the whole path, so geometry offered for one is dropped and only the
    // flow and the speed are taken. SurfaceInfo::variable_width tells a strategy so.
    if (planned->paths.empty() || surface.variable_width) {
        return Plan{std::move(stock), checked_flow_ratio(planned->flow_ratio, surface),
                    checked_speed(planned->speed, surface)};
    }

    // Clip to the surface rather than rejecting paths that overshoot it. A strategy
    // works from the region it was handed and small overshoot at the boundary is
    // ordinary rounding, not a mistake; the stock fillers have their lines clipped the
    // same way.
    Domain::Polylines clipped{intersection_pl(planned->paths, surface.region)};
    std::erase_if(clipped, [](const Domain::Polyline& path) {
        return path.size() < 2 || length(path) < MIN_PATH_LENGTH;
    });

    if (clipped.empty()) {
        SPDLOG_WARN(
            "Fill planner produced nothing usable for a surface on layer {}, "
            "keeping the slicer's own paths",
            surface.layer_id
        );
        return Plan{std::move(stock)};
    }

    return Plan{std::move(clipped), checked_flow_ratio(planned->flow_ratio, surface),
                checked_speed(planned->speed, surface)};
}

} // namespace Slic3r::FillPlanner
