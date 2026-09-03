#include "Slic3r/App/Lua/PackageRegistry.hpp"

#include <sstream>

#include <boost/nowide/config.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r::App::Lua {

void PackageRegistry::register_api(Biz::Lua::LuaEngine& lua)
{
    m_path_resolver = [&lua](const std::string& file_path) -> std::string
    { return lua.resolve_file(file_path); };
    lua.state()["require"] = [this](sol::this_state ts, std::string module_name) -> sol::object
    { return safe_require(ts, module_name); };
}

sol::object PackageRegistry::safe_require(sol::this_state ts, const std::string& module_name)
{
    sol::state_view lua(ts);

    // Check the secure cache in the Lua Registry
    // The registry is inaccessible from regular Lua scripts.
    sol::table registry = lua.registry();
    sol::table secure_cache = registry["_SECURE_REQUIRE_CACHE"].get_or_create<sol::table>();

    if (secure_cache[module_name].valid()) {
        // Return cached module
        return secure_cache[module_name];
    }

    // Resolve file
    std::string file_path = m_path_resolver(module_name + ".lua");
    if (file_path.empty()) {
        luaL_error(lua.lua_state(), "System Error: Module '%s' not found.", module_name.c_str());
        return sol::lua_nil;
    }

    // Read the file purely in C++
    boost::nowide::ifstream file(file_path);
    if (!file.is_open()) {
        luaL_error(lua.lua_state(), "System Error: Authorized module '%s' not found on disk.", module_name.c_str());
        return sol::lua_nil;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    // Compile the Lua code,
    // prepend @ to file_path so lua error reporting will treat this as file
    sol::load_result loaded_chunk = lua.load(buffer.str(), "@" + file_path);
    if (!loaded_chunk.valid()) {
        sol::error err = loaded_chunk;
        luaL_error(lua.lua_state(), "Syntax Error in module '%s': %s", module_name.c_str(), err.what());
        return sol::lua_nil;
    }

    // Execute the chunk
    sol::protected_function_result result = loaded_chunk();
    if (!result.valid()) {
        sol::error err = result;
        luaL_error(lua.lua_state(), "Runtime Error in module '%s': %s", module_name.c_str(), err.what());
        return sol::lua_nil;
    }

    // Cache the result (If a module returns nothing, Lua requires it to cache as 'true')
    sol::object final_result = result;
    if (final_result.get_type() == sol::type::lua_nil) {
        final_result = sol::make_object(lua, true);
    }

    secure_cache[module_name] = final_result;

    return final_result;
}

} // namespace Slic3r::App::Lua
