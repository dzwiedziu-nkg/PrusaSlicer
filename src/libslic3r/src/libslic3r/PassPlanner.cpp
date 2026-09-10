#include "libslic3r/PassPlanner.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Flow.hpp"

namespace Slic3r::PassPlanner {

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

std::optional<Plan> plan_pass(const Strategy& strategy, const SurfaceInfo& surface)
{
    if (!strategy) {
        return std::nullopt;
    }

    std::optional<Plan> planned = strategy(surface);
    if (!planned.has_value()) {
        // The strategy did not want an extra pass over this area.
        return std::nullopt;
    }

    // Clip to the area rather than rejecting paths that overshoot it. A strategy works
    // from the region it was handed and small overshoot at the boundary is ordinary
    // rounding, not a mistake; the stock fillers have their lines clipped the same way.
    //
    // One path at a time, and the strategy's own path is kept whenever the clip takes
    // nothing off it. Clipper is free to hand a path back reversed, and the order and
    // direction of an extra pass are the whole point of it - a pass whose lines have been
    // flipped one by one has the nozzle fly back across the surface between every pair.
    Domain::Polylines clipped;
    clipped.reserve(planned->paths.size());
    for (Domain::Polyline& path : planned->paths) {
        if (path.size() < 2 || length(path) < MIN_PATH_LENGTH) {
            continue;
        }
        Domain::Polylines pieces{intersection_pl(Domain::Polylines{path}, surface.region)};
        if (pieces.size() == 1 && length(pieces.front()) > length(path) - MIN_PATH_LENGTH) {
            clipped.push_back(std::move(path));
            continue;
        }
        for (Domain::Polyline& piece : pieces) {
            if (piece.size() >= 2 && length(piece) >= MIN_PATH_LENGTH) {
                clipped.push_back(std::move(piece));
            }
        }
    }

    if (clipped.empty()) {
        SPDLOG_WARN(
            "Pass planner produced nothing usable over an area on layer {}, running no "
            "extra pass over it",
            surface.layer_id
        );
        return std::nullopt;
    }

    // A spacing the strategy could not state is the spacing of what it is going over,
    // which is what a pass tracing the covering extrusions back wants anyway.
    double spacing = planned->spacing;
    if (!std::isfinite(spacing) || spacing <= 0.) {
        spacing = surface.spacing;
    }

    // A ratio outside the range is an arithmetic slip in the strategy rather than an
    // intention; clamping keeps a stray NaN from emptying the extruder and a stray 10
    // from burying the surface it was asked to smooth.
    double flow_ratio = planned->flow_ratio;
    if (!std::isfinite(flow_ratio) || flow_ratio < MIN_FLOW_RATIO || flow_ratio > MAX_FLOW_RATIO) {
        SPDLOG_WARN(
            "Pass planner asked for a flow ratio of {} on layer {}, which is outside "
            "{}..{}; using the nearest allowed value",
            flow_ratio,
            surface.layer_id,
            MIN_FLOW_RATIO,
            MAX_FLOW_RATIO
        );
        flow_ratio = std::isfinite(flow_ratio) ?
            std::clamp(flow_ratio, MIN_FLOW_RATIO, MAX_FLOW_RATIO) :
            DEFAULT_FLOW_RATIO;
    }
    // A bound that is not a positive number of seconds is no bound: run the pass in one
    // piece, which is what every planner that never heard of the field asks for.
    double max_run_time = planned->max_run_time;
    if (!std::isfinite(max_run_time) || max_run_time < 0.) {
        SPDLOG_WARN(
            "Pass planner asked for a maximum run time of {} on layer {}, which is not a "
            "length of time; running the pass unbroken",
            max_run_time,
            surface.layer_id
        );
        max_run_time = 0.;
    }
    return Plan{std::move(clipped), spacing, flow_ratio, max_run_time};
}

PassFlow pass_flow(const SurfaceInfo& surface, const Plan& plan)
{
    // The height of the ribbon this pass lays down: a fraction of a layer, thinned further
    // when the pass runs its lines closer together than the nozzle is wide. Straight out of
    // Layer::make_ironing(), so that a plugin's flow_ratio means exactly what the slicer's
    // own ironing_flowrate means.
    const double height = plan.flow_ratio * surface.layer_height * plan.spacing
        / surface.nozzle_diameter;
    return PassFlow{
        surface.nozzle_diameter * height,
        double(Flow::rounded_rectangle_extrusion_width_from_spacing(
            float(surface.nozzle_diameter), float(height)
        )),
        height
    };
}

} // namespace Slic3r::PassPlanner
