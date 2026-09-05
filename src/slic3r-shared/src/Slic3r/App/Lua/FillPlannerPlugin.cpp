#include "Slic3r/App/Lua/FillPlannerPlugin.hpp"

#include <memory>
#include <mutex>
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

using Domain::GCodeExtrusionRole;
using FillPlanner::SurfaceInfo;

/**
 * @brief Stable identifiers for the extrusion roles, as seen by a plugin.
 *
 * Deliberately the enum names rather than gcode_extrusion_role_to_string(), whose
 * output is translated and meant for the UI.
 */
const std::unordered_map<GCodeExtrusionRole, std::string> ROLE_NAMES = {
    {GCodeExtrusionRole::None, "None"},
    {GCodeExtrusionRole::Perimeter, "Perimeter"},
    {GCodeExtrusionRole::ExternalPerimeter, "ExternalPerimeter"},
    {GCodeExtrusionRole::OverhangPerimeter, "OverhangPerimeter"},
    {GCodeExtrusionRole::InternalInfill, "InternalInfill"},
    {GCodeExtrusionRole::SolidInfill, "SolidInfill"},
    {GCodeExtrusionRole::TopSolidInfill, "TopSolidInfill"},
    {GCodeExtrusionRole::Ironing, "Ironing"},
    {GCodeExtrusionRole::BridgeInfill, "BridgeInfill"},
    {GCodeExtrusionRole::GapFill, "GapFill"},
    {GCodeExtrusionRole::Skirt, "Skirt"},
    {GCodeExtrusionRole::SupportMaterial, "SupportMaterial"},
    {GCodeExtrusionRole::SupportMaterialInterface, "SupportMaterialInterface"},
    {GCodeExtrusionRole::WipeTower, "WipeTower"},
    {GCodeExtrusionRole::Custom, "Custom"}
};

const std::string& role_name(const GCodeExtrusionRole role)
{
    static const std::string unknown{"Unknown"};
    const auto it = ROLE_NAMES.find(role);
    return it == ROLE_NAMES.end() ? unknown : it->second;
}

double to_mm(const Domain::coord_t value)
{
    return static_cast<double>(value) * Domain::SCALING_FACTOR;
}

Domain::coord_t to_scaled(const double mm)
{
    return static_cast<Domain::coord_t>(std::llround(mm / Domain::SCALING_FACTOR));
}

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingFillPlanner) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Fill planner plugin {} is ignored, {} is already in use",
                plugin.meta().id,
                found->meta().id
            );
            continue;
        }
        found = plugin;
    }
    return found;
}

/** @brief Copies one closed contour into a fresh Lua array of {x, y} in millimetres. */
sol::table contour_to_lua(sol::state_view lua, const Domain::Polygon& contour)
{
    sol::table points = lua.create_table(static_cast<int>(contour.size()), 0);
    int index = 1;
    for (const Domain::Point& point : contour.points) {
        points[index++] = lua.create_table_with("x", to_mm(point.x()), "y", to_mm(point.y()));
    }
    return points;
}

/**
 * @brief Runs one plugin's plan_fill() as a FillPlanner::Strategy.
 *
 * Owns the Lua state the plugin runs in. Held by shared_ptr and never moved, because
 * the sandboxed require() implementation keeps references to both.
 *
 * Layer::make_fills() runs from a tbb::parallel_for over layers, so operator() is
 * entered concurrently and every touch of the Lua state is under m_mutex.
 */
class Runner
{
public:
    explicit Runner(Plugin plugin) : m_plugin(std::move(plugin))
    {
        m_lua.open_registry([this](auto& lua) { m_packages.register_api(lua); });
        m_plugin.load(m_lua);
        m_plan_fill = m_lua.state()["plan_fill"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_planned > 0) {
            SPDLOG_INFO(
                "Fill planner plugin {} laid out {} of {} surfaces",
                m_plugin.meta().id,
                m_planned,
                m_seen
            );
        }
    }

