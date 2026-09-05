#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/ObjectLabelsPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::App::Lua::make_object_labels;
using Slic3r::GCode::ObjectLabels::RegionInfo;
using Slic3r::GCode::ObjectLabels::RegionKind;
using Slic3r::GCode::ObjectLabels::Strategy;

namespace {

using Slic3r::Domain::Point;
using Slic3r::Domain::Polygon;

constexpr auto MANIFEST = R"({
    "id": "com.example.object-labels",
    "name": "Object labels under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.object_labels": "1.0.0"}
})";

/** @brief A 10 x 10 mm square footprint, in scaled coordinates. */
Polygon square()
{
    const auto mm = [](const double v) {
        return static_cast<Slic3r::Domain::coord_t>(v / Slic3r::Domain::SCALING_FACTOR);
    };
    return Polygon{{Point{mm(0), mm(0)}, Point{mm(10), mm(0)},
                    Point{mm(10), mm(10)}, Point{mm(0), mm(10)}}};
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy labels_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.object-labels";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "labels.lua").string()}
            << "info = {id = \"labels\", type = \"slicing.object_labels\"}\n"
            << body;

        return make_object_labels({temp_dir.path().string()});
    }

    static RegionInfo wipe_tower() { return RegionInfo{RegionKind::WipeTower, "Wipe tower", square()}; }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[ObjectLabelsPlugin] no plugin installed leaves no strategy")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_object_labels({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[ObjectLabelsPlugin] a returned name labels the region")
{
    const Strategy labels = labels_for(R"(
        function label_region(region)
            if region.kind ~= "wipe_tower" then return nil end
            return "Tower"
        end
    )");
    REQUIRE(static_cast<bool>(labels));

    const auto name = labels(wipe_tower());
    REQUIRE(name.has_value());
    REQUIRE(*name == "Tower");
}

TEST_CASE_METHOD(PluginFixture, "[ObjectLabelsPlugin] the region reaches the plugin")
{
    const Strategy labels = labels_for(R"(
        function label_region(region)
            if region.default_name ~= "Wipe tower" then return nil end
            if #region.outline ~= 4 then return nil end
            -- the square spans 0..10 mm in both axes
            if region.outline[3].x ~= 10.0 or region.outline[3].y ~= 10.0 then return nil end
            return region.default_name
        end
    )");
    REQUIRE(static_cast<bool>(labels));

    const auto name = labels(wipe_tower());
    REQUIRE(name.has_value());
    REQUIRE(*name == "Wipe tower");
}

TEST_CASE_METHOD(PluginFixture, "[ObjectLabelsPlugin] declining leaves the region unnamed")
{
    const Strategy labels = labels_for("function label_region(region) return nil end\n");
    REQUIRE(static_cast<bool>(labels));

    REQUIRE_FALSE(labels(wipe_tower()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[ObjectLabelsPlugin] a failing plugin declines every region")
{
    const Strategy labels = labels_for(R"(
        function label_region(region)
            error("this plugin is broken")
        end
    )");
    REQUIRE(static_cast<bool>(labels));

    REQUIRE_FALSE(labels(wipe_tower()).has_value());
    // Still usable, and still declining, once it has failed.
    REQUIRE_FALSE(labels(wipe_tower()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[ObjectLabelsPlugin] an answer that is not a name is refused")
{
    const Strategy labels = labels_for("function label_region(region) return 42 end\n");
    REQUIRE(static_cast<bool>(labels));

    REQUIRE_FALSE(labels(wipe_tower()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[ObjectLabelsPlugin] a plugin without label_region is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(labels_for("function execute(params) end\n")));
}
