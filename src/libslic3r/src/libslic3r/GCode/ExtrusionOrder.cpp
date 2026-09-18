#include "ExtrusionOrder.hpp"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <map>
#include <ranges>
#include <unordered_map>

#include "libslic3r/GCode/ExtrusionFilter.hpp"
#include "libslic3r/GCode/IslandOrdering.hpp"
#include "libslic3r/GCode/LayerPlanner.hpp"
#include "libslic3r/GCode/LoopDirection.hpp"
#include "libslic3r/GCode/SmoothPath.hpp"
#include <algorithm>

#include <spdlog/spdlog.h>

#include "libslic3r/Purge.hpp"
#include "libslic3r/ResumePlanner.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "Slic3r/Biz/Algorithms/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionRole.hpp"
#include "libslic3r/GCode/WipeTowerIntegration.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/LayerRegion.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r::GCode::ExtrusionOrder {

bool is_overriden(const ExtrusionEntityCollection &eec, const LayerTools &layer_tools, const std::size_t instance_id) {
    return layer_tools.wiping_extrusions().get_extruder_override(&eec, instance_id) > -1;
}

int get_extruder_id(
    const ExtrusionEntityCollection &eec,
    const LayerTools &layer_tools,
    const PrintRegion &region,
    const std::size_t instance_id
) {
    if (is_overriden(eec, layer_tools, instance_id)) {
        return layer_tools.wiping_extrusions().get_extruder_override(&eec, instance_id);
    }

    const int extruder_id = layer_tools.extruder(eec, region);
    if (! layer_tools.has_extruder(extruder_id)) {
        // Extruder is not in layer_tools - we'll print it by last extruder on this layer (could
        // happen e.g. when a wiping object is taller than others - dontcare extruders are
        // eradicated from layer_tools)
        return layer_tools.extruders.back();
    }
    return extruder_id;
}

Point get_gcode_point(const InstancePoint &point, const Point &offset) {
    return point.local_point + offset;
}

InstancePoint get_instance_point(const Point &point, const Point &offset) {
    return {point - offset};
}

std::optional<Point> get_gcode_point(const std::optional<InstancePoint> &point, const Point &offset) {
    if (point) {
        return get_gcode_point(*point, offset);
    }
    return std::nullopt;
}

std::optional<InstancePoint> get_instance_point(const std::optional<Point> &point, const Point &offset) {
    if (point) {
        return get_instance_point(*point, offset);
    }
    return std::nullopt;
}

using ExtractEntityPredicate = std::function<bool(const ExtrusionEntityCollection&, const PrintRegion&)>;

ExtrusionEntitiesPtr extract_infill_extrusions(
    const PrintRegion &region,
    const ExtrusionEntityCollection &fills,
    const LayerExtrusionRanges::const_iterator& begin,
    const LayerExtrusionRanges::const_iterator& end,
    const ExtractEntityPredicate &should_pick_extrusion
) {
    ExtrusionEntitiesPtr result;
    for (auto it = begin; it != end; ++ it) {
        assert(it->region() == begin->region());
        const LayerExtrusionRange &range{*it};
        for (uint32_t fill_id : range) {
            assert(dynamic_cast<ExtrusionEntityCollection*>(fills.entities[fill_id]));

            auto *eec{static_cast<ExtrusionEntityCollection*>(fills.entities[fill_id])};
            if (eec == nullptr || eec->empty() || !should_pick_extrusion(*eec, region)) {
                continue;
            }

            if (eec->can_reverse()) {
                // Flatten the infill collection for better path planning.
                for (auto *ee : eec->entities) {
                    result.emplace_back(ee);
                }
            } else {
                result.emplace_back(eec);
            }
        }
    }
    return result;
}

namespace {
// Asks the print's extrusion filter about a freshly smoothed path.
//
// The path is described by the role of its first element; a smooth path never mixes
// roles. Dropping a path must also undo the head position that producing it advanced,
// otherwise the seam of the next path would be anchored on an extrusion that is not
// going to be printed.
bool keep_smoothed_path(
    const Print &print,
    const GCode::SmoothPath &path,
    const Layer &layer,
    const unsigned extruder_id
) {
    if (!print.extrusion_filter || path.empty()) {
        return true;
    }

    const GCode::ExtrusionFilter::PathInfo info{
        extrusion_role_to_gcode_extrusion_role(path.front().path_attributes.role),
        unscaled<double>(GCode::length(path)),
        layer.id(),
        layer.print_z,
        extruder_id
    };
    return GCode::ExtrusionFilter::keep(print.extrusion_filter, info);
}

/**
 * @brief The outline of the layer below, clipped to this island.
 *
 * Empty optional on the first layer, where there is no layer below and nothing overhangs -
 * as against an empty Polygons, which means this island has nothing at all under it.
 */
std::optional<Polygons> lower_layer_outline(const Layer &layer, const LayerIsland &island)
{
    if (layer.lower_layer == nullptr) {
        return std::nullopt;
    }
    const BoundingBox bbox =
        Biz::Algorithms::BoundingBox::inflated(get_extents(island.boundary), SCALED_EPSILON);
    return ClipperUtils::clip_clipper_polygons_with_subject_bbox(layer.lower_layer->lslices, bbox);
}

/**
 * @brief How much of @p loop lies outside @p lower, in millimetres.
 *
 * Measured against the outline of the layer below with no allowance made for the width of the
 * bead, which is the same test OrcaSlicer's overhang_reverse makes at its default threshold.
 * Deliberately not the slicer's own overhang perimeter marking: that compares against the
 * lower layer grown by half a nozzle, so it says no on any slope the nozzle still partly
 * overlaps - which on a 31 degree wall at 0.2 mm layers is every layer of it.
 */
double length_outside(const ExtrusionLoop &loop, const std::optional<Polygons> &lower)
{
    if (!lower.has_value()) {
        return 0.;
    }
    Polylines subject;
    loop.collect_polylines(subject);
    double total = 0.;
    for (const Polyline &outside : diff_pl(subject, *lower)) {
        total += unscaled<double>(outside.length());
    }
    return total;
}

/** @brief What GCode::LoopDirection has to be told about one wall loop, in millimetres. */
struct LoopMeasure
{
    double length{0.};
    double overhang_length{0.};
    bool external{false};
    int perimeter_index{-1};
};

LoopMeasure measure_loop(const ExtrusionLoop &loop, const double overhang_length)
{
    LoopMeasure measure;
    measure.overhang_length = overhang_length;
    for (const ExtrusionPath &path : loop.paths) {
        measure.length += unscaled<double>(path.length());
        if (path.role().is_external_perimeter()) {
            measure.external = true;
        }
        if (measure.perimeter_index < 0 && path.attributes().perimeter_index.has_value()) {
            measure.perimeter_index = int(*path.attributes().perimeter_index);
        }
    }
    return measure;
}

/**
 * @brief Wall outside the layer below, per loop and summed over the island, in millimetres.
 *
 * Both numbers are wanted - a plugin is offered its loop's own and its island's - and the
 * island's cannot be had without measuring every loop, so they are measured once, here, and
 * the per loop answers are kept rather than clipped a second time when each loop comes round.
 */
struct IslandOverhang
{
    std::unordered_map<const ExtrusionEntity *, double> per_loop;
    double total{0.};
};

IslandOverhang measure_island_overhang(
    const LayerRegion &layerm, const LayerIsland &island, const std::optional<Polygons> &lower
)
{
    IslandOverhang measured;
    for (uint32_t perimeter_id : island.perimeters) {
        const auto *eec =
            dynamic_cast<const ExtrusionEntityCollection *>(layerm.perimeters().entities[perimeter_id]);
        if (eec == nullptr) {
            continue;
        }
        for (const ExtrusionEntity *ee : *eec) {
            if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(ee); loop != nullptr) {
                const double outside = length_outside(*loop, lower);
                measured.per_loop.emplace(ee, outside);
                measured.total += outside;
            }
        }
    }
    return measured;
}
} // namespace

