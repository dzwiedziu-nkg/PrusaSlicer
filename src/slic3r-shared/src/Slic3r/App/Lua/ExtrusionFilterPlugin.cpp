#include "Slic3r/App/Lua/ExtrusionFilterPlugin.hpp"

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

using Domain::GCodeExtrusionRole;
using GCode::ExtrusionFilter::PathInfo;

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

std::optional<Plugin> find_plugin(const std::vector<std::string>& plugin_paths)
{
    PluginRegistry registry;
    for (const std::string& path : plugin_paths) {
        registry.scan(path);
    }

    std::optional<Plugin> found;
    for (const Plugin& plugin : registry.plugins() | std::views::values) {
        if (plugin.meta().type != PluginType::SlicingExtrusionFilter) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Extrusion filter plugin {} is ignored, {} is already in use",
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
 * @brief Runs one plugin's keep_extrusion() as a GCode::ExtrusionFilter::Predicate.
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
        m_keep_extrusion = m_lua.state()["keep_extrusion"];
        // Reused for every call: this runs once per extrusion path, and rebuilding the
        // table each time would dominate the cost of the plugin.
        m_argument = m_lua.state().create_table(0, 5);
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    ~Runner()
    {
        if (m_dropped > 0) {
            SPDLOG_INFO(
                "Extrusion filter plugin {} dropped {} of {} extrusions",
                m_plugin.meta().id,
                m_dropped,
                m_seen
            );
        }
    }

    bool operator()(const PathInfo& path)
    {
        if (m_disabled) {
            return true;
        }
        ++m_seen;

        m_argument["role"] = role_name(path.role);
        m_argument["length"] = path.length;
        m_argument["layer_id"] = path.layer_id;
        m_argument["print_z"] = path.print_z;
        m_argument["extruder_id"] = path.extruder_id;

        const sol::protected_function_result result{m_keep_extrusion(m_argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("keep_extrusion() failed: {}", error.what()));
            return true;
        }

        const sol::object returned = result;
        if (returned.get_type() != sol::type::boolean) {
            disable("keep_extrusion() did not return a boolean");
            return true;
        }

        const bool keep = returned.as<bool>();
        if (!keep) {
            ++m_dropped;
        }
        return keep;
    }

private:
    /** @brief Reports the first failure and keeps every extrusion from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Extrusion filter plugin {} disabled for this export: {}",
            m_plugin.meta().id,
            reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_keep_extrusion;
    sol::table m_argument;
    bool m_disabled{false};
    std::size_t m_seen{0};
    std::size_t m_dropped{0};
};

} // namespace

GCode::ExtrusionFilter::Predicate make_extrusion_filter(
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
            "Loading extrusion filter plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading extrusion filter plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Extrusion filter plugin in use: {}", runner->id());
    return [runner](const PathInfo& path) { return (*runner)(path); };
}

} // namespace Slic3r::App::Lua
