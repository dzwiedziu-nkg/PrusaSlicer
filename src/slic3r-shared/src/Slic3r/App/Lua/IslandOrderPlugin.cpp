#include "Slic3r/App/Lua/IslandOrderPlugin.hpp"

#include <memory>
#include <numeric>
#include <optional>
#include <ranges>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/PluginRegistry.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

namespace Slic3r::App::Lua {

namespace {

using GCode::IslandOrdering::IslandInfo;
using GCode::IslandOrdering::LayerContext;

double to_mm(const Domain::coord_t value)
{
    return static_cast<double>(value) * Domain::SCALING_FACTOR;
}

/** @brief Picks the island ordering plugin to run, if there is one. */
std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const auto& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const auto& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingIslandOrder) {
            continue;
        }
        if (found.has_value()) {
            // The plugins are held in a std::map, so the winner does not depend on the
            // order the directories happen to be read in.
            SPDLOG_WARN(
                "Island ordering plugin {} is ignored, {} is already in use",
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
 * @brief Runs one plugin's order_islands() as a GCode::IslandOrdering::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because
 * the sandboxed require() implementation keeps references to both.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_order_islands = m_lua.state()["order_islands"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    std::vector<std::size_t> operator()(
        const std::vector<IslandInfo>& islands, const LayerContext& context
    )
    {
        if (m_disabled) {
            return stock_order(islands.size());
        }

        const sol::protected_function_result result{
            m_order_islands(describe(islands), describe(context))};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("order_islands() failed: {}", error.what()));
            return stock_order(islands.size());
        }

        const sol::object returned = result;
        if (returned.get_type() != sol::type::table) {
            disable("order_islands() did not return a table");
            return stock_order(islands.size());
        }

        return to_positions(returned.as<sol::table>(), islands.size());
    }

private:
    sol::table describe(const std::vector<IslandInfo>& islands)
    {
        sol::table result = m_lua.state().create_table(static_cast<int>(islands.size()), 0);
        int position = 1;
        for (const IslandInfo& island : islands) {
            sol::table entry = m_lua.state().create_table(0, 2);
            entry["centroid"] = m_lua.state().create_table_with(
                "x", to_mm(island.centroid.x()),
                "y", to_mm(island.centroid.y())
            );
            entry["bbox"] = m_lua.state().create_table_with(
                "min_x", to_mm(island.bbox.min.x()),
                "min_y", to_mm(island.bbox.min.y()),
                "max_x", to_mm(island.bbox.max.x()),
                "max_y", to_mm(island.bbox.max.y())
            );
            result[position++] = entry;
        }
        return result;
    }

    sol::table describe(const LayerContext& context)
    {
        sol::table result = m_lua.state().create_table_with(
            "layer_id", context.layer_id,
            "print_z", context.print_z,
            "extruder_id", context.extruder_id
        );
        if (context.head_position.has_value()) {
            result["head"] = m_lua.state().create_table_with(
                "x", to_mm(context.head_position->x()),
                "y", to_mm(context.head_position->y())
            );
        }
        return result;
    }

    /**
     * @brief Converts the returned 1-based positions to the 0-based engine contract.
     *
     * Only shape is checked here; whether the result is a permutation is validated by
     * GCode::IslandOrdering::order_islands(), which owns that contract.
     */
    std::vector<std::size_t> to_positions(const sol::table& returned, const std::size_t count)
    {
        std::vector<std::size_t> positions;
        positions.reserve(count);

        if (returned.size() != count) {
            disable(fmt::format(
                "order_islands() returned {} positions for {} islands", returned.size(), count
            ));
            return stock_order(count);
        }

        for (std::size_t i = 1; i <= count; ++i) {
            const sol::optional<std::size_t> value = returned[i];
            if (!value.has_value() || *value < 1 || *value > count) {
                disable(fmt::format("order_islands() returned an out of range position at {}", i));
                return stock_order(count);
            }
            positions.push_back(*value - 1);
        }
        return positions;
    }

    static std::vector<std::size_t> stock_order(const std::size_t count)
    {
        std::vector<std::size_t> order(count);
        std::iota(order.begin(), order.end(), std::size_t{0});
        return order;
    }

    /** @brief Reports the first failure and falls back to the stock order for good. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Island ordering plugin {} disabled for this export: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the function handle and the sandbox must go before
    // the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_order_islands;
    bool m_disabled{false};
};

} // namespace

GCode::IslandOrdering::Strategy make_island_order_strategy(
    const std::vector<std::string>& plugin_paths
)
{
    std::optional<Plugin> plugin = find_plugin(plugin_paths);
    if (!plugin.has_value()) {
        return {};
    }

    std::shared_ptr<Runner> runner;
    try {
        runner = std::make_shared<Runner>(std::move(*plugin));
    } catch (const Biz::Lua::LuaException& e) {
        SPDLOG_ERROR("Loading island ordering plugin failed in script {}\n{}", e.script_path(), e.what());
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading island ordering plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Island ordering plugin in use: {}", runner->id());
    return [runner](const std::vector<IslandInfo>& islands, const LayerContext& context)
    { return (*runner)(islands, context); };
}

} // namespace Slic3r::App::Lua