std::vector<Perimeter> extract_perimeter_extrusions(
    const Print &print,
    const Layer &layer,
    const LayerIsland &island,
    const ExtractEntityPredicate &should_pick_extrusion,
    const unsigned extruder_id,
    const Point &offset,
    std::optional<Point> &previous_position,
    const PathSmoothingFunction &smooth_path
) {
    std::vector<Perimeter> result;

    const LayerRegion &layerm = *layer.get_region(island.perimeters.region());
    const PrintRegion &region = print.get_print_region(layerm.region().print_region_id());

    // A plugin may want some of these loops walked the other way round - see
    // GCode::LoopDirection. How much of the island hangs over air is part of what it is
    // offered and has to be known before the first loop is answered for, so it is measured up
    // front. Nothing here runs when no strategy is installed.
    const LoopDirection::Strategy &loop_direction = print.loop_direction_strategy;
    const IslandOverhang overhang = loop_direction ?
        measure_island_overhang(layerm, island, lower_layer_outline(layer, island)) :
        IslandOverhang{};

    for (uint32_t perimeter_id : island.perimeters) {
        // Extrusions inside islands are expected to be ordered already.
        // Don't reorder them.
        assert(dynamic_cast<ExtrusionEntityCollection*>(layerm.perimeters().entities[perimeter_id]));
        auto *eec = static_cast<ExtrusionEntityCollection*>(layerm.perimeters().entities[perimeter_id]);
        if (eec == nullptr || eec->empty() || !should_pick_extrusion(*eec, region)) {
            continue;
        }

        for (ExtrusionEntity *ee : *eec) {
            if (ee != nullptr) {
                std::optional<InstancePoint> last_position{get_instance_point(previous_position, offset)};
                bool reverse_loop{false};
                if (auto loop = dynamic_cast<const ExtrusionLoop *>(ee)) {
                    const bool is_hole = loop->is_clockwise();
                    reverse_loop = print.config().get<bool>("prefer_clockwise_movements") ? !is_hole : is_hole;
                    if (loop_direction) {
                        // The loop is stored clockwise when it is a hole, so this is the
                        // direction the nozzle would actually travel in.
                        const bool stock_cw = reverse_loop ? !is_hole : is_hole;
                        const auto found = overhang.per_loop.find(ee);
                        const LoopMeasure measure = measure_loop(
                            *loop, found == overhang.per_loop.end() ? 0. : found->second
                        );
                        const LoopDirection::LoopInfo info{
                            layer.id(),
                            layer.print_z,
                            extruder_id,
                            measure.external,
                            is_hole,
                            measure.perimeter_index,
                            measure.length,
                            measure.overhang_length,
                            overhang.total,
                            stock_cw ? LoopDirection::Direction::Cw : LoopDirection::Direction::Ccw
                        };
                        if (const std::optional<LoopDirection::Direction> planned =
                                LoopDirection::plan_direction(loop_direction, info);
                            planned.has_value()) {
                            reverse_loop =
                                (*planned == LoopDirection::Direction::Cw) != is_hole;
                        }
                    }
                }
                const std::optional<Point> position_before{previous_position};
                auto [path, wipe_offset]{smooth_path(&layer, &region, ExtrusionEntityReference{*ee, reverse_loop}, extruder_id, last_position)};
                previous_position = get_gcode_point(last_position, offset);
                if (!keep_smoothed_path(print, path, layer, extruder_id)) {
                    previous_position = position_before;
                } else if (!path.empty()) {
                    result.push_back(Perimeter{std::move(path), reverse_loop, ee, wipe_offset});
                }
            }
        }
    }

    return result;
}

std::vector<ExtrusionEntityReference> sort_fill_extrusions(const ExtrusionEntitiesPtr &fills, const Point* start_near) {
    if (fills.empty()) {
        return {};
    }
    std::vector<ExtrusionEntityReference> sorted_extrusions;

    for (const ExtrusionEntityReference &fill : chain_extrusion_references(fills, start_near)) {
        if (auto *eec = dynamic_cast<const ExtrusionEntityCollection*>(&fill.extrusion_entity()); eec) {
            for (const ExtrusionEntityReference &ee : chain_extrusion_references(*eec, start_near, fill.flipped())) {
                sorted_extrusions.push_back(ee);
            }
        } else {
            sorted_extrusions.push_back(fill);
        }
    }
    return sorted_extrusions;
}

/** @brief One run of an island's fill: the ranges of it that belong to one region. */
struct FillRun
{
    LayerExtrusionRanges::const_iterator begin;
    LayerExtrusionRanges::const_iterator end;
};

