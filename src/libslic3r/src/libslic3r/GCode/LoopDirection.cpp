#include "libslic3r/GCode/LoopDirection.hpp"

namespace Slic3r::GCode::LoopDirection {

std::optional<Direction> plan_direction(const Strategy& strategy, const LoopInfo& loop)
{
    if (!strategy) {
        return std::nullopt;
    }

    const std::optional<Direction> planned = strategy(loop);
    if (!planned.has_value()) {
        // The strategy was happy with the direction the engine picked.
        return std::nullopt;
    }
    if (*planned == loop.stock) {
        // Answering with the direction it was handed is the same as declining.
        return std::nullopt;
    }
    return planned;
}

} // namespace Slic3r::GCode::LoopDirection
