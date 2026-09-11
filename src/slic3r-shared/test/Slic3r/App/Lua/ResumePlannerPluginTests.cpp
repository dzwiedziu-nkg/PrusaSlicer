#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <boost/nowide/fstream.hpp>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/ResumePlannerPlugin.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

using Catch::Approx;
using Slic3r::App::Lua::make_resume_planner;
using Slic3r::ResumePlanner::Interruption;
using Slic3r::ResumePlanner::Plan;
using Slic3r::ResumePlanner::ResumeInfo;
using Slic3r::ResumePlanner::Strategy;

namespace {

constexpr auto MANIFEST = R"({
    "id": "com.example.resume-planner",
    "name": "Resume planner under test",
    "license": "AGPL-3.0-only",
    "min_slicer_version": "3.0.0",
    "version": "1.0.0",
    "author": "example",
    "required_apis": {"slicing.resume_planner": "1.0.0"}
})";

ResumeInfo resume(
    const Interruption kind = Interruption::Pause,
    const double spare_volume = 120.
)
{
    return ResumeInfo{kind, 51, 10.2, 0, 0.2, 0.4, 540., spare_volume};
}

struct PluginFixture
{
    /** @brief Writes a one plugin bundle into the temp dir and loads it. */
    Strategy planner_for(const std::string& body)
    {
        const auto bundle = temp_dir.path() / "com.example.resume-planner";
        boost::filesystem::create_directories(bundle);

        boost::nowide::ofstream{(bundle / "manifest.json").string()} << MANIFEST;
        boost::nowide::ofstream{(bundle / "planner.lua").string()}
            << "info = {id = \"planner\", type = \"slicing.resume_planner\"}\n"
            << body;

        return make_resume_planner({temp_dir.path().string()});
    }

    Tests::TestTempDir temp_dir;
};

} // namespace

TEST_CASE("[ResumePlannerPlugin] no plugin installed leaves no planner")
{
    const Tests::TestTempDir empty_dir;

    REQUIRE_FALSE(static_cast<bool>(make_resume_planner({empty_dir.path().string()})));
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] a bare number is a purge volume")
{
    const Strategy planner = planner_for(R"(
        function plan_resume(resume)
            return 30.0
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(resume());
    REQUIRE(planned.has_value());
    REQUIRE(planned->purge_volume == Approx(30.));
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] a table may name the purge volume")
{
    const Strategy planner = planner_for(R"(
        function plan_resume(resume)
            return {purge_volume = 25.0}
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(resume());
    REQUIRE(planned.has_value());
    REQUIRE(planned->purge_volume == Approx(25.));
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] the interruption reaches the plugin")
{
    const Strategy planner = planner_for(R"(
        seen = {}
        function plan_resume(resume)
            seen = resume
            return resume.spare_volume / 4.0
        end
    )");
    REQUIRE(static_cast<bool>(planner));

    const auto planned = planner(resume(Interruption::ColorChange, 120.));
    REQUIRE(planned.has_value());
    // The plugin worked the answer out of what it was handed, so the arithmetic is the proof
    // that the fields arrived.
    REQUIRE(planned->purge_volume == Approx(30.));
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] declining resumes as the slicer would")
{
    const Strategy planner = planner_for(R"(
        function plan_resume(resume)
            return nil
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(resume()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] a malformed answer is refused")
{
    const Strategy planner = planner_for(R"(
        function plan_resume(resume)
            return {purge_volume = "a squirt"}
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(resume()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] a failing plugin declines every interruption")
{
    const Strategy planner = planner_for(R"(
        function plan_resume(resume)
            error("no")
        end
    )");
    REQUIRE(static_cast<bool>(planner));
    REQUIRE_FALSE(planner(resume()).has_value());
    // And stays declined rather than failing again on the next layer.
    REQUIRE_FALSE(planner(resume()).has_value());
}

TEST_CASE_METHOD(PluginFixture, "[ResumePlannerPlugin] a plugin without plan_resume is not loaded")
{
    REQUIRE_FALSE(static_cast<bool>(planner_for("function something_else() end")));
}

TEST_CASE("[ResumePlanner] no strategy resumes as the slicer would")
{
    REQUIRE_FALSE(Slic3r::ResumePlanner::plan_resume({}, resume()).has_value());
}

TEST_CASE("[ResumePlanner] a volume that is not a quantity purges nothing")
{
    const Strategy asked = [](const ResumeInfo&) { return Plan{30.}; };
    const auto kept = Slic3r::ResumePlanner::plan_resume(asked, resume());
    REQUIRE(kept.has_value());
    REQUIRE(kept->purge_volume == Approx(30.));

    // Asking for nothing is the same as declining, so a caller has one thing to check.
    const Strategy nothing = [](const ResumeInfo&) { return Plan{0.}; };
    REQUIRE_FALSE(Slic3r::ResumePlanner::plan_resume(nothing, resume()).has_value());

    const Strategy backwards = [](const ResumeInfo&) { return Plan{-30.}; };
    REQUIRE_FALSE(Slic3r::ResumePlanner::plan_resume(backwards, resume()).has_value());

    const Strategy not_a_number = [](const ResumeInfo&) { return Plan{std::nan("")}; };
    REQUIRE_FALSE(Slic3r::ResumePlanner::plan_resume(not_a_number, resume()).has_value());
}
