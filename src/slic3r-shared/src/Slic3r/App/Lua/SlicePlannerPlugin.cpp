#include "Slic3r/App/Lua/SlicePlannerPlugin.hpp"

#include <memory>
#include <optional>
#include <ranges>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/PluginRegistry.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

namespace Slic3r::App::Lua {

namespace {

using SlicePlanner::LayerInfo;

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingSlicePlanner) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Slice planner plugin {} is ignored, {} is already in use",
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
 * @brief Runs one plugin's plan_slice() as a SlicePlanner::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because the
 * sandboxed require() implementation keeps references to both. Layers are offered one at a
 * time from the bottom up, so unlike the perimeter planner there is no lock here.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_slice = m_lua.state()["plan_slice"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_bounded > 0) {
            SPDLOG_INFO(
                "Slice planner plugin {} bounded the outline of {} of {} layers",
                m_plugin.meta().id,
                m_bounded,
                m_seen
            );
        }
    }

    std::optional<SlicePlanner::Plan> operator()(const LayerInfo& layer)
    {
        if (m_disabled) {
            return std::nullopt;
        }
        ++m_seen;

        sol::state_view lua{m_lua.state()};
        sol::table argument = lua.create_table(0, 7);
        argument["layer_id"] = layer.layer_id;
        argument["print_z"] = layer.print_z;
        argument["slice_z"] = layer.slice_z;
        argument["layer_height"] = layer.layer_height;
        argument["object_height"] = layer.object_height;
        argument["area"] = layer.area;
        argument["islands"] = layer.islands;

        const sol::protected_function_result result{m_plan_slice(argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_slice() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin was happy with the outline the mesh gave.
            return std::nullopt;
        }
        // A bare number is the common answer and means how far the outline may grow.
        if (returned.get_type() == sol::type::number) {
            ++m_bounded;
            return SlicePlanner::Plan{returned.as<double>()};
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_slice() answered with neither a table, a number nor nil");
            return std::nullopt;
        }

        const sol::table answer = returned.as<sol::table>();
        const sol::object growth = answer["max_overhang"];
        if (growth.get_type() == sol::type::none || growth.get_type() == sol::type::nil) {
            // A table that says nothing about the growth asks for no bound. Clipping is
            // destructive, so it happens only when a plugin asks for it in so many words.
            return std::nullopt;
        }
        if (growth.get_type() != sol::type::number) {
            disable("plan_slice() answered with a max_overhang that is not a number");
            return std::nullopt;
        }

        SlicePlanner::Plan plan{growth.as<double>()};
        const sol::object width = answer["max_overhang_width"];
        if (width.get_type() != sol::type::none && width.get_type() != sol::type::nil) {
            if (width.get_type() != sol::type::number) {
                disable("plan_slice() answered with a max_overhang_width that is not a number");
                return std::nullopt;
            }
            plan.max_overhang_width = width.as<double>();
        }
        ++m_bounded;
        return plan;
    }

private:
    /** @brief Reports the first failure and declines every layer from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Slice planner plugin {} disabled for this slice: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_slice;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_bounded{0};
};

} // namespace

SlicePlanner::Strategy make_slice_planner(const std::vector<std::string>& plugin_paths)
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
            "Loading slice planner plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading slice planner plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Slice planner plugin in use: {}", runner->id());
    return [runner](const LayerInfo& layer) { return (*runner)(layer); };
}

} // namespace Slic3r::App::Lua
