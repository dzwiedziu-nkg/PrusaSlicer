#pragma once

#include "Slic3r/Biz/Lua/LuaEngine.hpp"

namespace Slic3r::App::Lua {

/**
 * @brief Sandboxed replacement for Lua's @c require.
 *
 * Modules are resolved through the engine's path resolver, so a plugin can only load
 * files sitting next to its own .lua file. Loaded modules are cached in the Lua
 * registry, which plugin code cannot reach.
 */
class PackageRegistry
{
public:
    void register_api(Biz::Lua::LuaEngine& lua);

private:
    sol::object safe_require(sol::this_state ts, const std::string& module_name);

private:
    Biz::Lua::LuaEngine::FilePathResolveFn m_path_resolver{nullptr};
};

} // namespace Slic3r::App::Lua
