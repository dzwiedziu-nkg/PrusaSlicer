#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/SlicePlannerPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::App::Lua::make_slice_planner;
using Slic3r::SlicePlanner::LayerInfo;
using Slic3r::SlicePlanner::Plan;
using Slic3r::SlicePlanner::Strategy;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.slice-planner",
    "name": "Slice planner under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.slice_planner": "1.0.0"}
})";

LayerInfo layer(const std::size_t layer_id = 4, const double layer_height = 0.2)
{
    return LayerInfo{layer_id, layer_height * double(layer_id + 1), 0.1, layer_height,
                     20., 400., 1};
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy planner_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.slice-planner";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "planner.lua").string()}
            << "info = {id = \"planner\", type = \"slicing.slice_planner\"}\n"
            << body;

        return make_slice_planner({temp_dir.path().string()});
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[SlicePlannerPlugin] no plugin installed leaves no planner")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_slice_planner({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a bare number is the growth allowed")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return 0.3
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(layer());
    REQUIRE(planned.has_value());
    REQUIRE(planned->max_overhang == 0.3);
    // Nothing said about the removal width means no limit on it.
    REQUIRE(planned->max_overhang_width == 0.);
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a table may name both distances")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return {max_overhang = 0.25, max_overhang_width = 2.0}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(layer());
    REQUIRE(planned.has_value());
    REQUIRE(planned->max_overhang == 0.25);
    REQUIRE(planned->max_overhang_width == 2.0);
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] the layer reaches the plugin")
{
    // A chamfer is layer_height/tan(theta), so the answer is worked out of what the plugin was
    // handed and the arithmetic proves the fields arrived. 35 degrees at 0.2 mm is 0.2856 mm.
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            if layer.print_z < 1.0 then
                return nil
            end
            return layer.layer_height / math.tan(math.rad(35))
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE_FALSE(planner(layer(0)).has_value());
    const auto high = planner(layer(20));
    REQUIRE(high.has_value());
    REQUIRE(std::abs(high->max_overhang - 0.2856) < 1e-4);

    // And a thicker layer of the same object gets a proportionally wider allowance.
    const auto thick = planner(layer(20, 0.3));
    REQUIRE(thick.has_value());
    REQUIRE(std::abs(thick->max_overhang - 0.4284) < 1e-4);
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] declining prints the mesh's outline")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(layer()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a table without a growth asks for nothing")
{
    // Clipping deletes geometry, so it happens only when a plugin asks for it in so many
    // words. A table naming only the removal width is not asking for it.
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return {max_overhang_width = 2.0}
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(layer()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a remedy of fill asks for the other direction")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return {max_overhang = 0.2856, remedy = "fill"}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(layer());
    REQUIRE(planned.has_value());
    REQUIRE(planned->remedy == Slic3r::SlicePlanner::Remedy::Fill);
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] clip is what an answer without a remedy means")
{
    // Including the bare-number answer, which predates the remedy existing.
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            if layer.layer_id % 2 == 0 then
                return 0.3
            end
            return {max_overhang = 0.3, remedy = "clip"}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto bare = planner(layer(4));
    REQUIRE(bare.has_value());
    REQUIRE(bare->remedy == Slic3r::SlicePlanner::Remedy::Clip);
    const auto named = planner(layer(5));
    REQUIRE(named.has_value());
    REQUIRE(named->remedy == Slic3r::SlicePlanner::Remedy::Clip);
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a remedy that is neither is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return {max_overhang = 0.3, remedy = "shrug"}
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(layer()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a malformed answer is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            return {max_overhang = "gently"}
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(layer()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a failing plugin declines every layer")
{
    const Strategy planner = planner_for(R"(
        function plan_slice(layer)
            error("no")
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(layer()).has_value());
    REQUIRE_FALSE(planner(layer()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[SlicePlannerPlugin] a plugin without plan_slice is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(planner_for("function something_else() end")));
}

TEST_CASE("[SlicePlanner] no strategy prints the mesh's outline")
{
    REQUIRE_FALSE(Slic3r::SlicePlanner::plan_slice({}, layer()).has_value());
}

TEST_CASE("[SlicePlanner] an unbounded growth is a decline, zero is not")
{
    using Slic3r::SlicePlanner::UNBOUNDED;

    const Strategy unbounded = [](const LayerInfo&) { return Plan{}; };
    REQUIRE(Plan{}.max_overhang == UNBOUNDED);
    REQUIRE_FALSE(Slic3r::SlicePlanner::plan_slice(unbounded, layer()).has_value());

    // Zero is a real answer and a very different one: the outline may not widen at all.
    const Strategy vertical = [](const LayerInfo&) { return Plan{0., 2.}; };
    const auto planned = Slic3r::SlicePlanner::plan_slice(vertical, layer());
    REQUIRE(planned.has_value());
    REQUIRE(planned->max_overhang == 0.);
}

TEST_CASE("[SlicePlanner] a runaway distance is clamped and a nonsense one refused")
{
    using Slic3r::SlicePlanner::MAX_DISTANCE;

    const Strategy runaway = [](const LayerInfo&) { return Plan{1e9, 1e9}; };
    const auto clamped = Slic3r::SlicePlanner::plan_slice(runaway, layer());
    REQUIRE(clamped.has_value());
    REQUIRE(clamped->max_overhang == MAX_DISTANCE);
    REQUIRE(clamped->max_overhang_width == MAX_DISTANCE);

    // A negative removal width is meant as "no limit", which is what zero says.
    const Strategy backwards = [](const LayerInfo&) { return Plan{0.3, -1.}; };
    const auto floored = Slic3r::SlicePlanner::plan_slice(backwards, layer());
    REQUIRE(floored.has_value());
    REQUIRE(floored->max_overhang_width == 0.);

    // A NaN is not caught by the unbounded test, so it has to be caught here.
    const Strategy nonsense = [](const LayerInfo&) {
        return Plan{std::numeric_limits<double>::quiet_NaN(), 2.};
    };
    REQUIRE_FALSE(Slic3r::SlicePlanner::plan_slice(nonsense, layer()).has_value());
}
