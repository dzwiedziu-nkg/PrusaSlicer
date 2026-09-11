#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PerimeterPlannerPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::App::Lua::make_perimeter_planner;
using Slic3r::PerimeterPlanner::Plan;
using Slic3r::PerimeterPlanner::RegionInfo;
using Slic3r::PerimeterPlanner::Strategy;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.perimeter-planner",
    "name": "Perimeter planner under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.perimeter_planner": "1.0.0"}
})";

RegionInfo region(const std::size_t layer_id = 4, const int perimeters = 2)
{
    return RegionInfo{layer_id, 0.2 * double(layer_id + 1), 0.2, 0, perimeters, 0.45, 0.45, 0.4};
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy planner_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.perimeter-planner";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "planner.lua").string()}
            << "info = {id = \"planner\", type = \"slicing.perimeter_planner\"}\n"
            << body;

        return make_perimeter_planner({temp_dir.path().string()});
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[PerimeterPlannerPlugin] no plugin installed leaves no planner")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_perimeter_planner({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] a bare number is a wall count")
{
    const Strategy planner = planner_for(R"(
        function plan_perimeters(region)
            return region.perimeters + 1
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(region(4, 2));
    REQUIRE(planned.has_value());
    REQUIRE(planned->perimeters == 3);
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] a table may name the count")
{
    const Strategy planner = planner_for(R"(
        function plan_perimeters(region)
            return {perimeters = 5}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(region());
    REQUIRE(planned.has_value());
    REQUIRE(planned->perimeters == 5);
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] the layer reaches the plugin")
{
    // Alternate layers, which is what the hook was built for: the answer is worked out of
    // what the plugin was handed, so the arithmetic proves the fields arrived.
    const Strategy planner = planner_for(R"(
        function plan_perimeters(region)
            if region.layer_id % 2 == 1 then
                return region.perimeters + 1
            end
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE_FALSE(planner(region(4, 2)).has_value());
    const auto odd = planner(region(5, 2));
    REQUIRE(odd.has_value());
    REQUIRE(odd->perimeters == 3);
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] declining uses the settings' count")
{
    const Strategy planner = planner_for(R"(
        function plan_perimeters(region)
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(region()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] a malformed answer is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_perimeters(region)
            return {perimeters = "lots"}
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(region()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] a failing plugin declines every layer")
{
    const Strategy planner = planner_for(R"(
        function plan_perimeters(region)
            error("no")
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(region()).has_value());
    REQUIRE_FALSE(planner(region()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[PerimeterPlannerPlugin] a plugin without plan_perimeters is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(planner_for("function something_else() end")));
}

TEST_CASE("[PerimeterPlanner] no strategy uses the settings' count")
{
    REQUIRE_FALSE(Slic3r::PerimeterPlanner::plan_perimeters({}, region()).has_value());
}

TEST_CASE("[PerimeterPlanner] a runaway count is clamped and a no-change is a decline")
{
    using Slic3r::PerimeterPlanner::MAX_PERIMETERS;
    using Slic3r::PerimeterPlanner::MIN_PERIMETERS;

    const Strategy runaway = [](const RegionInfo&) { return Plan{100000}; };
    const auto clamped = Slic3r::PerimeterPlanner::plan_perimeters(runaway, region());
    REQUIRE(clamped.has_value());
    REQUIRE(clamped->perimeters == MAX_PERIMETERS);

    const Strategy backwards = [](const RegionInfo&) { return Plan{-4}; };
    const auto floored = Slic3r::PerimeterPlanner::plan_perimeters(backwards, region());
    REQUIRE(floored.has_value());
    REQUIRE(floored->perimeters == MIN_PERIMETERS);

    // Answering with the count it was handed is the same as declining, so the slicer has one
    // thing to check rather than two.
    const Strategy unchanged = [](const RegionInfo& r) { return Plan{r.perimeters}; };
    REQUIRE_FALSE(Slic3r::PerimeterPlanner::plan_perimeters(unchanged, region(4, 2)).has_value());
}