/** @brief Splits an island's fill into the runs the slicer prints one after another. */
std::vector<FillRun> fill_runs(const LayerIsland &island)
{
    std::vector<FillRun> runs;
    for (auto it = island.fills.begin(); it != island.fills.end();) {
        auto it_end = it;
        for (++ it_end; it_end != island.fills.end() && it->region() == it_end->region(); ++ it_end) ;
        runs.push_back(FillRun{it, it_end});
        it = it_end;
    }
    return runs;
}

/** @brief Collects, sorts and smooths one run of fill. Nothing when none of it is printed. */
std::optional<InfillRange> extract_fill_run(
    const Print &print,
    const Layer &layer,
    const FillRun &run,
    const Point &offset,
    std::optional<Point> &previous_position,
    const ExtractEntityPredicate &should_pick_extrusion,
    const PathSmoothingFunction &smooth_path,
    const unsigned extruder_id
) {
    {
        const auto it = run.begin;
        const auto it_end = run.end;
        const LayerRegion &layerm = *layer.get_region(it->region());
        // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be unique to a PrintObject, they would not
        // identify the content of PrintRegion accross the whole print uniquely. Translate to a Print specific PrintRegion.
        const PrintRegion &region = print.get_print_region(layerm.region().print_region_id());

        ExtrusionEntitiesPtr extrusions{extract_infill_extrusions(
            region,
            layerm.fills(),
            it,
            it_end,
            should_pick_extrusion
        )};

        const std::optional<InstancePoint> previous_instance_point{get_instance_point(previous_position, offset)};
        const Point* start_near{previous_instance_point ? &(previous_instance_point->local_point) : nullptr};
        const ExtrusionEntityReferences sorted_extrusions{sort_fill_extrusions(extrusions, start_near)};

        std::vector<SmoothPath> paths;
        for (const ExtrusionEntityReference &extrusion_reference : sorted_extrusions) {
            // previous_position is only advanced below, so skipping a rejected path
            // leaves the head where the last kept extrusion put it.
            std::optional<InstancePoint> last_position{get_instance_point(previous_position, offset)};
            auto [path, _]{smooth_path(&layer, &region, extrusion_reference, extruder_id, last_position)};
            if (!keep_smoothed_path(print, path, layer, extruder_id)) {
                continue;
            }
            if (!path.empty()) {
                paths.push_back(std::move(path));
            }
            previous_position = get_gcode_point(last_position, offset);
        }
        if (!paths.empty()) {
            return InfillRange{std::move(paths), &region};
        }
    }
    return std::nullopt;
}

std::vector<InfillRange> extract_infill_ranges(
    const Print &print,
    const Layer &layer,
    const LayerIsland &island,
    const Point &offset,
    std::optional<Point> &previous_position,
    const ExtractEntityPredicate &should_pick_extrusion,
    const PathSmoothingFunction &smooth_path,
    const unsigned extruder_id
) {
    std::vector<InfillRange> result;
    for (const FillRun &run : fill_runs(island)) {
        if (std::optional<InfillRange> range = extract_fill_run(
                print, layer, run, offset, previous_position, should_pick_extrusion, smooth_path,
                extruder_id)) {
            result.push_back(std::move(*range));
        }
    }
    return result;
}

// Returns LayerIslands ordered by the shortest distance.
std::vector<std::reference_wrapper<const LayerIsland>> get_ordered_islands(
    const LayerSlice &lslice,
    const std::optional<Point> &previous_position
) {
    std::vector<std::reference_wrapper<const LayerIsland>> islands_to_order;
    for (const LayerIsland &island : lslice.islands) {
        islands_to_order.emplace_back(island);
    }

    chain_and_reorder_layer_islands(islands_to_order, previous_position.has_value() ? std::addressof(*previous_position) : nullptr);
    return islands_to_order;
}

/** @brief Sums what a set of extrusions is, for a plugin that has to choose between them. */
void describe_entity(
    const ExtrusionEntity *entity, const Point &offset, LayerPlanner::GroupInfo &into,
    std::map<Domain::GCodeExtrusionRole, LayerPlanner::RoleShare> &by_role, Points &points)
{
    if (entity == nullptr) {
        return;
    }
    // A collection has no length and no single role of its own - it is asked about its
    // children, which is what the G-code writer does with it as well.
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : collection->entities) {
            describe_entity(child, offset, into, by_role, points);
        }
        return;
    }

    Polylines polylines;
    entity->collect_polylines(polylines);
    double length = 0.;
    for (const Polyline &polyline : polylines) {
        length += unscaled<double>(polyline.length());
        for (const Point &point : polyline.points) {
            points.emplace_back(point + offset);
        }
    }
    if (length <= 0.) {
        return;
    }
    const double volume = entity->total_volume();
    into.length += length;
    into.volume += volume;

    const Domain::GCodeExtrusionRole role = extrusion_role_to_gcode_extrusion_role(entity->role());
    LayerPlanner::RoleShare &share =
        by_role.try_emplace(role, LayerPlanner::RoleShare{role, 0., 0.}).first->second;
    share.length += length;
    share.volume += volume;
}

/**
 * @brief Describes a set of extrusions as one group, breakdown by role included.
 *
 * The group is left with its shares sorted longest first, so that the dominant role is simply
 * the first of them and a plugin reading the list in order reads it in the order that matters.
 */
void describe_entities(
    const ExtrusionEntitiesPtr &entities, const Point &offset, LayerPlanner::GroupInfo &into)
{
    Points points;
    std::map<Domain::GCodeExtrusionRole, LayerPlanner::RoleShare> by_role;
    for (const ExtrusionEntity *entity : entities) {
        describe_entity(entity, offset, into, by_role, points);
    }
    if (!points.empty()) {
        into.bbox = Biz::Algorithms::BoundingBox::construct(points);
    }

    into.roles.reserve(by_role.size());
    for (const auto &share : by_role | std::views::values) {
        into.roles.push_back(share);
    }
    std::sort(
        into.roles.begin(), into.roles.end(),
        [](const LayerPlanner::RoleShare &a, const LayerPlanner::RoleShare &b) {
            return a.length > b.length;
        }
    );
    into.role = into.roles.empty() ? Domain::GCodeExtrusionRole::None : into.roles.front().role;
}

/** @brief One group of a layer slice, and where its extrusions are to be found. */
struct PlannedGroup
{
    std::size_t island;
    bool perimeters;
    /** @brief Which run of the island's fill, when this is a fill group. */
    std::size_t run;
};

