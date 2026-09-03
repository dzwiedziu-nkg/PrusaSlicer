#include <catch2/catch_test_macros.hpp>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/ExtrusionFilterPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Slic3r::Domain::GCodeExtrusionRole;
using Slic3r::App::Lua::make_extrusion_filter;
using Slic3r::GCode::ExtrusionFilter::PathInfo;
using Slic3r::GCode::ExtrusionFilter::Predicate;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.extrusion-filter",
    "name": "Extrusion filter under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.extrusion_filter": "1.0.0"}
})";

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Predicate filter_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.extrusion-filter";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "filter.lua").string()}
            << "info = {id = \"filter\", type = \"slicing.extrusion_filter\"}\n"
            << body;

        return make_extrusion_filter({temp_dir.path().string()});
    }

    static PathInfo path(const GCodeExtrusionRole role, const double length)
    {
        return PathInfo{role, length, 7, 1.4, 0};
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[ExtrusionFilterPlugin] no plugin installed leaves no filter")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_extrusion_filter({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[ExtrusionFilterPlugin] short paths can be dropped by role")
{
    const Predicate filter = filter_for(R"(
        function keep_extrusion(path)
            if path.role == "ExternalPerimeter" or path.role == "TopSolidInfill" then
                return true
            end
            return path.length >= 2.0
        end
    )");
    REQUIRE(static_cast<bool>(filter));

    REQUIRE_FALSE(filter(path(GCodeExtrusionRole::InternalInfill, 1.1)));
    REQUIRE(filter(path(GCodeExtrusionRole::InternalInfill, 2.5)));
    REQUIRE(filter(path(GCodeExtrusionRole::ExternalPerimeter, 0.4)));
    REQUIRE(filter(path(GCodeExtrusionRole::TopSolidInfill, 0.4)));
    REQUIRE_FALSE(filter(path(GCodeExtrusionRole::Perimeter, 0.4)));
    REQUIRE_FALSE(filter(path(GCodeExtrusionRole::SolidInfill, 0.4)));
}

TEST_CASE_METHOD(PluginFixture, "[ExtrusionFilterPlugin] the layer context reaches the plugin")
{
    const Predicate filter = filter_for(R"(
        function keep_extrusion(path)
            return path.layer_id == 7 and path.print_z == 1.4 and path.extruder_id == 0
        end
    )");
    REQUIRE(static_cast<bool>(filter));

    REQUIRE(filter(path(GCodeExtrusionRole::InternalInfill, 1.0)));
}

TEST_CASE_METHOD(PluginFixture, "[ExtrusionFilterPlugin] a failing plugin keeps every extrusion")
{
    const Predicate filter = filter_for(R"(
        function keep_extrusion(path)
            error("this plugin is broken")
        end
    )");
    REQUIRE(static_cast<bool>(filter));

    REQUIRE(filter(path(GCodeExtrusionRole::InternalInfill, 0.1)));
    // Still usable, and still keeping, once it has failed.
    REQUIRE(filter(path(GCodeExtrusionRole::InternalInfill, 0.1)));
}

TEST_CASE_METHOD(PluginFixture, "[ExtrusionFilterPlugin] a non boolean answer keeps every extrusion")
{
    const Predicate filter = filter_for(R"(
        function keep_extrusion(path)
            return "yes"
        end
    )");
    REQUIRE(static_cast<bool>(filter));

    REQUIRE(filter(path(GCodeExtrusionRole::InternalInfill, 0.1)));
}

TEST_CASE_METHOD(PluginFixture, "[ExtrusionFilterPlugin] a plugin without keep_extrusion is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(filter_for("function execute(params) end\n")));
}
