#include "Slic3r/App/Lua/LoopDirectionPlugin.hpp"

#include <memory>
#include <optional>
#include <ranges>
#include <string>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/PluginRegistry.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

namespace Slic3r::App::Lua {

namespace {

using GCode::LoopDirection::Direction;
using GCode::LoopDirection::LoopInfo;

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingLoopDirection) {
            continue;
        }
        if (found.has_value()) {
            // The plugins are held in a std::map, so the winner does not depend on the
            // order the directories happen to be read in.
            SPDLOG_WARN(
                "Loop direction plugin {} is ignored, {} is already in use",
                plugin.meta().id,
                found->meta().id
            );
            continue;
        }
        found = plugin;
    }
    return found;
}

const char* to_lua(const Direction direction)
{
    return direction == Direction::Cw ? "cw" : "ccw";
}

/**
 * @brief Runs one plugin's plan_direction() as a GCode::LoopDirection::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because the
 * sandboxed require() implementation keeps references to both.
 *
 * Unlike the fill, pass and perimeter planners this one is entered from the serialized G-code
 * stage, one loop at a time and in layer order, so there is no mutex here and none is needed.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_direction = m_lua.state()["plan_direction"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_turned > 0) {
            SPDLOG_INFO(
                "Loop direction plugin {} turned {} of {} wall loops round",
                m_plugin.meta().id,
                m_turned,
                m_seen
            );
        }
    }

    std::optional<Direction> operator()(const LoopInfo& loop)
    {
        if (m_disabled) {
            return std::nullopt;
        }
        ++m_seen;

        sol::state_view lua{m_lua.state()};
        sol::table argument = lua.create_table(0, 10);
        argument["layer_id"] = loop.layer_id;
        argument["print_z"] = loop.print_z;
        argument["extruder_id"] = loop.extruder_id;
        argument["role"] = loop.external ? "ExternalPerimeter" : "Perimeter";
        argument["perimeter_index"] = loop.perimeter_index;
        argument["is_hole"] = loop.hole;
        argument["length"] = loop.length;
        argument["overhang_length"] = loop.overhang_length;
        argument["island_overhang"] = loop.island_overhang_length;
        argument["default_direction"] = to_lua(loop.stock);

        const sol::protected_function_result result{m_plan_direction(argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_direction() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin was happy with the direction the slicer picked.
            return std::nullopt;
        }
        // A bare string is the common answer. A table naming `direction` is accepted too, so
        // the answer has somewhere to grow without breaking the plugins written against this.
        sol::object named = returned;
        if (returned.get_type() == sol::type::table) {
            named = returned.as<sol::table>()["direction"];
            if (named.get_type() == sol::type::none || named.get_type() == sol::type::nil) {
                // A table that says nothing about the direction asks for no change.
                return std::nullopt;
            }
        }
        if (named.get_type() != sol::type::string) {
            disable("plan_direction() answered with neither a string, a table nor nil");
            return std::nullopt;
        }

        const std::string answer = named.as<std::string>();
        if (answer == "cw") {
            ++m_turned;
            return Direction::Cw;
        }
        if (answer == "ccw") {
            ++m_turned;
            return Direction::Ccw;
        }
        disable(fmt::format("plan_direction() answered \"{}\", which is neither cw nor ccw", answer));
        return std::nullopt;
    }

private:
    /** @brief Reports the first failure and declines every loop from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Loop direction plugin {} disabled for this slice: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_direction;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_turned{0};
};

} // namespace

GCode::LoopDirection::Strategy make_loop_direction(const std::vector<std::string>& plugin_paths)
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
            "Loading loop direction plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading loop direction plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Loop direction plugin in use: {}", runner->id());
    return [runner](const LoopInfo& loop) { return (*runner)(loop); };
}

} // namespace Slic3r::App::Lua