/**
 * @brief Collects a layer slice's extrusions in the order a LayerPlanner asked for.
 *
 * Empty when there is nothing to plan or the planner leaves the order alone, and then the
 * caller collects them the way it always did - which is what makes the stock G-code with no
 * plugin installed structurally identical rather than merely equal.
 *
 * Each group becomes its own entry: the G-code writer prints whichever half of an entry is not
 * empty, so a group of walls and a group of fill can be put anywhere in the sequence, including
 * between two groups of another island.
 */
std::vector<IslandExtrusions> extract_in_planned_order(
    const std::vector<std::reference_wrapper<const LayerIsland>> &ordered_islands,
    const Print &print,
    const Layer &layer,
    const ExtractEntityPredicate &should_pick_extrusion,
    const PathSmoothingFunction &smooth_path,
    const Point &offset,
    const unsigned extruder_id,
    std::optional<Point> &previous_position
) {
    const auto should_pick_infill = [&should_pick_extrusion](const ExtrusionEntityCollection &eec, const PrintRegion &region) {
        return should_pick_extrusion(eec, region) && eec.role() != ExtrusionRole::Ironing;
    };

    std::vector<LayerPlanner::GroupInfo> groups;
    std::vector<PlannedGroup> plan;
    std::vector<std::vector<FillRun>> runs_of_island;
    runs_of_island.reserve(ordered_islands.size());

    for (std::size_t i = 0; i < ordered_islands.size(); ++i) {
        const LayerIsland &island = ordered_islands[i].get();
        const LayerRegion &layerm = *layer.get_region(island.perimeters.region());
        const PrintRegion &region = print.get_print_region(layerm.region().print_region_id());

        {
            ExtrusionEntitiesPtr walls;
            for (uint32_t perimeter_id : island.perimeters) {
                auto *eec = dynamic_cast<ExtrusionEntityCollection *>(layerm.perimeters().entities[perimeter_id]);
                if (eec == nullptr || eec->empty() || !should_pick_extrusion(*eec, region)) {
                    continue;
                }
                for (ExtrusionEntity *entity : *eec) {
                    if (entity != nullptr) {
                        walls.emplace_back(entity);
                    }
                }
            }
            LayerPlanner::GroupInfo info{i, LayerPlanner::GroupKind::Perimeters,
                                         Domain::GCodeExtrusionRole::None, 0., 0., BoundingBox{}};
            describe_entities(walls, offset, info);
            if (info.length > 0.) {
                groups.push_back(std::move(info));
                plan.push_back(PlannedGroup{i, true, 0});
            }
        }

        runs_of_island.push_back(fill_runs(island));
        const std::vector<FillRun> &runs = runs_of_island.back();
        for (std::size_t r = 0; r < runs.size(); ++r) {
            const LayerRegion &fill_layerm = *layer.get_region(runs[r].begin->region());
            const PrintRegion &fill_region =
                print.get_print_region(fill_layerm.region().print_region_id());
            const ExtrusionEntitiesPtr fills{extract_infill_extrusions(
                fill_region, fill_layerm.fills(), runs[r].begin, runs[r].end, should_pick_infill)};
            LayerPlanner::GroupInfo info{i, LayerPlanner::GroupKind::Fill,
                                         Domain::GCodeExtrusionRole::None, 0., 0., BoundingBox{}};
            describe_entities(fills, offset, info);
            if (info.length > 0.) {
                groups.push_back(std::move(info));
                plan.push_back(PlannedGroup{i, false, r});
            }
        }
    }

    if (groups.size() < 2) {
        return {};
    }

    const LayerPlanner::LayerContext context{layer.id(), layer.print_z, extruder_id};
    const std::vector<std::size_t> order =
        LayerPlanner::order_groups(print.layer_planner, groups, context);
    bool unchanged = true;
    for (std::size_t i = 0; i < order.size() && unchanged; ++i) {
        unchanged = order[i] == i;
    }
    if (unchanged) {
        return {};
    }

    std::vector<IslandExtrusions> result;
    result.reserve(order.size());
    for (const std::size_t position : order) {
        const PlannedGroup &group = plan[position];
        const LayerIsland &island = ordered_islands[group.island].get();
        if (group.perimeters) {
            const LayerRegion &layerm = *layer.get_region(island.perimeters.region());
            const PrintRegion &region = print.get_print_region(layerm.region().print_region_id());
            std::vector<Perimeter> perimeters{extract_perimeter_extrusions(
                print, layer, island, should_pick_extrusion, extruder_id, offset,
                previous_position, smooth_path)};
            if (!perimeters.empty()) {
                result.push_back(IslandExtrusions{&region, std::move(perimeters), {}, false});
            }
        } else if (std::optional<InfillRange> range = extract_fill_run(
                       print, layer, runs_of_island[group.island][group.run], offset,
                       previous_position, should_pick_infill, smooth_path, extruder_id)) {
            const PrintRegion *region = range->region;
            std::vector<InfillRange> ranges;
            ranges.push_back(std::move(*range));
            result.push_back(IslandExtrusions{region, {}, std::move(ranges), true});
        }
    }
    return result;
}

