#include "Slic3r/App/Lua/ObjectLabelsPlugin.hpp"

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

using GCode::ObjectLabels::RegionInfo;
using GCode::ObjectLabels::RegionKind;

/**
 * @brief Stable identifiers for the region kinds, as seen by a plugin.
 *
 * Deliberately not the label the slicer shows, which is translated and may be reworded.
 */
const std::unordered_map<RegionKind, std::string> KIND_NAMES = {
    {RegionKind::WipeTower, "wipe_tower"}
};

const std::string& kind_name(const RegionKind kind)
{
    static const std::string unknown{"unknown"};
    const auto it = KIND_NAMES.find(kind);
    return it == KIND_NAMES.end() ? unknown : it->second;
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
        if (plugin.meta().type != PluginType::SlicingObjectLabels) {
            continue;
        }
        if (found.has_value()) {
            SPDLOG_WARN(
                "Object labels plugin {} is ignored, {} is already in use",
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
 * @brief Runs one plugin's label_region() as a GCode::ObjectLabels::Strategy.
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
        m_label_region = m_lua.state()["label_region"];
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    const std::string& id() const { return m_plugin.meta().id; }

    std::optional<std::string> operator()(const RegionInfo& region)
    {
        if (m_disabled) {
            return std::nullopt;
        }

        sol::state_view lua{m_lua.state()};
        sol::table argument = lua.create_table(0, 3);
        argument["kind"] = kind_name(region.kind);
        argument["default_name"] = region.default_name;
        argument["outline"] = contour_to_lua(lua, region.outline);

        const sol::protected_function_result result{m_label_region(argument)};
        if (!result.valid()) {
            const sol::error error = result;
            disable(fmt::format("label_region() failed: {}", error.what()));
            return std::nullopt;
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin does not want this part of the print named.
            return std::nullopt;
        }
        if (returned.get_type() != sol::type::string) {
            disable("label_region() answered with neither a string nor nil");
            return std::nullopt;
        }

        std::string name = returned.as<std::string>();
        SPDLOG_INFO(
            "Object labels plugin {} named the {} \"{}\"",
            m_plugin.meta().id,
            kind_name(region.kind),
            name
        );
        return name;
    }

private:
    /** @brief Reports the first failure and declines every region from then on. */
    void disable(const std::string& reason)
    {
        m_disabled = true;
        SPDLOG_ERROR(
            "Object labels plugin {} disabled for this export: {}", m_plugin.meta().id, reason
        );
    }

private:
    // Destroyed in reverse order: the handles must go before the state they point into.
    Biz::Lua::LuaEngine m_lua;
    PackageRegistry m_packages;
    Plugin m_plugin;
    sol::protected_function m_label_region;
    bool m_disabled{false};
};

} // namespace

GCode::ObjectLabels::Strategy make_object_labels(const std::vector<std::string>& plugin_paths)
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
            "Loading object labels plugin failed in script {}\n{}", e.script_path(), e.what()
        );
        return {};
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Loading object labels plugin failed\n{}", e.what());
        return {};
    }

    SPDLOG_INFO("Object labels plugin in use: {}", runner->id());
    return [runner](const RegionInfo& region) { return (*runner)(region); };
}

} // namespace Slic3r::App::Lua
