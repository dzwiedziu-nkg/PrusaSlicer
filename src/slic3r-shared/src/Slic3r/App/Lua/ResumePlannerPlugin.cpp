#include "Slic3r/App/Lua/ResumePlannerPlugin.hpp"

#include <memory>
#include <optional>
#include <ranges>
#include <unordered_map>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/PluginRegistry.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

namespace Slic3r::App::Lua {

namespace {

using ResumePlanner::Interruption;
using ResumePlanner::ResumeInfo;

/** @brief Stable identifiers for the kinds of interruption, as seen by a plugin. */
const std::unordered_map<Interruption, std::string> INTERRUPTION_NAMES = {
    {Interruption::Pause, "Pause"},
    {Interruption::ColorChange, "ColorChange"},
    {Interruption::ToolChange, "ToolChange"},
    {Interruption::Template, "Template"},
    {Interruption::Custom, "Custom"}
};

const std::string& interruption_name(const Interruption kind)
{
    static const std::string unknown{"Unknown"};
    const auto it = INTERRUPTION_NAMES.find(kind);
    return it == INTERRUPTION_NAMES.end() ? unknown : it->second;
}

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingResumePlanner) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Resume planner plugin {} is ignored, {} is already in use",
                plugin.meta().id,
                found->meta().id
            );
            continue;
        }
        found = plugin;
    }
    return found;
}

/**
 * @brief Runs one plugin's plan_resume() as a ResumePlanner::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because the
 * sandboxed require() implementation keeps references to both. G-code generation is single
 * threaded, so unlike the pass planner there is no lock here.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_resume = m_lua.state()["plan_resume"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_seen > 0) {
            SPDLOG_INFO(
                "Resume planner plugin {} purged after {} of {} interruptions",
                m_plugin.meta().id,
                m_planned,
                m_seen
            );
        }
    }

    std::optional<ResumePlanner::Plan> operator()(const ResumeInfo& resume)
    {
        if (m_disabled) {
            return std::nullopt;
        }
        ++m_seen;

        sol::state_view lua{m_lua.state()};
        sol::table argument = lua.create_table(0, 8);
        argument["kind"] = interruption_name(resume.kind);
        argument["layer_id"] = resume.layer_id;
        argument["print_z"] = resume.print_z;
        argument["extruder_id"] = resume.extruder_id;
        argument["layer_height"] = resume.layer_height;
        argument["nozzle_diameter"] = resume.nozzle_diameter;
        argument["spare_area"] = resume.spare_area;
        argument["spare_volume"] = resume.spare_volume;

        const sol::protected_function_result result{m_plan_resume(argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_resume() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin did not want anything done about this interruption.
            return std::nullopt;
        }
        // A bare number is the common answer and means the purge volume.
        if (returned.get_type() == sol::type::number) {
            ++m_planned;
            return ResumePlanner::Plan{returned.as<double>()};
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_resume() answered with neither a table, a number nor nil");
            return std::nullopt;
        }

        const sol::table answer = returned.as<sol::table>();
        const sol::object purge = answer["purge_volume"];
        if (purge.get_type() == sol::type::none || purge.get_type() == sol::type::nil) {
            // A table that says nothing about purging asks for nothing.
            return std::nullopt;
        }
        if (purge.get_type() != sol::type::number) {
            disable("plan_resume() answered with a purge_volume that is not a number");
            return std::nullopt;
        }
        ++m_planned;
        return ResumePlanner::Plan{purge.as<double>()};
    }

private:
    /** @brief Reports the first failure and declines every interruption from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Resume planner plugin {} disabled for this slice: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_resume;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_planned{0};
};

} // namespace

ResumePlanner::Strategy make_resume_planner(const std::vector<std::string>& plugin_paths)
{
    std::optional<Plugin> plugin = find_plugin(plugin_paths);
    if (!plugin.has_value()) {
        return {};
    }

    std::shared_ptr<Runner> runner;
    try {
        runner = std::make_shared<Runner>(std::move(*plugin));
    } catch (const Biz::Lua::LuaException& e) {
        SPDLOG_ERROR(
            "Loading resume planner plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading resume planner plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Resume planner plugin in use: {}", runner->id());
    return [runner](const ResumeInfo& resume) { return (*runner)(resume); };
}

} // namespace Slic3r::App::Lua