std::vector<IslandExtrusions> extract_island_extrusions(
    const LayerSlice &lslice,
    const Print &print,
    const Layer &layer,
    const ExtractEntityPredicate &should_pick_extrusion,
    const PathSmoothingFunction &smooth_path,
    const Point &offset,
    const unsigned extruder_id,
    std::optional<Point> &previous_position,
    const bool force_infill_first
) {
    const auto should_pick_infill = [&should_pick_extrusion](const ExtrusionEntityCollection &eec, const PrintRegion &region) {
        return should_pick_extrusion(eec, region) && eec.role() != ExtrusionRole::Ironing;
    };

    std::vector<std::reference_wrapper<const LayerIsland>> ordered_islands = get_ordered_islands(lslice, previous_position);

    // A plugin may want these groups printed in some other order - the deck of a hull before
    // the wall that runs past it, say. Described and asked before anything is collected,
    // because collecting advances the head and every seam and travel after it follows from
    // where the head is.
    if (print.layer_planner) {
        if (std::vector<IslandExtrusions> planned = extract_in_planned_order(
                ordered_islands, print, layer, should_pick_extrusion, smooth_path, offset,
                extruder_id, previous_position);
            !planned.empty()) {
            return planned;
        }
    }

    std::vector<IslandExtrusions> result;
    for (const LayerIsland &island : ordered_islands) {
        const LayerRegion &layerm = *layer.get_region(island.perimeters.region());
        // PrintObjects own the PrintRegions, thus the pointer to PrintRegion would be
        // unique to a PrintObject, they would not identify the content of PrintRegion
        // accross the whole print uniquely. Translate to a Print specific PrintRegion.
        const PrintRegion &region = print.get_print_region(layerm.region().print_region_id());

        result.push_back(IslandExtrusions{&region});
        IslandExtrusions &island_extrusions{result.back()};
        island_extrusions.infill_first = force_infill_first
            || print.config().get<bool>("infill_first");

        if (island_extrusions.infill_first) {
            island_extrusions.infill_ranges = extract_infill_ranges(
                print, layer, island, offset, previous_position, should_pick_infill, smooth_path, extruder_id
            );

            island_extrusions.perimeters = extract_perimeter_extrusions(print, layer, island, should_pick_extrusion, extruder_id, offset, previous_position, smooth_path);
        } else {
            island_extrusions.perimeters = extract_perimeter_extrusions(print, layer, island, should_pick_extrusion, extruder_id, offset, previous_position, smooth_path);

            island_extrusions.infill_ranges = extract_infill_ranges(
                print, layer, island, offset, previous_position, should_pick_infill, smooth_path, extruder_id
            );
        }
    }
    return result;
}

std::vector<InfillRange> extract_ironing_extrusions(
    const LayerSlice &lslice,
    const Print &print,
    const Layer &layer,
    const ExtractEntityPredicate &should_pick_extrusion,
    const PathSmoothingFunction &smooth_path,
    const Point &offset,
    const unsigned extruder_id,
    std::optional<Point> &previous_position
) {
    const auto should_pick_ironing = [&should_pick_extrusion](const auto &eec, const auto &region) {
        return should_pick_extrusion(eec, region) && eec.role() == ExtrusionRole::Ironing;
    };

    std::vector<std::reference_wrapper<const LayerIsland>> ordered_islands = get_ordered_islands(lslice, previous_position);

    std::vector<InfillRange> result;
    for (const LayerIsland &island : ordered_islands) {
        const std::vector<InfillRange> ironing_ranges{extract_infill_ranges(
            print, layer, island, offset, previous_position, should_pick_ironing, smooth_path, extruder_id
        )};
        result.insert(
            result.end(), ironing_ranges.begin(), ironing_ranges.end()
        );
    }
    return result;
}

std::vector<SliceExtrusions> get_slices_extrusions(
    const Print &print,
    const Layer &layer,
    const ExtractEntityPredicate &should_pick_extrusion,
    const PathSmoothingFunction &smooth_path,
    const Point &offset,
    const unsigned extruder_id,
    std::optional<Point> &previous_position,
    const std::vector<std::size_t>* selected_indices,
    const bool force_infill_first
) {
    // Note: ironing.
    // FIXME move ironing into the loop above over LayerIslands?
    // First Ironing changes extrusion rate quickly, second single ironing may be done over
    // multiple perimeter regions. Ironing in a second phase is safer, but it may be less
    // efficient.

    std::vector<SliceExtrusions> result;

    // The print order of the layer's islands goes through the extension point. With no
    // strategy installed on the print this returns layer.lslice_indices_sorted_by_print_order
    // unchanged, so the stock behaviour is preserved exactly.
    const std::vector<size_t> island_order{IslandOrdering::order_islands(
        print.island_ordering_strategy, layer, offset, extruder_id, previous_position,
        selected_indices
    )};

    for (size_t idx : island_order) {
        const LayerSlice &lslice = layer.lslices_ex[idx];
        std::vector<IslandExtrusions> island_extrusions{extract_island_extrusions(
            lslice, print, layer, should_pick_extrusion, smooth_path, offset, extruder_id,
            previous_position, force_infill_first
        )};
        std::vector<InfillRange> ironing_extrusions{extract_ironing_extrusions(
            lslice, print, layer, should_pick_extrusion, smooth_path, offset, extruder_id, previous_position
        )};
        if (!island_extrusions.empty() || !ironing_extrusions.empty()) {
            result.emplace_back(
                SliceExtrusions{std::move(island_extrusions), std::move(ironing_extrusions)}
            );
        }
    }
    return result;
}

unsigned translate_support_extruder(
    const int configured_extruder,
    const LayerTools &layer_tools,
    const std::vector<bool> &is_soluable
) {
    if (configured_extruder <= 0) {
        // Some support will be printed with "don't care" material, preferably non-soluble.
        // Is the current extruder assigned a soluble filament?
        auto it_nonsoluble = std::find_if(layer_tools.extruders.begin(), layer_tools.extruders.end(),
            [&is_soluable](unsigned int extruder_id) { return ! is_soluable.at(extruder_id); });
        // There should be a non-soluble extruder available.
        assert(it_nonsoluble != layer_tools.extruders.end());
        return it_nonsoluble == layer_tools.extruders.end() ? layer_tools.extruders.front() : *it_nonsoluble;
    } else {
        return configured_extruder - 1;
    }
}

