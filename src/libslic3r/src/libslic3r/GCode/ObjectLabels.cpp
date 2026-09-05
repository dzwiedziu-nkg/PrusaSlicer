#include "libslic3r/GCode/ObjectLabels.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r::GCode::ObjectLabels {

std::optional<std::string> name_for(const Strategy& strategy, const RegionInfo& region)
{
    if (!strategy) {
        return std::nullopt;
    }

    std::optional<std::string> name = strategy(region);
    if (!name.has_value()) {
        return std::nullopt;
    }
    // A name of nothing but spaces would define an object the printer cannot show and
    // the user cannot pick out of the list, which is worse than not naming it at all.
    const bool blank = std::ranges::all_of(*name, [](const unsigned char c) {
        return std::isspace(c) != 0;
    });
    return blank ? std::nullopt : name;
}

} // namespace Slic3r::GCode::ObjectLabels
