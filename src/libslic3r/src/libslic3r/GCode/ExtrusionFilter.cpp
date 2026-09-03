#include "libslic3r/GCode/ExtrusionFilter.hpp"

namespace Slic3r::GCode::ExtrusionFilter {

bool keep(const Predicate& predicate, const PathInfo& path)
{
    return !predicate || predicate(path);
}

} // namespace Slic3r::GCode::ExtrusionFilter