std::vector<SupportPath> get_support_extrusions(
    const unsigned int extruder_id,
    const GCode::ObjectLayerToPrint &layer_to_print,
    unsigned int support_extruder,
    unsigned int interface_extruder,
    const PathSmoothingFunction &smooth_path,
    std::optional<Point> &previous_position
) {
    if (const SupportLayer &support_layer = *layer_to_print.support_layer;
        !support_layer.support_fills.entities.empty()) {
        ExtrusionRole role = support_layer.support_fills.role();
        bool has_support = role.is_mixed() || role.is_support_base();
        bool has_interface = role.is_mixed() || role.is_support_interface();

        bool extrude_support = has_support && support_extruder == extruder_id;
        bool extrude_interface = has_interface && interface_extruder == extruder_id;

        if (extrude_support || extrude_interface) {
            ExtrusionEntitiesPtr entities_cache;
            const ExtrusionEntitiesPtr &entities = extrude_support && extrude_interface ?
                support_layer.support_fills.entities :
                entities_cache;
            if (!extrude_support || !extrude_interface) {
                auto role = extrude_support ? ExtrusionRole::SupportMaterial :
                                              ExtrusionRole::SupportMaterialInterface;
                entities_cache.reserve(support_layer.support_fills.entities.size());
                for (ExtrusionEntity *ee : support_layer.support_fills.entities)
                    // A mixed collection is an interface fill with an extra pass over it,
                    // which belongs to whichever extruder prints the interface.
                    if (ee->role() == role
                        || (ee->role().is_mixed() && role == ExtrusionRole::SupportMaterialInterface))
                        entities_cache.emplace_back(ee);
            }
            std::vector<SupportPath> paths;
            for (const ExtrusionEntityReference &entity_reference : chain_extrusion_references(entities)) {
                auto collection{dynamic_cast<const ExtrusionEntityCollection *>(&entity_reference.extrusion_entity())};
                if (collection != nullptr) {
                    for (const ExtrusionEntity * sub_entity : *collection) {
                        // Read the role off the path rather than off the collection: a
                        // collection holding an interface fill and an extra pass over it
                        // has no single role, and the two are printed differently.
                        const ExtrusionRole role{sub_entity->role()};
                        std::optional<InstancePoint> last_position{get_instance_point(previous_position, {0, 0})};
                        auto [path, _]{smooth_path(nullptr, nullptr, {*sub_entity, entity_reference.flipped()}, extruder_id, last_position)};
                        if (!path.empty()) {
                            paths.push_back({std::move(path),
                                role != ExtrusionRole::SupportMaterial,
                                role == ExtrusionRole::Ironing,
                                role == ExtrusionRole::SolidInfill});
                        }
                        previous_position = get_gcode_point(last_position, {0, 0});
                    }
                } else {
                    const ExtrusionRole role{entity_reference.extrusion_entity().role()};
                    std::optional<InstancePoint> last_position{get_instance_point(previous_position, {0, 0})};
                    auto [path, _]{smooth_path(nullptr, nullptr, entity_reference, extruder_id, last_position)};
                    if (!path.empty()) {
                        paths.push_back({std::move(path), role != ExtrusionRole::SupportMaterial, role == ExtrusionRole::Ironing});
                    }
                    previous_position = get_gcode_point(last_position, {0, 0});
                }
            }
            return paths;
        }
    }
    return {};
}

std::vector<OverridenExtrusions> get_overriden_extrusions(
    const Print &print,
    const GCode::ObjectsLayerToPrint &layers,
    const LayerTools &layer_tools,
    const std::vector<InstanceToPrint> &instances_to_print,
    const unsigned int extruder_id,
    const PathSmoothingFunction &smooth_path,
    std::optional<Point> &previous_position
) {
    std::vector<OverridenExtrusions> result;

    for (const InstanceToPrint &instance : instances_to_print) {
        if (const Layer *layer = layers[instance.object_layer_to_print_id].object_layer; layer) {
            const auto should_pick_extrusion = [&layer_tools, &instance, &extruder_id](const ExtrusionEntityCollection &entity_collection,
                                       const PrintRegion &region) {
                if (!is_overriden(entity_collection, layer_tools, instance.instance_id)) {
                    return false;
                }

                if (get_extruder_id(
                        entity_collection, layer_tools, region, instance.instance_id
                    ) != static_cast<int>(extruder_id)) {
                    return false;
                }
                return true;
            };

            const PrintObject &print_object{instance.print_object};
            const Point offset{print_object.instances()[instance.instance_id].shift()};

            std::vector<SliceExtrusions> slices_extrusions{get_slices_extrusions(
                print, *layer, should_pick_extrusion, smooth_path, offset, extruder_id,
                previous_position, layers[instance.object_layer_to_print_id].island_indices ?
                    &*layers[instance.object_layer_to_print_id].island_indices : nullptr,
                layers[instance.object_layer_to_print_id].force_infill_first
            )};
            result.push_back({offset, std::move(slices_extrusions)});
        }
    }
    return result;
}

ResumePlanner::Interruption to_interruption(const Domain::CustomGCode::Type type)
{
    using Domain::CustomGCode::Type;
    switch (type) {
    case Type::ColorChange: return ResumePlanner::Interruption::ColorChange;
    case Type::ToolChange:  return ResumePlanner::Interruption::ToolChange;
    case Type::Template:    return ResumePlanner::Interruption::Template;
    case Type::Custom:      return ResumePlanner::Interruption::Custom;
    case Type::PausePrint:
    default:                return ResumePlanner::Interruption::Pause;
    }
}

// Plain extrusion to spend before anything else on this layer, offered to the resume planner
// because the print is interrupted here. Empty when no planner is installed or when it
// declines, which is what keeps the stock output untouched.
std::vector<SmoothPath> extract_resume_purge(
    const Print &print,
    const Layer &layer,
    const Domain::CustomGCode::Item &interruption,
    const Point &offset,
    const unsigned extruder_id,
    const PathSmoothingFunction &smooth_path,
    std::optional<Point> &previous_position
) {
    std::vector<SmoothPath> paths;
    if (!print.resume_planner || layer.regions().empty()) {
        return paths;
    }
    const LayerRegion &layerm = *layer.regions().front();
    const Flow flow = layerm.flow(frSolidInfill);
    if (flow.mm3_per_mm() <= 0. || flow.width() <= 0.) {
        return paths;
    }

    const double area = Purge::spare_area(layer, flow);
    const ResumePlanner::ResumeInfo resume{
        to_interruption(interruption.type),
        layer.id(),
        layer.print_z,
        extruder_id,
        layer.height,
        flow.nozzle_diameter(),
        area,
        area / flow.width() * flow.mm3_per_mm()
    };
    const std::optional<ResumePlanner::Plan> plan{
        ResumePlanner::plan_resume(print.resume_planner, resume)};
    if (!plan.has_value()) {
        return paths;
    }

    ExtrusionEntitiesPtr purge{Purge::generate(layer, flow, plan->purge_volume)};
    if (purge.empty()) {
        SPDLOG_INFO(
            "Resume planner asked for {:.1f} mm3 on layer {}, which had no room to spare",
            plan->purge_volume, layer.id()
        );
        return paths;
    }
    const PrintRegion &region = print.get_print_region(layerm.region().print_region_id());
    for (const ExtrusionEntity *entity : purge) {
        std::optional<InstancePoint> last_position{get_instance_point(previous_position, offset)};
        auto [path, _]{smooth_path(&layer, &region, {*entity, false}, extruder_id, last_position)};
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
        previous_position = get_gcode_point(last_position, offset);
    }
    for (ExtrusionEntity *entity : purge) {
        delete entity;
    }
    return paths;
}

