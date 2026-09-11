#include "libslic3r/ResumePlanner.hpp"

#include <cmath>

#include <spdlog/spdlog.h>

namespace Slic3r::ResumePlanner {

std::optional<Plan> plan_resume(const Strategy& strategy, const ResumeInfo& resume)
{
    if (!strategy) {
        return std::nullopt;
    }

    std::optional<Plan> planned = strategy(resume);
    if (!planned.has_value()) {
        // The strategy did not want anything done about this interruption.
        return std::nullopt;
    }

    double purge_volume = planned->purge_volume;
    if (!std::isfinite(purge_volume) || purge_volume < 0.) {
        SPDLOG_WARN(
            "Resume planner asked to purge {} mm3 on layer {}, which is not a quantity; "
            "resuming without one",
            purge_volume,
            resume.layer_id
        );
        purge_volume = 0.;
    }
    if (purge_volume <= 0.) {
        // Asking for nothing is the same as declining, and saying so here keeps every caller
        // from having to check the number as well as the optional.
        return std::nullopt;
    }
    return Plan{purge_volume};
}

} // namespace Slic3r::ResumePlanner
