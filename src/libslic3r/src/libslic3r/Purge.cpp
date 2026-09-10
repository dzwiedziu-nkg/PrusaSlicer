#include "libslic3r/Purge.hpp"

#include <memory>

#include <spdlog/spdlog.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Surface.hpp"

namespace Slic3r::Purge {

namespace {

void fill_expolygon(
    ExtrusionEntitiesPtr &dst,
    ExPolygon           &&expolygon,
    Fill                 *filler,
    const FillParams     &fill_params,
    ExtrusionRole         role,
    const Flow           &flow)
{
    Surface surface(stInternal, std::move(expolygon));
    Polylines polylines;
    try {
        polylines = filler->fill_surface(&surface, fill_params);
    } catch (InfillFailedException &) {
        // A shape the filler cannot handle is one fewer place to purge, not a failed slice.
    }
    extrusion_entities_append_paths(dst, std::move(polylines), { role, flow });
}

} // namespace

ExtrusionEntitiesPtr generate(
    const Layer  &layer,
    const Flow   &flow,
    const double  volume,
    ExtrusionRole role)
{
    ExtrusionEntitiesPtr out;
    if (volume <= 0. || flow.mm3_per_mm() <= 0.) {
        return out;
    }

    // Room is what the slicer meant to fill, less what it actually put there, less half a bead
    // so a purge line never lands on material that is already down.
    const float margin = 0.5f * float(scale_(flow.width()));
    ExPolygons room;
    for (const LayerRegion *region : layer.regions()) {
        Polygons covered = region->fills().polygons_covered_by_width(float(SCALED_EPSILON));
        append(covered, region->perimeters().polygons_covered_by_width(float(SCALED_EPSILON)));
        append(room, offset_ex(diff_ex(region->fill_expolygons(), covered), - margin));
    }
    if (room.empty()) {
        return out;
    }

    std::unique_ptr<Fill> filler(Fill::new_from_type(Domain::InfillPattern::ipRectilinear));
    filler->angle   = 0.f;
    filler->spacing = flow.spacing();
    FillParams fill_params;
    fill_params.density     = 1.f;
    fill_params.dont_adjust = true;

    ExtrusionEntitiesPtr lines;
    for (ExPolygon &expoly : room) {
        fill_expolygon(lines, std::move(expoly), filler.get(), fill_params, role, flow);
    }

    // Whole lines until the budget is met: a purge is a quantity of plastic, not a pattern.
    double taken = 0.;
    for (ExtrusionEntity *line : lines) {
        if (taken >= volume) {
            delete line;
            continue;
        }
        taken += line->total_volume();
        out.emplace_back(line);
    }
    if (taken < volume) {
        SPDLOG_INFO(
            "Layer {} had room for {:.1f} mm3 of purge out of the {:.1f} asked for",
            layer.id(), taken, volume
        );
    }
    return out;
}

double volume_of(const ExtrusionEntitiesPtr &paths)
{
    double volume = 0.;
    for (const ExtrusionEntity *path : paths) {
        volume += path->total_volume();
    }
    return volume;
}

} // namespace Slic3r::Purge
