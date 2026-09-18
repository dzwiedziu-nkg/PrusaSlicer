#include "libslic3r/GCode/LayerPlanner.hpp"

#include <numeric>
#include <vector>

#include <spdlog/spdlog.h>

namespace Slic3r::GCode::LayerPlanner {

namespace {

std::vector<std::size_t> stock_order(const std::size_t count)
{
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    return order;
}

/** @brief True when @p order holds every position in [0, count) exactly once. */
bool is_permutation_of(const std::vector<std::size_t>& order, const std::size_t count)
{
    if (order.size() != count) {
        return false;
    }
    std::vector<bool> seen(count, false);
    for (const std::size_t position : order) {
        if (position >= count || seen[position]) {
            return false;
        }
        seen[position] = true;
    }
    return true;
}

} // namespace

std::vector<std::size_t> order_groups(
    const Strategy& strategy, const std::vector<GroupInfo>& groups, const LayerContext& context)
{
    if (!strategy || groups.size() < 2) {
        return stock_order(groups.size());
    }

    const std::vector<std::size_t> order = strategy(groups, context);
    if (!is_permutation_of(order, groups.size())) {
        SPDLOG_WARN(
            "Layer planner answered with something that is not an order of the {} groups of "
            "layer {}; printing them as the slicer arranged them",
            groups.size(),
            context.layer_id
        );
        return stock_order(groups.size());
    }
    return order;
}

} // namespace Slic3r::GCode::LayerPlanner