    std::optional<FillPlanner::Plan> operator()(const SurfaceInfo& surface)
    {
        const std::lock_guard<std::mutex> guard{m_mutex};
        if (m_disabled) {
            return std::nullopt;
        }
        ++m_seen;

        sol::state_view lua{m_lua.state()};
        sol::table argument = lua.create_table(0, 8);
        argument["role"] = role_name(surface.role);
        argument["layer_id"] = surface.layer_id;
        argument["print_z"] = surface.print_z;
        argument["extruder_id"] = surface.extruder_id;
        argument["spacing"] = surface.spacing;
        argument["bridge_angle"] = surface.bridge_angle;
        argument["contour"] = contour_to_lua(lua, surface.region.contour);

        sol::table holes = lua.create_table(static_cast<int>(surface.region.holes.size()), 0);
        int hole_index = 1;
        for (const Domain::Polygon& hole : surface.region.holes) {
            holes[hole_index++] = contour_to_lua(lua, hole);
        }
        argument["holes"] = holes;

        const sol::protected_function_result result{m_plan_fill(argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("plan_fill() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin had no opinion about this surface.
            return std::nullopt;
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_fill() answered with neither a table nor nil");
            return std::nullopt;
        }

        std::optional<FillPlanner::Plan> plan = to_plan(returned.as<sol::table>());
        if (!plan.has_value()) {
            disable("plan_fill() answered with something that is not a list of paths");
            return std::nullopt;
        }
        ++m_planned;
        return plan;
    }

private:
    /**
     * @brief Converts the answer, or nothing when it is not shaped like a plan.
     *
     * Two shapes are accepted. A plain list of paths is filled at the flow the slicer
     * worked out, which is what a planner keeping the stock line spacing wants. A table
     * carrying the paths under `paths` may also name a `flow_ratio` for them.
     */
    static std::optional<FillPlanner::Plan> to_plan(const sol::table& answer)
    {
        const sol::object paths_field = answer["paths"];
        if (paths_field.get_type() == sol::type::none
            || paths_field.get_type() == sol::type::nil) {
            std::optional<Domain::Polylines> paths = to_polylines(answer);
            if (!paths.has_value()) {
                return std::nullopt;
            }
            return FillPlanner::Plan{std::move(*paths)};
        }
        if (paths_field.get_type() != sol::type::table) {
            return std::nullopt;
        }

        std::optional<Domain::Polylines> paths = to_polylines(paths_field.as<sol::table>());
        if (!paths.has_value()) {
            return std::nullopt;
        }

        const sol::object ratio = answer["flow_ratio"];
        if (ratio.get_type() == sol::type::none || ratio.get_type() == sol::type::nil) {
            return FillPlanner::Plan{std::move(*paths)};
        }
        if (ratio.get_type() != sol::type::number) {
            return std::nullopt;
        }
        return FillPlanner::Plan{std::move(*paths), ratio.as<double>()};
    }

    /** @brief Converts a list of paths, or nothing when it is not shaped like one. */
    static std::optional<Domain::Polylines> to_polylines(const sol::table& answer)
    {
        Domain::Polylines paths;
        paths.reserve(answer.size());
        for (std::size_t i = 1; i <= answer.size(); ++i) {
            const sol::optional<sol::table> path = answer[i];
            if (!path.has_value()) {
                return std::nullopt;
            }

            Domain::Polyline polyline;
            polyline.points.reserve(path->size());
            for (std::size_t j = 1; j <= path->size(); ++j) {
                const sol::optional<sol::table> point = (*path)[j];
                if (!point.has_value()) {
                    return std::nullopt;
                }
                const sol::optional<double> x = (*point)["x"];
                const sol::optional<double> y = (*point)["y"];
                if (!x.has_value() || !y.has_value()) {
                    return std::nullopt;
                }
                polyline.points.emplace_back(to_scaled(*x), to_scaled(*y));
            }
            if (polyline.size() >= 2) {
                paths.push_back(std::move(polyline));
            }
        }
        return paths.empty() ? std::nullopt : std::optional{std::move(paths)};
    }

    /** @brief Reports the first failure and declines every surface from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Fill planner plugin {} disabled for this slice: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_plan_fill;
    std::mutex m_mutex;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_planned{0};
};

} // namespace

FillPlanner::Strategy make_fill_planner(const std::vector<std::string>& plugin_paths)
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
            "Loading fill planner plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading fill planner plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Fill planner plugin in use: {}", runner->id());
    return [runner](const SurfaceInfo& surface) { return (*runner)(surface); };
}

} // namespace Slic3r::App::Lua