std::vector<NormalExtrusions> get_normal_extrusions(
    const Print &print,
    const GCode::ObjectsLayerToPrint &layers,
    const LayerTools &layer_tools,
    const std::vector<InstanceToPrint> &instances_to_print,
    const unsigned int extruder_id,
    const PathSmoothingFunction &smooth_path,
    std::optional<Point> &previous_position
) {
    std::vector<NormalExtrusions> result;

    for (std::size_t i{0}; i < instances_to_print.size(); ++i) {
        const InstanceToPrint &instance{instances_to_print[i]};
        const PrintObject &print_object = instance.print_object;
        const Point offset = print_object.instances()[instance.instance_id].shift();

        result.emplace_back();
        result.back().instance_offset = offset;

        if (layers[instance.object_layer_to_print_id].support_layer != nullptr) {
            result.back().support_extrusions = get_support_extrusions(
                extruder_id,
                layers[instance.object_layer_to_print_id],
                translate_support_extruder(instance.print_object.config().get<int>("support_material_extruder"), layer_tools, print.config().get<std::vector<bool>>("filament_soluble")),
                translate_support_extruder(instance.print_object.config().get<int>("support_material_interface_extruder"), layer_tools, print.config().get<std::vector<bool>>("filament_soluble")),
                smooth_path,
                previous_position
            );
        }

        if (const Layer *layer = layers[instance.object_layer_to_print_id].object_layer; layer) {
            // Only the first instance that finds room purges; one turnover of the melt serves
            // every copy on the plate, and the rest would be filament thrown at a solved
            // problem.
            if (layer_tools.custom_gcode != nullptr
                && std::all_of(result.begin(), result.end(), [](const NormalExtrusions &e) {
                       return e.resume_purge.empty();
                   })) {
                result.back().resume_purge = extract_resume_purge(
                    print, *layer, *layer_tools.custom_gcode, offset, extruder_id, smooth_path,
                    previous_position
                );
            }

            const auto should_pick_extrusion{[&layer_tools, &instance, &extruder_id](const ExtrusionEntityCollection &entity_collection, const PrintRegion &region){
                if (is_overriden(entity_collection, layer_tools, instance.instance_id)) {
                    return false;
                }

                if (get_extruder_id(entity_collection, layer_tools, region, instance.instance_id) != static_cast<int>(extruder_id)) {
                    return false;
                }
                return true;
            }};

            result.back().slices_extrusions = get_slices_extrusions(
                print,
                *layer,
                should_pick_extrusion,
                smooth_path,
                offset,
                extruder_id,
                previous_position,
                layers[instance.object_layer_to_print_id].island_indices ?
                    &*layers[instance.object_layer_to_print_id].island_indices : nullptr,
                layers[instance.object_layer_to_print_id].force_infill_first
            );
        }
    }
    return result;
}

bool is_empty(const std::vector<SliceExtrusions> &extrusions) {
    for (const SliceExtrusions &slice_extrusions : extrusions) {
        for (const IslandExtrusions &island_extrusions : slice_extrusions.common_extrusions) {
            if (!island_extrusions.perimeters.empty() || !island_extrusions.infill_ranges.empty()) {
                return false;
            }
        }
        if (!slice_extrusions.ironing_extrusions.empty()) {
            return false;
        }
    }
    return true;
}

bool is_empty(const ExtruderExtrusions &extruder_extrusions) {
    for (const OverridenExtrusions &overriden_extrusions : extruder_extrusions.overriden_extrusions) {
        if (!is_empty(overriden_extrusions.slices_extrusions)) {
            return false;
        }
    }
    for (const NormalExtrusions &normal_extrusions : extruder_extrusions.normal_extrusions) {
        if (!normal_extrusions.support_extrusions.empty()) {
            return false;
        }
        if (!is_empty(normal_extrusions.slices_extrusions)) {
            return false;
        }
    }
    return true;
}


