#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/IslandOrderPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::BoundingBox;
using Slic3r::Domain::Point;
using Slic3r::App::Lua::make_island_order_strategy;
using Slic3r::GCode::IslandOrdering::IslandInfo;
using Slic3r::GCode::IslandOrdering::LayerContext;
using Slic3r::GCode::IslandOrdering::Strategy;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.island-order",
    "name": "Island order under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.island_order": "1.0.0"}
})";

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy strategy_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.island-order";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "island_order.lua").string()}
            << "info = {id = \"island_order\", type = \"slicing.island_order\"}\n"
            << body;

        return make_island_order_strategy({temp_dir.path().string()});
    }

    /** @brief Islands one millimetre apart along X, in stock order. */
    static std::vector<IslandInfo> islands(const std::size_t count)
    {
        std::vector<IslandInfo> result;
        for (std::size_t i = 0; i < count; ++i) {
            const Point centroid{static_cast<Slic3r::Domain::coord_t>(i) * 1000000, 0};
            result.push_back(IslandInfo{i, BoundingBox{centroid, centroid, true}, centroid});
        }
        return result;
    }

    static LayerContext context() { return LayerContext{7, 1.4, 0, Point{5000000, 0}}; }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[IslandOrderPlugin] no plugin installed leaves no strategy")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_island_order_strategy({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[IslandOrderPlugin] the returned order is honoured")
{
    const Strategy strategy = strategy_for(R"(
        function order_islands(islands, ctx)
            local order = {}
            for i = 1, #islands do order[i] = #islands - i + 1 end
            return order
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE(strategy(islands(4), context()) == std::vector<std::size_t>{3, 2, 1, 0});
}

TEST_CASE_METHOD(PluginFixture, "[IslandOrderPlugin] coordinates arrive in millimetres")
{
    const Strategy strategy = strategy_for(R"(
        -- Sorts by X, but only for islands whose coordinates are the expected ones.
        function order_islands(islands, ctx)
            if ctx.head.x ~= 5.0 or ctx.print_z ~= 1.4 or ctx.layer_id ~= 7 then
                error("unexpected context")
            end
            local order = {}
            for i = 1, #islands do
                if islands[i].centroid.x ~= i - 1 then error("unexpected centroid") end
                if islands[i].bbox.max_x ~= i - 1 then error("unexpected bbox") end
                order[i] = i
            end
            return order
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE(strategy(islands(3), context()) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[IslandOrderPlugin] a failing plugin falls back to the stock order")
{
    const Strategy strategy = strategy_for(R"(
        function order_islands(islands, ctx)
            error("this plugin is broken")
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE(strategy(islands(3), context()) == std::vector<std::size_t>{0, 1, 2});
    // Still usable, and still stock, once it has failed.
    REQUIRE(strategy(islands(2), context()) == std::vector<std::size_t>{0, 1});
}

TEST_CASE_METHOD(PluginFixture, "[IslandOrderPlugin] an out of range position falls back to the stock order")
{
    const Strategy strategy = strategy_for(R"(
        function order_islands(islands, ctx)
            return {1, 2, 99}
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE(strategy(islands(3), context()) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[IslandOrderPlugin] a short answer falls back to the stock order")
{
    const Strategy strategy = strategy_for(R"(
        function order_islands(islands, ctx)
            return {1}
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE(strategy(islands(3), context()) == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE_METHOD(PluginFixture, "[IslandOrderPlugin] a plugin without order_islands is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(strategy_for("function execute(params) end\n")));
}
