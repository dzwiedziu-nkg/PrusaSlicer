#include <catch2/catch_test_macros.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/IslandSequencePlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::BoundingBox;
using Slic3r::Domain::Point;
using Slic3r::App::Lua::make_island_sequence_strategy;
using Slic3r::GCode::IslandSequencing::IslandInfo;
using Slic3r::GCode::IslandSequencing::LayerInfo;
using Slic3r::GCode::IslandSequencing::Plan;
using Slic3r::GCode::IslandSequencing::PrintContext;
using Slic3r::GCode::IslandSequencing::Strategy;
using Slic3r::GCode::IslandSequencing::is_collision_free;
using Slic3r::GCode::IslandSequencing::supports_collision_check;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.island-sequence",
    "name": "Island sequence under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.island_sequence": "1.0.0"}
})";

struct PluginFixture
{
    Strategy strategy_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.island-sequence";
        boost::filesystem::create_directories(bundle);
        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "sequence.lua").string()}
            << "info = {id = \"sequence\", type = \"slicing.island_sequence\"}\n"
            << body;
        return make_island_sequence_strategy({temp_dir.path().string()});
    }

    static std::vector<LayerInfo> layers()
    {
        const Point p0{1000000, 2000000};
        const Point p1{1000000, 3000000};
        return {
            LayerInfo{0, 0.2, 0.2, {
                IslandInfo{0, BoundingBox{p0, p0, true}, p0, {}, {0, 1}}
            }},
            LayerInfo{1, 0.4, 0.2, {
                IslandInfo{0, BoundingBox{p0, p0, true}, p0, {0}, {}},
                IslandInfo{1, BoundingBox{p1, p1, true}, p1, {0}, {}}
            }}
        };
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

namespace {

IslandInfo box_island(
    const std::size_t index,
    const double min_x,
    const double min_y,
    const double max_x,
    const double max_y
)
{
    const auto mm = [](const double value) {
        return static_cast<Slic3r::Domain::coord_t>(value * 1000000.);
    };
    const Point min{mm(min_x), mm(min_y)};
    const Point max{mm(max_x), mm(max_y)};
    return IslandInfo{index, BoundingBox{min, max, true}, (min + max) / 2, {}, {}};
}

Plan high_island_then_low_island()
{
    return Plan{{{1, {0}}, {0, {1}}}, 2., .6};
}

} // namespace

TEST_CASE_METHOD(PluginFixture, "[IslandSequencePlugin] plan and context are converted")
{
    const Strategy strategy = strategy_for(R"(
        function plan_islands(layers, ctx)
            if ctx.printer_model ~= "COREONE_INDX8T" then error("bad printer") end
            if ctx.extruder_clearance_radius ~= 75.0 then error("bad radius") end
            if ctx.extruder_clearance_height ~= 33.0 then error("bad height") end
            if ctx.collision_model ~= "unchecked" then error("bad collision model") end
            if layers[1].islands[1].centroid.x ~= 1.0 then error("bad x") end
            if layers[1].islands[1].overlaps_above[2] ~= 2 then error("bad link") end
            return {
                steps = {
                    {layer = 1, islands = {1}},
                    {layer = 2, islands = {2}},
                    {layer = 2, islands = {1}}
                },
                wipe_distance = 1.5,
                z_clearance = 0.7
            }
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    const std::optional<Plan> plan = strategy(
        layers(), PrintContext{"COREONE_INDX8T", 75., 33.}
    );
    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 3);
    REQUIRE(plan->steps[1].layer == 1);
    REQUIRE(plan->steps[1].islands == std::vector<std::size_t>{1});
    REQUIRE(plan->wipe_distance == 1.5);
    REQUIRE(plan->z_clearance == 0.7);
}

TEST_CASE_METHOD(PluginFixture, "[IslandSequencePlugin] nil keeps normal layer order")
{
    const Strategy strategy = strategy_for(
        "function plan_islands(layers, ctx) return nil end\n"
    );
    REQUIRE(static_cast<bool>(strategy));
    REQUIRE_FALSE(strategy(layers(), PrintContext{"XL"}).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[IslandSequencePlugin] malformed plan disables plugin")
{
    const Strategy strategy = strategy_for(R"(
        function plan_islands(layers, ctx)
            return {steps = {{layer = 1, islands = {0}}}}
        end
    )");
    REQUIRE(static_cast<bool>(strategy));
    REQUIRE_FALSE(strategy(layers(), PrintContext{"XL"}).has_value());
    REQUIRE_FALSE(strategy(layers(), PrintContext{"XL"}).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[IslandSequencePlugin] wrong entry point is rejected")
{
    REQUIRE_FALSE(static_cast<bool>(strategy_for("function execute(params) end\n")));
}

TEST_CASE("[IslandSequencePlugin] Core One head collision is detected")
{
    const PrintContext core_one{"COREONE", 75., 33.};
    REQUIRE(supports_collision_check(core_one));
    REQUIRE_FALSE(supports_collision_check(PrintContext{"COREONE_INDX8T", 75., 33.}));

    const std::vector<LayerInfo> nearby{
        LayerInfo{0, .4, .2, {
            box_island(0, 0., 0., 10., 10.),
            box_island(1, 30., 0., 40., 10.)
        }},
        LayerInfo{1, 2., .2, {box_island(0, 0., 0., 10., 10.)}}
    };
    REQUIRE_FALSE(is_collision_free(high_island_then_low_island(), nearby, core_one));

    auto far_apart = nearby;
    far_apart[0].islands[1] = box_island(1, 200., 0., 210., 10.);
    REQUIRE(is_collision_free(high_island_then_low_island(), far_apart, core_one));

    // At the configured carriage height the X gantry spans the bed, so separating
    // the islands only along X no longer makes the sequence safe.
    far_apart[1].print_z = 40.;
    REQUIRE_FALSE(is_collision_free(high_island_then_low_island(), far_apart, core_one));
}