std::vector<ExtruderExtrusions> get_extrusions(
    const Print &print,
    const GCode::WipeTowerIntegration *wipe_tower,
    const GCode::ObjectsLayerToPrint &layers,
    const bool is_first_layer,
    const LayerTools &layer_tools,
    const std::vector<InstanceToPrint> &instances_to_print,
    const std::map<unsigned int, std::pair<size_t, size_t>> &skirt_loops_per_extruder,
    unsigned current_extruder_id,
    const PathSmoothingFunction &smooth_path,
    bool get_brim,
    std::optional<Point> previous_position
) {
    unsigned toolchange_number{0};

    std::vector<ExtruderExtrusions> extrusions;
    for (const unsigned int extruder_id : layer_tools.extruders)
    {
        ExtruderExtrusions extruder_extrusions{extruder_id};

        if (layer_tools.has_wipe_tower && wipe_tower != nullptr) {
            const bool finish_wipe_tower{extruder_id == layer_tools.extruders.back()};
            if (finish_wipe_tower || is_toolchange_required(is_first_layer, layer_tools.extruders.back(), extruder_id, current_extruder_id)) {
                const bool ignore_sparse{print.config().get<bool>("wipe_tower_no_sparse_layers")};
                if (const auto tool_change{wipe_tower->get_toolchange(toolchange_number, ignore_sparse)}) {
                    toolchange_number++;
                    previous_position = scaled(wipe_tower->transform_wt_pt(tool_change->end_pos));
                    current_extruder_id = tool_change->new_tool;
                    extruder_extrusions.wipe_tower_start = scaled(wipe_tower->transform_wt_pt(tool_change->start_pos));
                }
            }
        }


        if (auto loops_it = skirt_loops_per_extruder.find(extruder_id); loops_it != skirt_loops_per_extruder.end()) {
            const std::pair<size_t, size_t> loops = loops_it->second;
            for (std::size_t i = loops.first; i < loops.second; ++i) {
                bool reverse{false};
                if (auto loop = dynamic_cast<const ExtrusionLoop *>(print.skirt().entities[i])) {
                    const bool is_hole = loop->is_clockwise();
                    reverse = print.config().get<bool>("prefer_clockwise_movements") ? !is_hole : is_hole;
                }
                const ExtrusionEntityReference entity{*print.skirt().entities[i], reverse};
                std::optional<InstancePoint> last_position{get_instance_point(previous_position, {0, 0})};
                auto [path, _]{smooth_path(nullptr, nullptr, entity, extruder_id, last_position)};
                previous_position = get_gcode_point(last_position, {0, 0});
                extruder_extrusions.skirt.emplace_back(i, std::move(path));
            }
        }

        // Extrude brim with the extruder of the 1st region.
        if (get_brim) {
            for (const ExtrusionEntity *entity : print.brim().entities) {
                bool reverse{false};
                bool is_loop{false};
                if (auto loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
                    const bool is_hole = loop->is_clockwise();
                    is_loop = true;
                    reverse = print.config().get<bool>("prefer_clockwise_movements") ? !is_hole : is_hole;
                }
                const ExtrusionEntityReference entity_reference{*entity, reverse};

                std::optional<InstancePoint> last_position{get_instance_point(previous_position, {0, 0})};
                auto [path, _]{smooth_path(nullptr, nullptr, entity_reference, extruder_id, last_position)};
                previous_position = get_gcode_point(last_position, {0, 0});
                extruder_extrusions.brim.push_back({std::move(path), is_loop});
            }
            get_brim = false;
        }

        using GCode::ExtrusionOrder::get_overriden_extrusions;
        bool is_anything_overridden = layer_tools.wiping_extrusions().is_anything_overridden();
        if (is_anything_overridden) {
            extruder_extrusions.overriden_extrusions = get_overriden_extrusions(
                print, layers, layer_tools, instances_to_print, extruder_id, smooth_path,
                previous_position
            );
        }

        using GCode::ExtrusionOrder::get_normal_extrusions;
        extruder_extrusions.normal_extrusions = get_normal_extrusions(
            print, layers, layer_tools, instances_to_print, extruder_id, smooth_path,
            previous_position
        );

        extrusions.push_back(std::move(extruder_extrusions));
    }
    return extrusions;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const SmoothPath &path) {
    for (const SmoothPathElement & element : path) {
        if (!element.path.empty()) {
            return element.path.front();
        }
    }
    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const std::vector<SmoothPath> &smooth_paths) {
    for (const SmoothPath &path : smooth_paths) {
        if (auto result = get_first_point(path)) {
            return result;
        }
    }
    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const std::vector<InfillRange> &infill_ranges) {
    for (const InfillRange &infill_range : infill_ranges) {
        if (auto result = get_first_point(infill_range.items)) {
            return result;
        }
    }
    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const std::vector<Perimeter> &perimeters) {
    for (const Perimeter &perimeter : perimeters) {
        if (auto result = get_first_point(perimeter.smooth_path)) {
            return result;
        }
    }
    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const std::vector<IslandExtrusions> &extrusions) {
    for (const IslandExtrusions &island : extrusions) {
        if (island.infill_first) {
            if (auto result = get_first_point(island.infill_ranges)) {
                return result;
            }
            if (auto result = get_first_point(island.perimeters)) {
                return result;
            }
        } else {
            if (auto result = get_first_point(island.perimeters)) {
                return result;
            }
            if (auto result = get_first_point(island.infill_ranges)) {
                return result;
            }
        }
    }
    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const std::vector<SliceExtrusions> &extrusions) {
    for (const SliceExtrusions &slice : extrusions) {
        if (auto result = get_first_point(slice.common_extrusions)) {
            return result;
        }
    }
    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const ExtruderExtrusions &extrusions) {
    for (const auto&[_, path] : extrusions.skirt) {
        if (auto result = get_first_point(path)) {
            return result;
        };
    }
    for (const BrimPath &brim_path : extrusions.brim) {
        if (auto result = get_first_point(brim_path.path)) {
            return result;
        };
    }
    for (const OverridenExtrusions &overriden_extrusions : extrusions.overriden_extrusions) {
        if (auto result = get_first_point(overriden_extrusions.slices_extrusions)) {
            result->point += overriden_extrusions.instance_offset;
            return result;
        }
    }

    for (const NormalExtrusions &normal_extrusions : extrusions.normal_extrusions) {
        for (const SupportPath &support_path : normal_extrusions.support_extrusions) {
            if (auto result = get_first_point(support_path.path)) {
                result->point += normal_extrusions.instance_offset;
                return result;
            }
        }
        if (auto result = get_first_point(normal_extrusions.slices_extrusions)) {
            result->point += normal_extrusions.instance_offset;
            return result;
        }
    }

    return std::nullopt;
}

std::optional<Geometry::ArcWelder::Segment> get_first_point(const std::vector<ExtruderExtrusions> &extrusions) {
    if (extrusions.empty()) {
        return std::nullopt;
    }

    if (extrusions.front().wipe_tower_start) {
        return {{*(extrusions.front().wipe_tower_start)}};
    }

    for (const ExtruderExtrusions &extruder_extrusions : extrusions) {
        if (auto result = get_first_point(extruder_extrusions)) {
            return result;
        }
    }

    return std::nullopt;
}

const PrintInstance * get_first_instance(
    const std::vector<ExtruderExtrusions> &extrusions,
    const std::vector<InstanceToPrint> &instances_to_print
) {
    if (instances_to_print.empty()) {
        return nullptr;
    }

    for (const ExtruderExtrusions &extruder_extrusions : extrusions) {
        if (!extruder_extrusions.overriden_extrusions.empty()) {
            for (std::size_t i{0}; i < instances_to_print.size(); ++i) {
                const OverridenExtrusions &overriden_extrusions{extruder_extrusions.overriden_extrusions[i]};
                if (!is_empty(overriden_extrusions.slices_extrusions)) {
                    const InstanceToPrint &instance{instances_to_print[i]};
                    return &instance.print_object.instances()[instance.instance_id];
                }
            }
        }
        for (std::size_t i{0}; i < instances_to_print.size(); ++i) {
            const InstanceToPrint &instance{instances_to_print[i]};
            const std::vector<SupportPath> &support_extrusions{extruder_extrusions.normal_extrusions[i].support_extrusions};
            const std::vector<SliceExtrusions> &slices_extrusions{extruder_extrusions.normal_extrusions[i].slices_extrusions};

            if (!support_extrusions.empty() || !is_empty(slices_extrusions)) {
                return &instance.print_object.instances()[instance.instance_id];
            }
        }
    }
    const InstanceToPrint &instance{instances_to_print.front()};
    return &instance.print_object.instances()[instance.instance_id];
}

}
