# Coding Conventions — Singleplayer Resource Loader

These rules apply to all code in this repository. Where a rule is enforceable, `.clang-format` or `.clang-tidy` enforces it (§10).
If something is not covered here, match the surrounding code.

---

## 1. Language

- **C++20** (`/std:c++20`). Do not rely on C++23 features.
- Use when they fit:
  - `std::span`, `std::string_view`, `std::optional`, `std::variant`;
  - designated initializers;
  - concepts / `requires` for template constraints;
  - `constexpr`/`consteval`;
  - `std::ranges` algorithms;
  - `std::bit_cast`;
  - `using enum` inside `switch`;
  - `[[nodiscard]]`, `[[maybe_unused]]`.
- Do not use:
  - C++20 modules (tooling support is still weak);
  - coroutines;
  - `std::format` (we already format with spdlog's bundled `fmt`; do not mix the two);
  - `std::expected` (C++23; use `spl::Result<T>`, §6).
- Exceptions: enabled (dependencies need them), but **never thrown across subsystem boundaries**. Catch at the call site that uses a
  throwing library (toml++) and convert to `Result`/`std::optional`.

---

## 2. Naming

| Element | Style | Example |
|---|---|---|
| Namespaces | `lower_case`, root `spl` | `spl::streaming`, `spl::rage` |
| Classes, structs, enums, type aliases, concepts | `PascalCase` | `StreamingPlan`, `AssetType`, `ResourceId` |
| Interfaces (pure abstract classes) | `I` + `PascalCase` | `IStreamingBackend` |
| Functions and methods | `PascalCase`, verb first | `RegisterAsset()`, `BuildPlan()` |
| Getters | `Get` + noun | `GetRootPath()` |
| Boolean queries | `Is` / `Has` / `Can` / `Should` | `IsReady()`, `HasStreamDirectory()` |
| Lookups that may find nothing | `Find` + noun → `std::optional` or `T*` (§5) | `FindSlot()`, `FindResource()` |
| Local variables, parameters | `camelCase` | `vfsPath`, `resourceCount` |
| Private/protected data members | `m_` + `camelCase` | `m_assets` |
| Static data members | `s_` + `camelCase` | `s_instance` |
| Public fields of plain structs | `camelCase`, no prefix | `config.logging.level` |
| Globals (only in `rage/` and `main/Entry.cpp`) | `g_` + `camelCase` | `g_moduleHandle` |
| Constants (`constexpr`, `const` at namespace scope) | `k` + `PascalCase` | `kMaxRegistrationsPerTick` |
| Enumerators | `PascalCase` (`enum class` only) | `AssetType::MapData` |
| Macros (avoid; prefer `constexpr`/templates) | `SPL_` + `UPPER_SNAKE` | `SPL_LOG_INFO` |
| Template parameters | `T` or `T` + `PascalCase` | `T`, `TValue`, `TCallback` |
| Files | `PascalCase`, named after the main type | `AssetScanner.h` / `AssetScanner.cpp` |
| Test files | `<Subject>Tests.cpp` | `ManifestParserTests.cpp` |
| VS projects (`.vcxproj`) | `spl_snake` | `spl_core`, `spl_tests` |
| MSBuild properties / targets / configurations | `Spl` + `PascalCase` / `Spl` + `PascalCase` / `PascalCase` | `SplWarningsAsErrors`, `SplCheckPrerequisites`, `Dev` |
| Build-feature preprocessor defines | `SPL_` + `UPPER_SNAKE` (always defined to `0`/`1`, tested with `#if`) | `SPL_DEV_TOOLS` |
| TOML config keys | `snake_case` | `allow_overrides` |
| Log channel names | `lowercase` | `streaming` |

Additional rules:
- **Abbreviations are words:** `VfsPath`, `RscHeader`, `TomlParser`, `LoadYmf`. Not `VFSPath`, `RSCHeader`. Exceptions: established RAGE type names (§3).
- **No Hungarian notation** beyond the member/static/global/constant prefixes above (`pName`, `szPath` and `dwFlags` are not allowed).
- **Units in names** whenever a number has a unit: `sizeBytes`, `timeoutMs`, `durationMs`, `virtualSizeMiB`.
- **`struct` vs `class`:** `struct` for aggregates with public fields and no invariants; `class` for everything with invariants or private state.
- Names are descriptive, not abbreviated: `resourceIndex`, not `ri`. Single-letter names are only allowed for loop counters and lambdas of ≤ 3 lines.

---

## 3. Game-specific naming (`rage/`, `memory/`, `hooking/`)

- **Game structures** keep RAGE's own name when it is known. Structs that we lay over game memory get a `View` suffix and are never
  constructed or owned by us: `strStreamingInfoManagerView`, `atPoolView`, `DataFileEntryView`. They live in `rage/types/`. These headers wrap their
  contents in `// NOLINTBEGIN(readability-identifier-naming)` … `// NOLINTEND(…)`.
- **Fields at unknown offsets** are named after the offset: `pad_0x1C[…]`, `unk_0x1B8`. Every field has an offset comment (`// +0x018`), and every
  field we read has a `static_assert(offsetof(View, field) == 0x18)`.
- **Vtable slots** are `constexpr size_t kSlot<Method>` constants inside a per-class namespace (`StreamingModuleLayout::kSlotFindSlot`). Raw numbers are never used at call sites.
- **Signatures** are named `"Class::Method"` or `"Class::sm_member"` (the RAGE spelling). The matching `GameAddresses` field is the camelCase form:
  `"strStreamingInfoManager::RegisterObject"` → `GameAddresses::streamingInfoManagerRegisterObject`.
- **Hooks:** the replacement function is `<Target>Detour`, and the saved original is `s_<target>Original`
  (`RunInitFunctionsDetour`, `s_runInitFunctionsOriginal`). The hook's log name is the signature name.
- **Code patches** have PascalCase names that describe the effect: `RawMapTypesLoadingCheck`, `MapDataStoreSyncPlacement`.
- **Magic values** from reverse engineering must cite their source:
  `// FiveM gta-streaming-five/src/LoadStreamingFile.cpp:1924 — handle = (collection << 16) | entry`.

---

## 4. Shared vocabulary

Always use these words with exactly these meanings, in code, logs and docs.

| Term | Meaning | Type |
|---|---|---|
| **resource** | a folder with `fxmanifest.lua` / `__resource.lua` | `resource::Resource` |
| **asset** | one file under a resource's `stream/` folder | `streaming::StreamAsset` |
| **file name** | lower-case basename with extension: `prop.ydr` | `std::string` |
| **streaming name** | file name without extension: `prop` | `std::string` |
| **absolute path** | full OS path | `std::filesystem::path` |
| **relative path** | path relative to the resource root: `stream/sub/prop.ydr` | `std::string` (forward slashes) |
| **VFS path** | RAGE device path: `splres:/map_one/stream/prop.ydr` | `std::string` (UTF-8) |
| **module** | a RAGE streaming module / asset store (`ytd`, `ydr`, …) | `rage::StreamingModule` |
| **global index** | index into `strStreamingInfoManager::Entries` | `rage::GlobalIndex` |
| **local slot** | index inside one module (`globalIndex = baseIndex + localSlot`) | `rage::LocalSlot` |
| **handle** | `Entries[i].handle` = `(collectionIndex << 16) \| entryIndex` | `rage::StreamingHandle` |
| **register** | make an asset known to the game (no loading) | — |
| **load / request** | make the game read the asset into memory | — |
| **override** | our asset replaces a same-named game asset | — |

`GlobalIndex`, `LocalSlot` and `StreamingHandle` are **strong types** (`struct GlobalIndex { uint32_t value; auto operator<=>(const GlobalIndex&) const = default; };`).
That makes it a compile error to mix them up. Conversion to raw integers happens only at the game-call boundary.

---

## 5. `std::optional` instead of sentinel values

**Rule:** our APIs never express "no value" with magic numbers such as `-1`, `0`, `UINT32_MAX`, `0xFFFF`, `INVALID_FILE_ATTRIBUTES`, `nullptr`
(for values), or an empty string.

| Situation | Return type |
|---|---|
| A value may legitimately be absent (lookup, parse of an optional thing) | `std::optional<T>` |
| An operation can **fail** and the caller needs to know why | `spl::Result<T>` (§6) |
| A lookup into a container **we own** returns a reference to an element | `T*` / `const T*` (`nullptr` = not found). C++20 has no `std::optional<T&>`, and the pointer is always non-owning. Document it in the declaration comment. |
| Yes/no | `bool` (never `std::optional<bool>`; use an `enum class` if there are three states) |

**Game sentinels are converted at the `rage/` boundary**, in the wrapper that makes the call, and never leak further:
```cpp
// rage/StreamingInterface.cpp
std::optional<LocalSlot> StreamingModule::FindSlot(std::string_view name) const
{
    uint32_t slot = kInvalidSlotRaw;          // 0xFFFFFFFF, named constant in rage/types
    CallVirtual<kSlotFindSlot>(m_module, &slot, name.data());
    if (slot == kInvalidSlotRaw)
    {
        return std::nullopt;
    }
    return LocalSlot{slot};
}
```
The same pattern applies to: `RegisterRawStreamingFile` (`UINT32_MAX` → `std::optional<GlobalIndex>`), `GetFileAttributes` (`INVALID_FILE_ATTRIBUTES` →
`std::optional<uint32_t>`), `pgRawStreamer::GetEntryByName` (`0xFFFF` → `std::optional<uint16_t>`), data-file type lookup (`-1` → `std::optional<DataFileTypeIndex>`),
and `Module::Find` (`nullptr` module → `std::optional<Module>`).

Using `std::optional`:
- Test with `if (auto slot = module.FindSlot(name))`, then use `*slot`. Call `.value()` only where absence is a bug, because it throws.
- `value_or()` only when the default is genuinely meaningful, not to re-introduce a sentinel (`FindSlot(n).value_or(-1)` is forbidden).
- Don't take `const std::optional<T>&` parameters for "maybe" inputs. Use an overload or a default argument. Optional *fields* in config/data structs are fine.
- Out-parameters are not used for results. Return a value, a struct, or an optional (game-call wrappers are the only exception, inside `rage/`).

---

## 6. Errors and results

- `spl::Result<T>` (`core/Result.h`): holds either a `T` or an `Error { ErrorCode code; std::string message; }`. `Result<void>` for operations with no value.
  All functions returning `Result` are `[[nodiscard]]`.
- Absence is not failure. `FindSlot` returns `std::optional`, and `RegisterAsset` returns `Result`.
- Messages are complete enough to log directly: `Result` errors are logged once, at the level where they are handled, not at every layer.
- Calls into game code go through `rage::SafeCall` (returns `std::optional<R>`; `nullopt` means the call faulted).

---

## 7. Code style

- Formatting is whatever `.clang-format` produces: LLVM base, 4 spaces, 100 columns, Allman braces, `PointerAlignment: Left`. In Visual Studio,
  enable ClangFormat support and use *Format Document* (`Ctrl+K, Ctrl+D`). Run `tools/format.ps1` before committing; `format.ps1 -Check` is authoritative in CI.
- Compiler: MSVC (`/std:c++20 /permissive- /W4`). Code must compile warning-free, because CI builds with `/WX`. Silence a warning only locally
  with `#pragma warning(suppress: NNNN) // reason`, never project-wide.
- `#pragma once` in every header.
- Include order (clang-format `IncludeBlocks: Regroup`): own header, then `<std>`, then third-party (`<spdlog/…>`, `<toml++/…>`, `<MinHook.h>`, `<main.h>`), then project headers (`"streaming/AssetType.h"`).
  Project includes are relative to `src/`.
- Include `<Windows.h>` only via `platform/Win32.h` (defines `WIN32_LEAN_AND_MEAN`, `NOMINMAX`), and never in public headers of `spl_core`.
- No `using namespace` in headers. In `.cpp` files, `using namespace` is allowed only for `std::literals` and our own sub-namespaces.
- `auto` only when the type is obvious from the right-hand side, or unspellable (lambdas, iterators).
- `const` by default: locals, parameters by `const&`, member functions. Use `[[nodiscard]]` on every function whose return value must not be ignored (queries, `Result`, `std::optional`).
- Pass cheap types by value, strings as `std::string_view` (copy to `std::string` only when storing), and ranges as `std::span`.
- Ownership: `std::unique_ptr` for owned heap objects. No raw `new`/`delete` except placement-construction of game objects. Raw pointers are always non-owning.
- One main type per `.h/.cpp` pair. Keep `.cpp`-local helpers in an anonymous namespace.
- Comments explain *why*, not *what*. Doc comments (`///`) go on public API declarations whose behavior isn't obvious from the name.

---

## 8. Logging

- Use the channel of the subsystem that emits the message (`core`, `config`, `resource`, `manifest`, `streaming`, `rage`, `hook`).
- Sentence case, with no trailing period. Put file and resource names in single quotes: `'prop.ydr' from 'map_one' overrides a game asset`.
- Game addresses are logged as module + offset: `GTA5.exe+0x2A3C5B0`.
- The default level is `warning`, so a clean launch logs only the version, the start time and the startup lines. `info` is
  what a user turns on to see what loaded; it reads as a short report, not a trace.
- Level guide:
  | Level | Use for | Test |
  |---|---|---|
  | `error` | something the user installed did not load | "your X is not working, and here is why" |
  | `warning` | the user should act, or something will likely misbehave | bad or missing file, two resources clashing, unverified build, quarantine, a feature off on this build |
  | `info` | the launch as a user would describe it: the build and bridge, one line per resource and per mod, their totals, the ready line, and rare events that change what the user sees | no addresses, handles, internal mount names or pipeline stage names |
  | `debug` | per-file, per-hook, per-patch and per-stage detail, timings, diagnostics dumps | what a bug report needs |
  | `trace` | per-call detail inside `rage/` | |
- Replacing game files is what mods are for: an override is never a warning.
- One event, one line. Put the detail of a follow-up into the same line instead of logging it again at another level.
- Paths in `info` lines use forward slashes.

---

## 9. Tests and commits

- Catch2: `TEST_CASE("ManifestParser: table argument strips trailing s", "[manifest]")`. The name is `"<Subject>: <behavior>"`, with one tag per subsystem.
- Tests never touch the real game directory. Use temp directories created per test.
- **Commit messages: Conventional Commits with a scope** — `type(scope): imperative summary`, lower case, no trailing period, ≤ 72 characters
  in the subject. The release-notes generator (`.github/scripts/generate-release-notes.ps1`) groups releases by `type`, so the type matters.

  | Type | Use for | Release-notes section |
  |---|---|---|
  | `feat` | new user-visible capability | Features |
  | `fix` | bug fix | Bug fixes |
  | `refactor`, `perf` | restructuring, speed-ups with no behavior change | Improvements |
  | `docs` | documentation, plan documents | Documentation |
  | `chore`, `build`, `ci`, `test`, `style`, `revert` | dependencies, project files, workflows, tests, formatting | Maintenance |

  **Scope** is the subsystem folder or the area: `core`, `config`, `logging`, `resource`, `manifest`, `streaming`, `memory`, `hooking`, `rage`,
  `util`, `tests`, `tools`, `deps`, `docs`. Use the most specific one; omit the scope only when a change is genuinely repo-wide.

  ```text
  feat(streaming): add duplicate policy for same-named assets
  fix(rage): convert FindSlot's -1 sentinel to std::nullopt
  chore(deps): spdlog v1.15.3 -> v1.16.0
  docs(plan): record the map-store reload trace
  ```
  A breaking change to the config file or resource layout adds `!` (`feat(config)!: rename resources key`) and explains the migration in the body.

---

## 10. Enforcement (`.clang-tidy` excerpt)

clang-tidy runs through Visual Studio's Code Analysis integration (`tools/tidy.ps1`, or *Analyze → Run Code Analysis* with Clang-Tidy enabled).
It reads this repo's `.clang-tidy`.

```yaml
CheckOptions:
  - { key: readability-identifier-naming.NamespaceCase,          value: lower_case }
  - { key: readability-identifier-naming.ClassCase,              value: CamelCase }
  - { key: readability-identifier-naming.StructCase,             value: CamelCase }
  - { key: readability-identifier-naming.EnumCase,               value: CamelCase }
  - { key: readability-identifier-naming.EnumConstantCase,       value: CamelCase }
  - { key: readability-identifier-naming.TypeAliasCase,          value: CamelCase }
  - { key: readability-identifier-naming.TemplateParameterCase,  value: CamelCase }
  - { key: readability-identifier-naming.FunctionCase,           value: CamelCase }
  - { key: readability-identifier-naming.MethodCase,             value: CamelCase }
  - { key: readability-identifier-naming.VariableCase,           value: camelBack }
  - { key: readability-identifier-naming.ParameterCase,          value: camelBack }
  - { key: readability-identifier-naming.PublicMemberCase,       value: camelBack }
  - { key: readability-identifier-naming.PrivateMemberCase,      value: camelBack }
  - { key: readability-identifier-naming.PrivateMemberPrefix,    value: m_ }
  - { key: readability-identifier-naming.ProtectedMemberCase,    value: camelBack }
  - { key: readability-identifier-naming.ProtectedMemberPrefix,  value: m_ }
  - { key: readability-identifier-naming.ClassMemberCase,        value: camelBack }
  - { key: readability-identifier-naming.ClassMemberPrefix,      value: s_ }
  - { key: readability-identifier-naming.GlobalVariableCase,     value: camelBack }
  - { key: readability-identifier-naming.GlobalVariablePrefix,   value: g_ }
  - { key: readability-identifier-naming.ConstexprVariableCase,  value: CamelCase }
  - { key: readability-identifier-naming.ConstexprVariablePrefix, value: k }
  - { key: readability-identifier-naming.MacroDefinitionCase,    value: UPPER_CASE }
  - { key: readability-identifier-naming.MacroDefinitionPrefix,  value: SPL_ }
```
The full check list (enabled families and pragmatic exclusions for reverse-engineering code) is in `.clang-tidy`.
`NOLINT` is allowed only with a reason (`// NOLINT(…): game layout`), and mainly in `rage/` and `memory/`.
