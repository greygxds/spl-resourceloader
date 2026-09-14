#include "manifest/LuaManifestLoader.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Lua is C and its headers carry no linkage guards of their own, unlike lua.hpp, which this
// distribution does not ship.
extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <spdlog/fmt/fmt.h>

#include "manifest/ManifestParser.h"
#include "util/Strings.h"

namespace spl::manifest
{
namespace
{
// FiveM sets this itself after a successful fxmanifest.lua and refuses it from user metadata.
constexpr std::string_view kIsCfxV2Key = "is_cfxv2";

/// Everything the running chunk may reach. It lives as long as the lua_State and is found
/// through the registry, so the C closures do not have to carry it as an upvalue.
struct SandboxState
{
    ManifestDocument* document = nullptr;
    std::vector<ManifestDiagnostic>* diagnostics = nullptr;
    std::size_t usedBytes = 0;
    std::size_t memoryBudgetBytes = 0;
};

constexpr const char* kStateRegistryKey = "spl.sandbox";

SandboxState& GetSandbox(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, kStateRegistryKey);
    auto* sandbox = static_cast<SandboxState*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return *sandbox;
}

/// Allocator with a ceiling: a manifest cannot exhaust the game's memory, however hard it
/// tries. Returning nullptr makes Lua raise a normal "not enough memory" error.
void* BoundedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
{
    auto* sandbox = static_cast<SandboxState*>(ud);

    if (nsize == 0)
    {
        std::free(ptr);
        sandbox->usedBytes -= (ptr != nullptr) ? osize : 0;
        return nullptr;
    }

    const std::size_t current = (ptr != nullptr) ? osize : 0;
    if (nsize > current && sandbox->usedBytes + (nsize - current) > sandbox->memoryBudgetBytes)
    {
        return nullptr;
    }

    void* block = std::realloc(ptr, nsize);
    if (block != nullptr)
    {
        sandbox->usedBytes = sandbox->usedBytes - current + nsize;
    }
    return block;
}

/// Runs every few instructions. It is the only thing that stops `while true do end`.
void InstructionBudgetHook(lua_State* state, lua_Debug* /*activation*/)
{
    // From now on every instruction fails, so a pcall that swallows this error cannot loop back
    // into another budget: the first instruction outside it raises again.
    lua_sethook(state, InstructionBudgetHook, LUA_MASKCOUNT, 1);
    luaL_error(state, "manifest exceeded its instruction budget (an endless loop?)");
}

/// json.encode for the values a manifest can hand to the _extra function. The decoded strings
/// are collected alongside, so nothing downstream needs a JSON parser.
std::string EncodeValue(lua_State* state, int index, std::vector<std::string>& decoded)
{
    switch (lua_type(state, index))
    {
    case LUA_TSTRING:
    {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        std::string value{text, length};
        decoded.push_back(value);
        return EncodeJsonString(value);
    }
    case LUA_TBOOLEAN:
        return lua_toboolean(state, index) != 0 ? "true" : "false";
    case LUA_TNUMBER:
    {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        return std::string{text, length};
    }
    case LUA_TTABLE:
    {
        // Only the array part, which is what json.encode of a sequence produces and what
        // ipairs would have walked.
        std::vector<std::string> values;
        const lua_Integer count = luaL_len(state, index);
        for (lua_Integer element = 1; element <= count; ++element)
        {
            lua_geti(state, index, element);
            if (lua_type(state, -1) == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char* text = lua_tolstring(state, -1, &length);
                values.emplace_back(text, length);
            }
            lua_pop(state, 1);
        }
        decoded = values;
        return EncodeJsonArray(values);
    }
    default:
        return "null";
    }
}

void AddEntry(lua_State* state, std::string key, std::string value,
              std::vector<std::string> decoded)
{
    if (util::EqualsIgnoreCase(key, kIsCfxV2Key))
    {
        return; // never taken from user metadata
    }

    // The line the call came from, so a diagnostic can point at it.
    uint32_t line = 0;
    lua_Debug activation;
    if (lua_getstack(state, 1, &activation) != 0 && lua_getinfo(state, "l", &activation) != 0)
    {
        line = static_cast<uint32_t>(activation.currentline > 0 ? activation.currentline : 0);
    }

    GetSandbox(state).document->Add(ManifestEntry{.key = std::move(key),
                                                  .value = std::move(value),
                                                  .decoded = std::move(decoded),
                                                  .line = line});
}

/// Takes any argument past the second and returns itself, so however long the chain is, the
/// next call still has a function to call.
int IgnoredArgumentFunction(lua_State* state)
{
    lua_pushcfunction(state, IgnoredArgumentFunction);
    return 1;
}

/// The function returned by the value function: `key 'a' 'b'` calls this with 'b'.
/// Upvalue 1 is newK, so the key is the de-pluralized one exactly when the first value was a
/// table, matching `newK .. '_extra'` in resource_init.lua.
int ExtraFunction(lua_State* state)
{
    const std::string key = std::string{lua_tostring(state, lua_upvalueindex(1))} + "_extra";

    std::vector<std::string> decoded;
    std::string encoded = EncodeValue(state, 1, decoded);
    AddEntry(state, key, std::move(encoded), std::move(decoded));

    // resource_init.lua returns nothing here, so a third argument is a call on nil. That is a
    // hard error in FiveM; we return another no-op instead, because a user who cannot edit the
    // resource they downloaded gains nothing from losing the whole manifest.
    lua_pushcfunction(state, IgnoredArgumentFunction);
    return 1;
}

/// The function an unknown global resolves to: `fx_version 'cerulean'` calls this with the
/// string. Upvalue 1 is the key.
int ValueFunction(lua_State* state)
{
    const std::string key = lua_tostring(state, lua_upvalueindex(1));
    std::string newKey = key;

    if (lua_type(state, 1) == LUA_TTABLE)
    {
        // A table argument loses one trailing 's': client_scripts -> client_script.
        if (key.size() > 1 && key.back() == 's')
        {
            newKey = key.substr(0, key.size() - 1);
        }

        const lua_Integer count = luaL_len(state, 1);
        for (lua_Integer element = 1; element <= count; ++element)
        {
            lua_geti(state, 1, element);
            if (lua_type(state, -1) == LUA_TSTRING)
            {
                std::size_t length = 0;
                const char* text = lua_tolstring(state, -1, &length);
                std::string value{text, length};
                AddEntry(state, newKey, value, {value});
            }
            lua_pop(state, 1);
        }
    }
    else
    {
        // A string argument keeps the key as written; resource_init.lua uses k, not newK.
        std::size_t length = 0;
        const char* text = lua_tolstring(state, 1, &length);
        std::string value = text != nullptr ? std::string{text, length} : std::string{};
        AddEntry(state, key, value, {value});
    }

    lua_pushstring(state, newKey.c_str());
    lua_pushcclosure(state, ExtraFunction, 1);
    return 1;
}

/// _G.__index: any global the chunk has not defined becomes a metadata function.
int GlobalIndex(lua_State* state)
{
    const char* key = lua_tostring(state, 2);
    if (key == nullptr)
    {
        lua_pushnil(state);
        return 1;
    }

    lua_pushstring(state, key);
    lua_pushcclosure(state, ValueFunction, 1);
    return 1;
}

/// The error on top of the stack as text. `error({})` and `error()` raise values that are not
/// strings, which lua_tostring answers with nullptr.
std::string ErrorMessage(lua_State* state)
{
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text == nullptr)
    {
        return fmt::format("manifest raised a {} value as an error", luaL_typename(state, -1));
    }
    return std::string{text, length};
}

