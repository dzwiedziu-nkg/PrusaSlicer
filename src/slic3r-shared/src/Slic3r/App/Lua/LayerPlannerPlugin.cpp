#include "Slic3r/App/Lua/LayerPlannerPlugin.hpp"

#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "Slic3r/Domain/Point.hpp"
#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/PluginRegistry.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

namespace Slic3r::App::Lua {

namespace {

using GCode::LayerPlanner::GroupInfo;
using GCode::LayerPlanner::GroupKind;
using GCode::LayerPlanner::LayerContext;

const std::unordered_map<Domain::GCodeExtrusionRole, std::string> ROLE_NAMES = {
    {Domain::GCodeExtrusionRole::None, "None"},
    {Domain::GCodeExtrusionRole::Perimeter, "Perimeter"},
    {Domain::GCodeExtrusionRole::ExternalPerimeter, "ExternalPerimeter"},
    {Domain::GCodeExtrusionRole::OverhangPerimeter, "OverhangPerimeter"},
    {Domain::GCodeExtrusionRole::InternalInfill, "InternalInfill"},
    {Domain::GCodeExtrusionRole::SolidInfill, "SolidInfill"},
    {Domain::GCodeExtrusionRole::TopSolidInfill, "TopSolidInfill"},
    {Domain::GCodeExtrusionRole::Ironing, "Ironing"},
    {Domain::GCodeExtrusionRole::BridgeInfill, "BridgeInfill"},
    {Domain::GCodeExtrusionRole::GapFill, "GapFill"},
    {Domain::GCodeExtrusionRole::Skirt, "Skirt"},
    {Domain::GCodeExtrusionRole::SupportMaterial, "SupportMaterial"},
    {Domain::GCodeExtrusionRole::SupportMaterialInterface, "SupportMaterialInterface"},
    {Domain::GCodeExtrusionRole::WipeTower, "WipeTower"},
    {Domain::GCodeExtrusionRole::Custom, "Custom"}
};

const std::string& role_name(const Domain::GCodeExtrusionRole role)
{
    static const std::string unknown{"Unknown"};
    const auto it = ROLE_NAMES.find(role);
    return it == ROLE_NAMES.end() ? unknown : it->second;
}

double to_mm(const Domain::coord_t value)
{
    return static_cast<double>(value) * Domain::SCALING_FACTOR;
}

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingLayerPlanner) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Layer planner plugin {} is ignored, {} is already in use",
                plugin.meta().id,
                found->meta().id
            );
            continue;
        }
        found = plugin;
    }
    return found;
}

std::vector<std::size_t> stock_order(const std::size_t count)
{
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    return order;
}

/**
 * @brief Runs one plugin's plan_layer() as a GCode::LayerPlanner::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because the
 * sandboxed require() implementation keeps references to both.
 *
 * Reached from the G-code generator's serial_in_order stage, so operator() is never entered
 * concurrently for one export and needs no lock - and a plugin may remember what it saw on the
 * layer below, which is the point of the hook.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_layer = m_lua.state()["plan_layer"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_reordered > 0) {
            SPDLOG_INFO(
                "Layer planner plugin {} reordered {} of {} layer slices",
                m_plugin.meta().id,
                m_reordered,
                m_seen
            );
        }
    }

    std::vector<std::size_t> operator()(
        const std::vector<GroupInfo>& groups, const LayerContext& context
    )
    {
        if (m_disabled) {
            return stock_order(groups.size());
        }
        ++m_seen;

        const sol::protected_function_result result{m_plan_layer(describe(groups, context))};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_layer() failed: {}", error.what()));
            return stock_order(groups.size());
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin was happy with the order the slicer chose.
            return stock_order(groups.size());
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_layer() answered with neither a table nor nil");
            return stock_order(groups.size());
        }

        const sol::table answer = returned.as<sol::table>();
        std::vector<std::size_t> order;
        order.reserve(answer.size());
        for (std::size_t i = 1; i <= answer.size(); ++i) {
            const sol::object position = answer[i];
            if (position.get_type() != sol::type::number) {
                disable("plan_layer() answered with a position that is not a number");
                return stock_order(groups.size());
            }
            const auto given = position.as<std::int64_t>();
            if (given < 1) {
                disable("plan_layer() answered with a position below one");
                return stock_order(groups.size());
            }
            order.push_back(static_cast<std::size_t>(given - 1));
        }
        ++m_reordered;
        return order;
    }

private:
    sol::table describe(const std::vector<GroupInfo>& groups, const LayerContext& context)
    {
        sol::state_view lua{m_lua.state()};
        sol::table layer = lua.create_table(0, 4);
        layer["layer_id"] = context.layer_id;
        layer["print_z"] = context.print_z;
        layer["extruder_id"] = context.extruder_id;

        sol::table described = lua.create_table(static_cast<int>(groups.size()), 0);
        int position = 1;
        for (const GroupInfo& group : groups) {
            sol::table entry = lua.create_table(0, 7);
            // One based, like the answer, so a plugin never has to convert between the two.
            entry["island"] = group.island + 1;
            entry["kind"] = group.kind == GroupKind::Perimeters ? "perimeters" : "fill";
            entry["role"] = role_name(group.role);
            entry["length"] = group.length;
            entry["volume"] = group.volume;
            entry["bbox"] = lua.create_table_with(
                "min_x", to_mm(group.bbox.min.x()),
                "min_y", to_mm(group.bbox.min.y()),
                "max_x", to_mm(group.bbox.max.x()),
                "max_y", to_mm(group.bbox.max.y())
            );
            // Keyed by role name rather than a list, because a plugin looking for one role
            // wants to ask for it rather than to search: group.roles.SolidInfill, or nil.
            sol::table shares = lua.create_table(0, static_cast<int>(group.roles.size()));
            for (const GCode::LayerPlanner::RoleShare &share : group.roles) {
                shares[role_name(share.role)] =
                    lua.create_table_with("length", share.length, "volume", share.volume);
            }
            entry["roles"] = shares;
            described[position++] = entry;
        }
        layer["groups"] = described;
        return layer;
    }

    /** @brief Reports the first failure and leaves every layer alone from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Layer planner plugin {} disabled for this export: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_layer;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_reordered{0};
};

} // namespace

GCode::LayerPlanner::Strategy make_layer_planner(const std::vector<std::string>& plugin_paths)
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
            "Loading layer planner plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading layer planner plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Layer planner plugin in use: {}", runner->id());
    return [runner](const std::vector<GroupInfo>& groups, const LayerContext& context) {
        return (*runner)(groups, context);
    };
}

} // namespace Slic3r::App::Lua
