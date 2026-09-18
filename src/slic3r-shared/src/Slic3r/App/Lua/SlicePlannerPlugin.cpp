#include "Slic3r/App/Lua/SlicePlannerPlugin.hpp"

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

    /** @brief Which pass an answer is for. */
    enum class Remedy
    {
        Clip,
        Fill,
        Cap
    };

    static const char* name_of(const Remedy remedy)
    {
        switch (remedy) {
        case Remedy::Fill: return "fill";
        case Remedy::Cap: return "cap";
        default: return "clip";
        }
    }

    /** @brief The slot a remedy's plan belongs in. */
    static std::optional<SlicePlanner::Plan>& slot_of(SlicePlanner::Plans& plans, const Remedy remedy)
    {
        switch (remedy) {
        case Remedy::Fill: return plans.fill;
        case Remedy::Cap: return plans.cap;
        default: return plans.clip;
        }
    }

    /** @brief Reads one answer table into a plan, and says which pass it is for. */
    std::optional<SlicePlanner::Plan> to_plan(const sol::table& answer, Remedy& remedy_out)
    {
        remedy_out = Remedy::Clip;
        const sol::object growth = answer["max_overhang"];
        if (growth.get_type() == sol::type::none || growth.get_type() == sol::type::nil) {
            // A table that says nothing about the growth asks for no bound. Rewriting the
            // outline is destructive, so it happens only when a plugin asks in so many words.
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

        const sol::object remedy = answer["remedy"];
        if (remedy.get_type() != sol::type::none && remedy.get_type() != sol::type::nil) {
            if (remedy.get_type() != sol::type::string) {
                disable("plan_slice() answered with a remedy that is not a string");
                return std::nullopt;
            }
            const std::string name = remedy.as<std::string>();
            if (name == "fill") {
                remedy_out = Remedy::Fill;
            } else if (name == "cap") {
                remedy_out = Remedy::Cap;
            } else if (name != "clip") {
                disable(fmt::format(
                    "plan_slice() answered with a remedy of '{}', which is none of "
                    "'clip', 'fill' and 'cap'",
                    name
                ));
                return std::nullopt;
            }
        }
        return plan;
    }

    SlicePlanner::Plans operator()(const LayerInfo& layer)
    {
        // Layers of one object arrive in order and one at a time, but Print::process() runs
        // make_perimeters() - and therefore slice() - over the objects of a plate in parallel,
        // so two objects reach this at the same time. Every touch of the Lua state is under
        // m_mutex for that reason, and only for that reason.
        const std::lock_guard<std::mutex> guard{m_mutex};
        if (m_disabled) {
            return {};
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
            return {};
        }

        const sol::object returned = result;
        if (returned.get_type() == sol::type::nil) {
            // The plugin was happy with the outline the mesh gave.
            return {};
        }
        // A bare number is the common answer and means how far the outline may grow, clipped.
        if (returned.get_type() == sol::type::number) {
            ++m_bounded;
            return SlicePlanner::Plans{SlicePlanner::Plan{returned.as<double>()}, std::nullopt, std::nullopt};
        }
        if (returned.get_type() != sol::type::table) {
            disable("plan_slice() answered with neither a table, a number nor nil");
            return {};
        }

        const sol::table answer = returned.as<sol::table>();
        SlicePlanner::Plans plans;
        // A list of tables asks for one pass per entry, which is how a plugin says "cut the
        // small overhangs off and carry what is left". A single table is the one-pass case.
        if (answer[1].get_type() == sol::type::table) {
            for (std::size_t i = 1; i <= answer.size(); ++i) {
                const sol::object entry = answer[i];
                if (entry.get_type() != sol::type::table) {
                    disable("plan_slice() answered with a list holding something that is not a table");
                    return {};
                }
                Remedy remedy = Remedy::Clip;
                std::optional<SlicePlanner::Plan> plan =
                    to_plan(entry.as<sol::table>(), remedy);
                if (m_disabled) {
                    return {};
                }
                if (!plan.has_value()) {
                    continue;
                }
                std::optional<SlicePlanner::Plan>& slot = slot_of(plans, remedy);
                if (slot.has_value()) {
                    disable(fmt::format(
                        "plan_slice() answered with two '{}' plans for the same layer",
                        name_of(remedy)
                    ));
                    return {};
                }
                slot = plan;
            }
        } else {
            Remedy remedy = Remedy::Clip;
            std::optional<SlicePlanner::Plan> plan = to_plan(answer, remedy);
            if (m_disabled) {
                return {};
            }
            slot_of(plans, remedy) = plan;
        }

        if (!plans.empty()) {
            ++m_bounded;
        }
        return plans;
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
    std::mutex m_mutex;
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
