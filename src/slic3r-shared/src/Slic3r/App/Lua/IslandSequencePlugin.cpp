#include "Slic3r/App/Lua/IslandSequencePlugin.hpp"

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

using GCode::IslandSequencing::IslandInfo;
using GCode::IslandSequencing::LayerInfo;
using GCode::IslandSequencing::Plan;
using GCode::IslandSequencing::PlanStep;
using GCode::IslandSequencing::PrintContext;

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
        if (plugin.meta().type != PluginType::SlicingIslandSequence) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Island sequencing plugin {} is ignored, {} is already in use",
                plugin.meta().id,
                found->meta().id
            );
            continue;
        }
        found = plugin;
    }
    return found;
}

class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_islands = m_lua.state()["plan_islands"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    std::optional<Plan> operator()(
        const std::vector<LayerInfo>& layers, const PrintContext& context
    )
    {
        if (m_disabled) {
            return std::nullopt;
        }

        const sol::protected_function_result result{
            m_plan_islands(describe(layers), describe(context))};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_islands() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::lua_nil) {
            return std::nullopt;
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_islands() returned neither a table nor nil");
            return std::nullopt;
        }
        return to_plan(returned.as<sol::table>());
    }

private:
    sol::table positions(const std::vector<std::size_t>& values)
    {
        sol::table result = m_lua.state().create_table(static_cast<int>(values.size()), 0);
        for (std::size_t i = 0; i < values.size(); ++i) {
            result[i + 1] = values[i] + 1;
        }
        return result;
    }

    sol::table describe(const IslandInfo& island)
    {
        return m_lua.state().create_table_with(
            "centroid", m_lua.state().create_table_with(
                "x", to_mm(island.centroid.x()), "y", to_mm(island.centroid.y())
            ),
            "bbox", m_lua.state().create_table_with(
                "min_x", to_mm(island.bbox.min.x()),
                "min_y", to_mm(island.bbox.min.y()),
                "max_x", to_mm(island.bbox.max.x()),
                "max_y", to_mm(island.bbox.max.y())
            ),
            "overlaps_below", positions(island.overlaps_below),
            "overlaps_above", positions(island.overlaps_above)
        );
    }

    sol::table describe(const std::vector<LayerInfo>& layers)
    {
        sol::table result = m_lua.state().create_table(static_cast<int>(layers.size()), 0);
        for (std::size_t i = 0; i < layers.size(); ++i) {
            sol::table islands = m_lua.state().create_table(
                static_cast<int>(layers[i].islands.size()), 0
            );
            for (std::size_t j = 0; j < layers[i].islands.size(); ++j) {
                islands[j + 1] = describe(layers[i].islands[j]);
            }
            result[i + 1] = m_lua.state().create_table_with(
                "layer_id", layers[i].layer_id,
                "print_z", layers[i].print_z,
                "height", layers[i].height,
                "islands", islands
            );
        }
        return result;
    }

    sol::table describe(const PrintContext& context)
    {
        return m_lua.state().create_table_with(
            "printer_model", context.printer_model,
            "extruder_clearance_radius", context.extruder_clearance_radius,
            "extruder_clearance_height", context.extruder_clearance_height,
            "collision_model", GCode::IslandSequencing::supports_collision_check(context)
                ? "coreone_fallback_v1"
                : "unchecked"
        );
    }

    std::optional<Plan> to_plan(const sol::table& returned)
    {
        const sol::optional<sol::table> steps = returned["steps"];
        if (!steps.has_value() || steps->size() == 0) {
            disable("plan_islands() returned no steps");
            return std::nullopt;
        }

        Plan plan;
        plan.wipe_distance = returned.get_or("wipe_distance", 2.0);
        plan.z_clearance = returned.get_or("z_clearance", 0.5);
        plan.steps.reserve(steps->size());

        for (std::size_t i = 1; i <= steps->size(); ++i) {
            const sol::optional<sol::table> step = (*steps)[i];
            if (!step.has_value()) {
                disable(fmt::format("plan step {} is not a table", i));
                return std::nullopt;
            }
            const sol::optional<std::size_t> layer = (*step)["layer"];
            const sol::optional<sol::table> islands = (*step)["islands"];
            if (!layer.has_value() || *layer == 0 || !islands.has_value()
                || islands->size() == 0) {
                disable(fmt::format("plan step {} is incomplete", i));
                return std::nullopt;
            }

            PlanStep parsed{*layer - 1, {}};
            parsed.islands.reserve(islands->size());
            for (std::size_t j = 1; j <= islands->size(); ++j) {
                const sol::optional<std::size_t> island = (*islands)[j];
                if (!island.has_value() || *island == 0) {
                    disable(fmt::format("plan step {} has an invalid island at {}", i, j));
                    return std::nullopt;
                }
                parsed.islands.push_back(*island - 1);
            }
            plan.steps.push_back(std::move(parsed));
        }
        return plan;
    }

    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Island sequencing plugin {} disabled for this export: {}",
            m_plugin.meta().id,
            reason
        );
    }

private:
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_islands;
    bool m_disabled{false};
};

} // namespace

GCode::IslandSequencing::Strategy make_island_sequence_strategy(
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
        SPDLOG_ERROR(
            "Loading island sequencing plugin failed in script {}\n{}",
            e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading island sequencing plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Island sequencing plugin in use: {}", runner->id());
    return [runner](const std::vector<LayerInfo>& layers, const PrintContext& context) {
        return (*runner)(layers, context);
    };
}

} // namespace Slic3r::App::Lua