/// Opens only the libraries a manifest could reasonably want. io, os, package, debug and
/// coroutine are never registered, so the chunk cannot reach the filesystem, spawn anything,
/// or inspect the host.
void OpenSafeLibraries(lua_State* state)
{
    static constexpr luaL_Reg kLibraries[] = {
        {LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };

    for (const luaL_Reg& library : kLibraries)
    {
        luaL_requiref(state, library.name, library.func, 1);
        lua_pop(state, 1);
    }

    // Everything in the base library that loads code or touches the host, removed by name.
    // dofile and loadfile read files; load and loadstring would take bytecode.
    for (const char* name : {"dofile", "loadfile", "load", "loadstring", "require", "print",
                             "collectgarbage", "rawset", "rawget", "setmetatable", "getmetatable"})
    {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
}
} // namespace

ParseResult LoadManifestWithLua(std::string_view source, std::string_view chunkName,
                                const LuaSandboxLimits& limits)
{
    ParseResult result;

    SandboxState sandbox{.document = &result.document,
                         .diagnostics = &result.diagnostics,
                         .usedBytes = 0,
                         .memoryBudgetBytes = limits.memoryBudgetBytes};

    lua_State* state = lua_newstate(BoundedAlloc, &sandbox);
    if (state == nullptr)
    {
        result.fatal = true;
        result.diagnostics.push_back(
            ManifestDiagnostic{.severity = ManifestDiagnostic::Severity::Error,
                               .line = 0,
                               .column = 0,
                               .message = "Could not create the Lua sandbox"});
        return result;
    }

    lua_pushlightuserdata(state, &sandbox);
    lua_setfield(state, LUA_REGISTRYINDEX, kStateRegistryKey);

    OpenSafeLibraries(state);

    // The metatable that turns undefined globals into metadata functions. It is installed
    // after the libraries, so a real global such as `string` still resolves normally.
    lua_pushglobaltable(state);
    lua_createtable(state, 0, 1);
    lua_pushcfunction(state, GlobalIndex);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_pop(state, 1);

    lua_sethook(state, InstructionBudgetHook, LUA_MASKCOUNT,
                static_cast<int>(limits.instructionBudget));

    // "t" means text only: a precompiled bytecode chunk is refused, because the bytecode
    // verifier is not a security boundary.
    const std::string name = "@" + std::string{chunkName};
    const int loaded = luaL_loadbufferx(state, source.data(), source.size(), name.c_str(), "t");
    if (loaded != LUA_OK)
    {
        result.fatal = true;
        result.diagnostics.push_back(
            ManifestDiagnostic{.severity = ManifestDiagnostic::Severity::Error,
                               .line = 0,
                               .column = 0,
                               .message = ErrorMessage(state)});
        lua_close(state);
        return result;
    }

    if (lua_pcall(state, 0, 0, 0) != LUA_OK)
    {
        result.fatal = true;
        result.diagnostics.push_back(
            ManifestDiagnostic{.severity = ManifestDiagnostic::Severity::Error,
                               .line = 0,
                               .column = 0,
                               .message = ErrorMessage(state)});
    }

    lua_close(state);
    return result;
}
} // namespace spl::manifest
