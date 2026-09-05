#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/FillPlannerPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::Domain::GCodeExtrusionRole;
using Slic3r::App::Lua::make_fill_planner;
using Slic3r::FillPlanner::Strategy;
using Slic3r::FillPlanner::SurfaceInfo;

namespace {

using Slic3r::Domain::ExPolygon;
using Slic3r::Domain::Point;
using Slic3r::Domain::Polygon;

constexpr auto MANIFEST = R"({
    "id": "com.example.fill-planner",
    "name": "Fill planner under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.fill_planner": "1.0.0"}
})";

/** @brief A 10 x 10 mm square, in scaled coordinates. */
ExPolygon square()
{
    const auto mm = [](const double v) {
        return static_cast<Slic3r::Domain::coord_t>(v / Slic3r::Domain::SCALING_FACTOR);
    };
    ExPolygon region;
    region.contour = Polygon{{Point{mm(0), mm(0)}, Point{mm(10), mm(0)},
                              Point{mm(10), mm(10)}, Point{mm(0), mm(10)}}};
    return region;
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy planner_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.fill-planner";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "planner.lua").string()}
            << "info = {id = \"planner\", type = \"slicing.fill_planner\"}\n"
            << body;

        return make_fill_planner({temp_dir.path().string()});
    }

    static SurfaceInfo surface(const ExPolygon& region, const GCodeExtrusionRole role)
    {
        return SurfaceInfo{role, region, 7, 1.4, 0, 0.45, 1.5708};
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[FillPlannerPlugin] no plugin installed leaves no planner")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_fill_planner({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] returned paths replace the stock ones")
{
    const Strategy planner = planner_for(R"(
        function plan_fill(surface)
            if surface.role ~= "BridgeInfill" then return nil end
            return {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    const auto planned = planner(surface(region, GCodeExtrusionRole::BridgeInfill));
    REQUIRE(planned.has_value());
    REQUIRE(planned->paths.size() == 1);
    REQUIRE(planned->paths.front().size() == 2);
    // A bare list of paths asks for no change to the flow.
    REQUIRE(planned->flow_ratio == 1.);

    // A role the plugin declines keeps the slicer's own paths.
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::SolidInfill)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] a plan may ask for a different flow")
{
    const Strategy planner = planner_for(R"(
        function plan_fill(surface)
            return {
                paths = {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}},
                flow_ratio = 0.8
            }
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    const auto planned = planner(surface(region, GCodeExtrusionRole::BridgeInfill));
    REQUIRE(planned.has_value());
    REQUIRE(planned->paths.size() == 1);
    REQUIRE(planned->flow_ratio == 0.8);
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] a flow ratio that is not a number is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_fill(surface)
            return {
                paths = {{{x = 1.0, y = 1.0}, {x = 9.0, y = 9.0}}},
                flow_ratio = "thick"
            }
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::BridgeInfill)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] the surface reaches the plugin")
{
    const Strategy planner = planner_for(R"(
        function plan_fill(surface)
            if surface.layer_id ~= 7 or surface.print_z ~= 1.4 then return nil end
            if surface.extruder_id ~= 0 or surface.spacing ~= 0.45 then return nil end
            if #surface.contour ~= 4 or #surface.holes ~= 0 then return nil end
            -- the square spans 0..10 mm in both axes
            local c = surface.contour
            if c[3].x ~= 10.0 or c[3].y ~= 10.0 then return nil end
            return {{{x = 2.0, y = 2.0}, {x = 8.0, y = 2.0}}}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE(planner(surface(region, GCodeExtrusionRole::BridgeInfill)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] a failing plugin declines every surface")
{
    const Strategy planner = planner_for(R"(
        function plan_fill(surface)
            error("this plugin is broken")
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::BridgeInfill)).has_value());
    // Still usable, and still declining, once it has failed.
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::BridgeInfill)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] a malformed answer is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_fill(surface)
            return {{{x = 1.0}, {y = 2.0}}}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const ExPolygon region = square();
    REQUIRE_FALSE(planner(surface(region, GCodeExtrusionRole::BridgeInfill)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[FillPlannerPlugin] a plugin without plan_fill is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(planner_for("function execute(params) end\n")));
}
