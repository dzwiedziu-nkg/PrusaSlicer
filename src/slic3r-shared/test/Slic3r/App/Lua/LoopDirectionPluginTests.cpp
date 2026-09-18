#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/LoopDirectionPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::App::Lua::make_loop_direction;
using Slic3r::GCode::LoopDirection::Direction;
using Slic3r::GCode::LoopDirection::LoopInfo;
using Slic3r::GCode::LoopDirection::plan_direction;
using Slic3r::GCode::LoopDirection::Strategy;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.loop-direction",
    "name": "Loop direction plugin under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.loop_direction": "1.0.0"}
})";

LoopInfo loop(
    const std::size_t layer_id = 4,
    const bool external = true,
    const double overhang = 0.,
    const double island_overhang = 0.
)
{
    return LoopInfo{
        layer_id,
        0.2 * double(layer_id + 1),
        0,
        external,
        false,
        external ? 0 : 1,
        42.,
        overhang,
        island_overhang,
        Direction::Ccw
    };
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy strategy_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.loop-direction";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "direction.lua").string()}
            << "info = {id = \"direction\", type = \"slicing.loop_direction\"}\n"
            << body;

        return make_loop_direction({temp_dir.path().string()});
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[LoopDirectionPlugin] no plugin installed leaves no strategy")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_loop_direction({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] a bare string names the direction")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return "cw"
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    const auto planned = strategy(loop());
    REQUIRE(planned.has_value());
    REQUIRE(*planned == Direction::Cw);
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] a table may name the direction")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return {direction = "cw"}
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    const auto planned = strategy(loop());
    REQUIRE(planned.has_value());
    REQUIRE(*planned == Direction::Cw);
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] nil leaves the engine's choice")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(strategy(loop()).has_value());
    REQUIRE_FALSE(plan_direction(strategy, loop()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] a table saying nothing asks for no change")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return {}
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(strategy(loop()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] answering with the stock direction declines")
{
    // The plugin says ccw and the engine was going to walk it ccw anyway. The bridge reports
    // the answer; the contract is what decides that it changes nothing, and that is what keeps
    // the slicer off an override path for a plugin that agrees with it.
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return loop.default_direction
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    const auto reported = strategy(loop());
    REQUIRE(reported.has_value());
    REQUIRE(*reported == Direction::Ccw);
    REQUIRE_FALSE(plan_direction(strategy, loop()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] the layer reaches the plugin")
{
    // Reverse on even, which is what the hook was built for: the answer is worked out of what
    // the plugin was handed, so the arithmetic proves the fields arrived.
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            if loop.layer_id % 2 == 1 then
                return loop.default_direction == "ccw" and "cw" or "ccw"
            end
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(plan_direction(strategy, loop(4)).has_value());
    const auto odd = plan_direction(strategy, loop(5));
    REQUIRE(odd.has_value());
    REQUIRE(*odd == Direction::Cw);
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] the overhang reaches the plugin")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            if loop.island_overhang > 0.0 and loop.overhang_length >= 0.0 then
                return "cw"
            end
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(plan_direction(strategy, loop(4, true, 0., 0.)).has_value());
    REQUIRE(plan_direction(strategy, loop(4, true, 0., 3.5)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] the wall's place in the stack reaches the plugin")
{
    // What OrcaSlicer's overhang_reverse_internal_only does: leave the wall that shows alone.
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            if loop.role == "ExternalPerimeter" or loop.perimeter_index == 0 then
                return nil
            end
            return "cw"
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(plan_direction(strategy, loop(4, true)).has_value());
    REQUIRE(plan_direction(strategy, loop(4, false)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] an answer that is not a direction disables the plugin")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return "sideways"
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(strategy(loop()).has_value());
    // And it stays disabled rather than being asked again for every loop of the print.
    REQUIRE_FALSE(strategy(loop(5)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] a plugin that raises is disabled")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            error("no")
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(strategy(loop()).has_value());
    REQUIRE_FALSE(strategy(loop(5)).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[LoopDirectionPlugin] a number is not an answer")
{
    const Strategy strategy = strategy_for(R"(
        function plan_direction(loop)
            return 1
        end
    )");
    REQUIRE(static_cast<bool>(strategy));

    REQUIRE_FALSE(strategy(loop()).has_value());
}
