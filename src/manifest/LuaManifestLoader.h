#pragma once

#include <cstddef>
#include <string_view>

#include "manifest/ManifestParser.h"

namespace spl::manifest
{
/// Limits applied to every manifest chunk. A manifest is code from a folder the user
/// downloaded, so it runs on a budget it cannot raise.
struct LuaSandboxLimits
{
    /// Aborts a runaway loop. Generous next to any real manifest, which executes a few
    /// hundred instructions.
    std::size_t instructionBudget = 5'000'000;

    /// Hard ceiling on everything the interpreter allocates, enforced in the allocator.
    std::size_t memoryBudgetBytes = 16u * 1024 * 1024;
};

/// Executes a manifest in a sandboxed Lua interpreter, reproducing FiveM's
/// data/shared/citizen/scripting/resource_init.lua metatable.
///
/// The environment has no io, os, package, debug or coroutine library, no load/dofile/require,
/// and only text chunks are accepted, so precompiled bytecode cannot reach the VM. Anything the
/// chunk does wrong becomes a diagnostic; nothing escapes into the process.
///
/// ParseResult::fatal means the chunk did not run to completion. Entries it managed to add
/// before failing are still in the document, and the caller decides what to do with them.
[[nodiscard]] ParseResult LoadManifestWithLua(std::string_view source, std::string_view chunkName,
                                              const LuaSandboxLimits& limits = {});
} // namespace spl::manifest
