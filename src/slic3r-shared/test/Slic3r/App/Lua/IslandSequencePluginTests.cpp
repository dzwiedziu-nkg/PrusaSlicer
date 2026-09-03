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

TEST_CASE_METHOD(PluginFixture, "[IslandSequencePlugin] plan and context are converted")
{
    const Strategy strategy = strategy_for(R"(
        function plan_islands(layers, ctx)
            if ctx.printer_model ~= "COREONE_INDX8T" then error("bad printer") end
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

    const std::optional<Plan> plan = strategy(layers(), PrintContext{"COREONE_INDX8T"});
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
