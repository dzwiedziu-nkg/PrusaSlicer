#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/LayerPlannerPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::App::Lua::make_layer_planner;
using Slic3r::GCode::LayerPlanner::GroupInfo;
using Slic3r::GCode::LayerPlanner::GroupKind;
using Slic3r::GCode::LayerPlanner::LayerContext;
using Slic3r::GCode::LayerPlanner::order_groups;
using Slic3r::GCode::LayerPlanner::Strategy;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.layer-planner",
    "name": "Layer planner under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.layer_planner": "1.0.0"}
})";

/**
 * @brief A layer of one island: its walls, its sparse fill and a patch of solid.
 *
 * The sparse run carries a little solid infill of its own, the way a real one does where a deck
 * starts inside a part - which is why a group says what every role in it contributes.
 */
std::vector<GroupInfo> groups()
{
    using Slic3r::Domain::GCodeExtrusionRole;
    return {
        GroupInfo{0, GroupKind::Perimeters, GCodeExtrusionRole::ExternalPerimeter, 106.7, 3.6, {},
                  {{GCodeExtrusionRole::ExternalPerimeter, 60.2, 2.0},
                   {GCodeExtrusionRole::Perimeter, 46.5, 1.6}}},
        GroupInfo{0, GroupKind::Fill, GCodeExtrusionRole::InternalInfill, 46.9, 1.1, {},
                  {{GCodeExtrusionRole::InternalInfill, 31.9, 0.4},
                   {GCodeExtrusionRole::SolidInfill, 15.0, 0.7}}},
        GroupInfo{0, GroupKind::Fill, GCodeExtrusionRole::SolidInfill, 74.5, 2.4, {},
                  {{GCodeExtrusionRole::SolidInfill, 74.5, 2.4}}}
    };
}

LayerContext context(const std::size_t layer_id = 35)
{
    return LayerContext{layer_id, 0.2 * double(layer_id + 1), 0};
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy planner_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.layer-planner";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "planner.lua").string()}
            << "info = {id = \"planner\", type = \"slicing.layer_planner\"}\n"
            << body;

        return make_layer_planner({temp_dir.path().string()});
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[LayerPlannerPlugin] no plugin installed leaves no planner")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_layer_planner({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] an order is a list of positions")
{
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            return {3, 2, 1}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const std::vector<std::size_t> order = order_groups(planner, groups(), context());
    REQUIRE(order == std::vector<std::size_t>{2, 1, 0});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] nil keeps the order the slicer chose")
{
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context()) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] the groups reach the plugin")
{
    // The solid fill first, then the rest in the order they came: what Prusa did by hand
    // against the hull line, written as a plugin would write it.
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            local solid, rest = {}, {}
            for i, group in ipairs(layer.groups) do
                if group.kind == "fill" and group.role == "SolidInfill" then
                    solid[#solid + 1] = i
                else
                    rest[#rest + 1] = i
                end
            end
            for _, i in ipairs(rest) do solid[#solid + 1] = i end
            return solid
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context()) == std::vector<std::size_t>{2, 0, 1});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] the layer reaches the plugin")
{
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            if layer.layer_id == 35 then
                return {2, 3, 1}
            end
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context(35)) == std::vector<std::size_t>{1, 2, 0});
    REQUIRE(order_groups(planner, groups(), context(36)) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] an answer that drops a group is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            return {1, 2}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context()) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] an answer that prints one twice is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            return {1, 1, 2}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context()) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] a plugin that raises is disabled")
{
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            error("no")
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context()) == std::vector<std::size_t>{0, 1, 2});
    REQUIRE(order_groups(planner, groups(), context(36)) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[LayerPlannerPlugin] a group says what every role in it is")
{
    // The whole point of the breakdown: the sparse run holds solid infill too, and a plugin
    // looking for the solid of a layer has to see both of them.
    const Strategy planner = planner_for(R"(
        function plan_layer(layer)
            local solid = 0.0
            for _, group in ipairs(layer.groups) do
                local share = group.roles.SolidInfill
                if share ~= nil then solid = solid + share.volume end
            end
            -- 0.7 from the sparse run and 2.4 from the solid one.
            if math.abs(solid - 3.1) > 1e-9 then return nil end
            if layer.groups[1].roles.Perimeter.length ~= 46.5 then return nil end
            if layer.groups[3].roles.InternalInfill ~= nil then return nil end
            return {3, 2, 1}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    REQUIRE(order_groups(planner, groups(), context()) == std::vector<std::size_t>{2, 1, 0});
}

TEST_CASE("[LayerPlannerPlugin] a layer with one group is never asked")
{
    const Strategy never = [](const std::vector<GroupInfo>&, const LayerContext&) {
        FAIL("the planner was asked about a layer with nothing to order");
        return std::vector<std::size_t>{};
    };

    REQUIRE(order_groups(never, {groups().front()}, context()).size() == 1);
}
