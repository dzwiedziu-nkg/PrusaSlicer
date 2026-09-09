#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PassPlannerPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Catch::Approx;
using Slic3r::Domain::GCodeExtrusionRole;
using Slic3r::App::Lua::make_pass_planner;
using Slic3r::PassPlanner::Plan;
using Slic3r::PassPlanner::Strategy;
using Slic3r::PassPlanner::SurfaceInfo;

namespace {

using Slic3r::Domain::ExPolygon;
using Slic3r::Domain::Point;
using Slic3r::Domain::Polygon;
using Slic3r::Domain::Polyline;

constexpr auto MANIFEST = R"({
    "id": "com.example.pass-planner",
    "name": "Pass planner under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.pass_planner": "1.0.0"}
})";

Slic3r::Domain::coord_t mm(const double v)
{
    return static_cast<Slic3r::Domain::coord_t>(v / Slic3r::Domain::SCALING_FACTOR);
}

/** @brief A 10 x 10 mm square, in scaled coordinates. */
ExPolygon square()
{
    ExPolygon region;
    region.contour = Polygon{{Point{mm(0), mm(0)}, Point{mm(10), mm(0)},
                              Point{mm(10), mm(10)}, Point{mm(0), mm(10)}}};
    return region;
}

SurfaceInfo surface(const ExPolygon& region, const GCodeExtrusionRole role, const bool object_above = true)
{
    return SurfaceInfo{role, region, 7, 1.4, 0, 0.45, 0.5, 1.5708, 0.2, 0.4, object_above};
}

/** @brief A single path from (x0, y0) to (x1, y1), in millimetres. */
Slic3r::Domain::Polylines path_mm(const double x0, const double y0, const double x1, const double y1)
{
    Polyline path;
    path.points = {Point{mm(x0), mm(y0)}, Point{mm(x1), mm(y1)}};
    return {path};
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy planner_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.pass-planner";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "planner.lua").string()}
            << "info = {id = \"planner\", type = \"slicing.pass_planner\"}\n"
            << body;

        return make_pass_planner({temp_dir.path().string()});
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[PassPlannerPlugin] no plugin installed leaves no planner")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_pass_planner({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] returned paths become an extra pass")
{
    const Strategy planner = planner_for(R"(
        function plan_pass(surface)
            if surface.role ~= "SupportMaterialInterface" then return nil end
            return {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    const auto planned = planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface));
    REQUIRE(planned.has_value());
    REQUIRE(planned->paths.size() == 1);
    REQUIRE(planned->paths.front().size() == 2);
    // A bare list of paths takes the spacing of what it goes over and the slicer's own
    // ironing flow.
    REQUIRE(planned->spacing == 0.);
    REQUIRE(planned->flow_ratio == Slic3r::PassPlanner::DEFAULT_FLOW_RATIO);

    // A role the plugin declines gets no extra pass.
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::SolidInfill)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] a plan may ask for its own spacing and flow")
{
    const Strategy planner = planner_for(R"(
        function plan_pass(surface)
            return {
                paths = {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}},
                spacing = 0.1,
                flow_ratio = 0.2
            }
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    const auto planned = planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface));
    REQUIRE(planned.has_value());
    REQUIRE(planned->spacing == Approx(0.1));
    REQUIRE(planned->flow_ratio == Approx(0.2));
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] a spacing or flow that is not a number is refused")
{
    const ExPolygon region = square();

    const Strategy bad_spacing = planner_for(R"(
        function plan_pass(surface)
            return {paths = {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}}, spacing = "close"}
        end
    )");
    REQUIRE(static_cast<bool>(bad_spacing));
    REQUIRE_FALSE(bad_spacing(surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());

    PluginFixture other;
    const Strategy bad_flow = other.planner_for(R"(
        function plan_pass(surface)
            return {paths = {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}}, flow_ratio = "thin"}
        end
    )");
    REQUIRE(static_cast<bool>(bad_flow));
    REQUIRE_FALSE(bad_flow(surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] the surface reaches the plugin")
{
    const Strategy planner = planner_for(R"(
        function plan_pass(surface)
            if surface.layer_id ~= 7 or surface.print_z ~= 1.4 then return nil end
            if surface.extruder_id ~= 0 or surface.spacing ~= 0.45 then return nil end
            if surface.extrusion_width ~= 0.5 or surface.layer_height ~= 0.2 then return nil end
            if surface.nozzle_diameter ~= 0.4 then return nil end
            if not surface.object_above then return nil end
            if #surface.contour ~= 4 or #surface.holes ~= 0 then return nil end
            -- the square spans 0..10 mm in both axes
            local c = surface.contour
            if c[3].x ~= 10.0 or c[3].y ~= 10.0 then return nil end
            return {{{x = 2.0, y = 2.0}, {x = 8.0, y = 2.0}}}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE(planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
    // The same surface with nothing printed on top of it is a different question, and the
    // plugin above answers it differently.
    REQUIRE_FALSE(
        planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface, false)).has_value()
    );
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] a failing plugin declines every surface")
{
    const Strategy planner = planner_for(R"(
        function plan_pass(surface)
            error("this plugin is broken")
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
    // Still usable, and still declining, once it has failed.
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] a malformed answer is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_pass(surface)
            return {{{x = 1.0}, {y = 2.0}}}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PassPlannerPlugin] a plugin without plan_pass is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(planner_for("function execute(params) end\n")));
}

