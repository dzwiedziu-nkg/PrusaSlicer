#include "Slic3r/App/Lua/PluginSystem.hpp"
#include "Slic3r/App/Lua/PluginDialog.hpp"
#include "Slic3r/App/Lua/PackageRegistry.hpp"
#include "Slic3r/App/Lua/ProjectApi.hpp"
#include "Slic3r/Biz/Lua/LuaException.hpp"

#include <utility>
#include <imgui/imgui.h>

namespace Slic3r::App::Lua {

PluginSystem::PluginSystem(
    std::initializer_list<std::string> plugin_paths,
    Biz::ProjectInteractor& project_interactor,
    Biz::Emboss::IFontManager& font_manager
) :
    m_plugin_paths(plugin_paths),
    m_project_interactor(project_interactor),
    m_font_manager(font_manager)
{}

void PluginSystem::install(const std::string& zip_file_path)
{
    auto zip_source = Biz::Crypto::create_zip_source(zip_file_path);
    if (zip_source == nullptr) {
        std::string error_message = fmt::format(
            fmt::runtime(
                // TRN {} Is a path to file that can't be opened
                Biz::_u8L("Cannot open file: {}")
            ),
            zip_file_path
        );
        invoke_listeners<IPluginInstallationListener>(
            [&error_message](auto* listener)
            { listener->on_plugin_installation_error(error_message); }
        );
        return;

    }
    PluginBundle plugin_bundle{std::move(zip_source)};
    if (auto load_meta_result = plugin_bundle.load_meta(); !load_meta_result.has_value()) {
        invoke_listeners<IPluginInstallationListener>(
            [&load_meta_result](auto* listener)
            { listener->on_plugin_installation_error(load_meta_result.error()); }
        );
        return;
    }

    if (auto install_result = m_registry.install(plugin_bundle); !install_result.has_value()) {
        invoke_listeners<IPluginInstallationListener>(
            [&install_result](auto* listener)
            { listener->on_plugin_installation_error(install_result.error()); }
        );
        return;
    }

    rescan();

    invoke_listeners<IPluginInstallationListener>(
        [&plugin_bundle](auto* listener)
        { listener->on_plugin_installation_succeeded(plugin_bundle.meta()); }
    );

}

Yoga::Passthrough<PluginDialog>& PluginSystem::init_dialog()
{
    m_dialog = std::make_unique<PluginDialog>([this](const auto& meta, const auto& params)
    {
        m_current_plugin_data = std::make_optional<PluginData>(meta, params);
        finalize_run();
    });
    return m_dialog;
}

void PluginSystem::execute_plugin(const std::string& id)
{
    auto it = m_registry.plugins().find(id);
    if (it == m_registry.plugins().end()) {
        SPDLOG_ERROR("Cannot execute missing plugin id: {}", id);
        return;
    }
    const auto& plugin = it->second;
    if (plugin.meta().type != PluginType::ProjectPlugin) {
        SPDLOG_ERROR("Plugin id {} is not a project plugin and cannot be executed", id);
        return;
    }
    ASSERT(m_dialog.get() != nullptr);
    m_dialog->show_plugin(
        plugin.meta(),
        m_last_plugin_data.has_value() && m_last_plugin_data->meta.id == id ?
            m_last_plugin_data->param_values :
            PluginParamValueMap{}
    );
    Biz::Platform::PlatformServices::instance().render_request_handler().request_render();
}

void PluginSystem::finalize_run()
{
    ASSERT(m_current_plugin_data.has_value());

    ProjectApi project_api(m_project_interactor, m_font_manager);
    Biz::Lua::LuaEngine lua;
    lua.open_registry([&project_api](auto& lua) { project_api.register_api(lua); });
    PackageRegistry package_registry;
    lua.open_registry([&package_registry](auto& lua) { package_registry.register_api(lua); });

    const auto& plugin = m_registry.plugins().at(m_current_plugin_data->meta.id);

    try {
        plugin.execute(lua, m_current_plugin_data->param_values);
    } catch (Biz::Lua::LuaException& e) {
        SPDLOG_ERROR(
            "Running plugin {} failed in script {}\n{}",
            plugin.meta().id,
            e.script_path(),
            e.what()
        );
    } catch (std::exception& e) {
        SPDLOG_ERROR("Running plugin {} failed\n{}", plugin.meta().id, e.what());
    }

    m_last_plugin_data = m_current_plugin_data;
    m_current_plugin_data = std::nullopt;

    m_project_interactor.undo_provider().take_snapshot(Biz::UndoSnapshotType::ExecutePlugin);
}

void PluginSystem::clear()
{
    m_registry.clear();
    m_last_plugin_data = std::nullopt;
    m_current_plugin_data = std::nullopt;
}

void PluginSystem::scan(const std::string& path)
{
    m_registry.scan(path);
}

void PluginSystem::rescan()
{
    clear();
    for (const auto& path : m_plugin_paths) {
        scan(path);
    }
    invoke_listeners<IPluginRescanListener>([this](auto* l) { l->on_plugins_scanned(m_registry); });
}

} // namespace Slic3r::App::Lua
