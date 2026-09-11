#include "Slic3r/App/Lua/PerimeterPlannerPlugin.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <ranges>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/PluginRegistry.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

namespace Slic3r::App::Lua {

namespace {

using PerimeterPlanner::RegionInfo;

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingPerimeterPlanner) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Perimeter planner plugin {} is ignored, {} is already in use",
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
 * @brief Runs one plugin's plan_perimeters() as a PerimeterPlanner::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because the
 * sandboxed require() implementation keeps references to both.
 *
 * Perimeters are generated from a tbb::parallel_for over layers, so operator() is entered
 * concurrently and every touch of the Lua state is under m_mutex.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_perimeters = m_lua.state()["plan_perimeters"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_changed > 0) {
            SPDLOG_INFO(
                "Perimeter planner plugin {} changed the wall count on {} of {} layer regions",
                m_plugin.meta().id,
                m_changed,
                m_seen
            );
        }
    }

    std::optional<PerimeterPlanner::Plan> operator()(const RegionInfo& region)
    {
        const std::lock_guard<std::mutex> guard{m_mutex};
        if (m_disabled) {
            return std::nullopt;
        }
        ++m_seen;

        sol::state_view lua{m_lua.state()};
        sol::table argument = lua.create_table(0, 8);
        argument["layer_id"] = region.layer_id;
        argument["print_z"] = region.print_z;
        argument["layer_height"] = region.layer_height;
        argument["extruder_id"] = region.extruder_id;
        argument["perimeters"] = region.perimeters;
        argument["perimeter_width"] = region.perimeter_width;
        argument["perimeter_spacing"] = region.perimeter_spacing;
        argument["nozzle_diameter"] = region.nozzle_diameter;

        const sol::protected_function_result result{m_plan_perimeters(argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_perimeters() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin was happy with the count the settings ask for.
            return std::nullopt;
        }
        // A bare number is the common answer and means the wall count.
        if (returned.get_type() == sol::type::number) {
            ++m_changed;
            return PerimeterPlanner::Plan{returned.as<int>()};
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_perimeters() answered with neither a table, a number nor nil");
            return std::nullopt;
        }

        const sol::object count = returned.as<sol::table>()["perimeters"];
        if (count.get_type() == sol::type::none || count.get_type() == sol::type::nil) {
            // A table that says nothing about the count asks for no change.
            return std::nullopt;
        }
        if (count.get_type() != sol::type::number) {
            disable("plan_perimeters() answered with a perimeters that is not a number");
            return std::nullopt;
        }
        ++m_changed;
        return PerimeterPlanner::Plan{count.as<int>()};
    }

private:
    /** @brief Reports the first failure and declines every layer from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Perimeter planner plugin {} disabled for this slice: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_perimeters;
    std::mutex m_mutex;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_changed{0};
};

} // namespace

PerimeterPlanner::Strategy make_perimeter_planner(const std::vector<std::string>& plugin_paths)
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
            "Loading perimeter planner plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading perimeter planner plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Perimeter planner plugin in use: {}", runner->id());
    return [runner](const RegionInfo& region) { return (*runner)(region); };
}

} // namespace Slic3r::App::Lua