TEST_CASE("[PassPlanner] no strategy runs no extra pass")
{
    const ExPolygon region = square();

    REQUIRE_FALSE(Slic3r::PassPlanner::plan_pass({}, surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
}

TEST_CASE("[PassPlanner] paths are clipped to the area that was covered")
{
    const ExPolygon region = square();
    // Runs from well outside the square to its middle.
    const Strategy overshooting = [](const SurfaceInfo&) {
        return Plan{path_mm(-5., 5., 5., 5.)};
    };

    const auto planned = Slic3r::PassPlanner::plan_pass(overshooting, surface(region, GCodeExtrusionRole::SupportMaterialInterface));
    REQUIRE(planned.has_value());
    REQUIRE(planned->paths.size() == 1);
    const auto& path = planned->paths.front();
    // Only the part inside the square survives, whichever end Clipper puts first.
    const auto x0 = std::min(path.points.front().x(), path.points.back().x());
    const auto x1 = std::max(path.points.front().x(), path.points.back().x());
    REQUIRE(x0 == Approx(0.).margin(mm(0.01)));
    REQUIRE(x1 == Approx(double(mm(5.))).margin(mm(0.01)));
}

TEST_CASE("[PassPlanner] a pass that stays inside is left exactly as it was laid out")
{
    const ExPolygon region = square();
    // Two lines walked in opposite directions, which is how a pass avoids flying back
    // across the surface between them. Both are well inside the square.
    const Strategy zigzag = [](const SurfaceInfo&) {
        Plan plan{path_mm(1., 4., 9., 4.)};
        plan.paths.push_back(path_mm(9., 5., 1., 5.).front());
        return plan;
    };

    const auto planned = Slic3r::PassPlanner::plan_pass(zigzag, surface(region, GCodeExtrusionRole::SupportMaterialInterface));
    REQUIRE(planned.has_value());
    REQUIRE(planned->paths.size() == 2);
    // Order and direction both survive: clipping a path that needs no clipping would
    // otherwise be free to hand it back reversed.
    REQUIRE(planned->paths[0].points.front().x() == mm(1.));
    REQUIRE(planned->paths[0].points.back().x() == mm(9.));
    REQUIRE(planned->paths[1].points.front().x() == mm(9.));
    REQUIRE(planned->paths[1].points.back().x() == mm(1.));
}

TEST_CASE("[PassPlanner] a pass that misses the area entirely is dropped")
{
    const ExPolygon region = square();
    const Strategy elsewhere = [](const SurfaceInfo&) {
        return Plan{path_mm(-5., -5., -1., -1.)};
    };

    REQUIRE_FALSE(Slic3r::PassPlanner::plan_pass(elsewhere, surface(region, GCodeExtrusionRole::SupportMaterialInterface)).has_value());
}

TEST_CASE("[PassPlanner] an unusable spacing or flow ratio is corrected")
{
    const ExPolygon region = square();
    const SurfaceInfo info = surface(region, GCodeExtrusionRole::SupportMaterialInterface);

    // A spacing the strategy could not state is the spacing of what it goes over.
    const Strategy no_spacing = [](const SurfaceInfo&) {
        return Plan{path_mm(1., 5., 9., 5.), 0., 0.15};
    };
    const auto defaulted = Slic3r::PassPlanner::plan_pass(no_spacing, info);
    REQUIRE(defaulted.has_value());
    REQUIRE(defaulted->spacing == Approx(info.spacing));

    // More than a full layer over material that is already there is over-extrusion.
    const Strategy too_much = [](const SurfaceInfo&) {
        return Plan{path_mm(1., 5., 9., 5.), 0.1, 4.};
    };
    const auto clamped = Slic3r::PassPlanner::plan_pass(too_much, info);
    REQUIRE(clamped.has_value());
    REQUIRE(clamped->flow_ratio == Approx(Slic3r::PassPlanner::MAX_FLOW_RATIO));

    // A ratio that is not a number at all falls back rather than clamping to an edge.
    const Strategy not_a_number = [](const SurfaceInfo&) {
        return Plan{path_mm(1., 5., 9., 5.), 0.1, std::nan("")};
    };
    const auto fallen_back = Slic3r::PassPlanner::plan_pass(not_a_number, info);
    REQUIRE(fallen_back.has_value());
    REQUIRE(fallen_back->flow_ratio == Approx(Slic3r::PassPlanner::DEFAULT_FLOW_RATIO));
}

TEST_CASE("[PassPlanner] the flow of a pass is a fraction of a layer")
{
    const ExPolygon region = square();
    const SurfaceInfo info = surface(region, GCodeExtrusionRole::SupportMaterialInterface);
    const Plan plan{path_mm(1., 5., 9., 5.), 0.1, 0.15};

    const auto flow = Slic3r::PassPlanner::pass_flow(info, plan);
    // 15 % of a 0.2 mm layer, thinned again because the pass runs its lines 0.1 mm apart
    // rather than a nozzle width apart: the arithmetic of the slicer's own ironing.
    REQUIRE(flow.height == Approx(0.15 * 0.2 * 0.1 / 0.4));
    REQUIRE(flow.mm3_per_mm == Approx(0.4 * flow.height));
    REQUIRE(flow.width > 0.);

    // Nothing extruded at all is a pass that only reheats the surface.
    const Plan dry{path_mm(1., 5., 9., 5.), 0.1, 0.};
    REQUIRE(Slic3r::PassPlanner::pass_flow(info, dry).mm3_per_mm == Approx(0.));
}
