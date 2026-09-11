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

/**
 * @brief Shortens @p path to @p keep scaled units, interpolating the last point.
 *
 * Domain::Polyline has no clip_end() of its own - that lives on the private Polyline - and a
 * purge only ever needs the front of a line, so this walks from the start and stops.
 */
void truncate(Domain::Polyline &path, const double keep)
{
    double travelled = 0.;
    for (std::size_t i = 1; i < path.points.size(); ++i) {
        const Vec2d from = path.points[i - 1].cast<double>();
        const Vec2d to = path.points[i].cast<double>();
        const double segment = (to - from).norm();
        if (travelled + segment < keep || segment <= 0.) {
            travelled += segment;
            continue;
        }
        const double along = (keep - travelled) / segment;
        const Vec2d cut = from + (to - from) * along;
        path.points.resize(i);
        path.points.emplace_back(Domain::coord_t(cut.x()), Domain::coord_t(cut.y()));
        return;
    }
}

/** @brief What the slicer meant to fill, less what it put there, less half a bead. */
ExPolygons room_on(const Layer &layer, const Flow &flow)
{
    const float margin = 0.5f * float(scale_(flow.width()));
    ExPolygons room;
    for (const LayerRegion *region : layer.regions()) {
        Polygons covered = region->fills().polygons_covered_by_width(float(SCALED_EPSILON));
        append(covered, region->perimeters().polygons_covered_by_width(float(SCALED_EPSILON)));
        append(room, offset_ex(diff_ex(region->fill_expolygons(), covered), - margin));
    }
    return room;
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
    ExPolygons room = room_on(layer, flow);
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

    // Lines until the budget is met, and the last one cut to fit.
    //
    // Taking whole lines is not good enough here: the filler links its lines into one run per
    // island, so a single "line" can be most of the layer, and asking for 25 mm3 on the test
    // part gave 41. A purge is a quantity of plastic and the quantity is the whole point of
    // it, so the overshoot is trimmed off the end rather than lived with.
    double taken = 0.;
    for (ExtrusionEntity *line : lines) {
        if (taken >= volume) {
            delete line;
            continue;
        }
        if (auto *path = dynamic_cast<ExtrusionPath *>(line);
            path != nullptr && path->mm3_per_mm() > 0. && taken + path->total_volume() > volume) {
            truncate(path->polyline, scale_((volume - taken) / path->mm3_per_mm()));
            if (path->polyline.size() < 2) {
                delete line;
                continue;
            }
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

double spare_area(const Layer &layer, const Flow &flow)
{
    double area = 0.;
    for (const ExPolygon &expoly : room_on(layer, flow)) {
        area += expoly.area();
    }
    return unscaled<double>(unscaled<double>(area));
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
