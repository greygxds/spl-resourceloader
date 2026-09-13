# Dependencies

Every third-party library is a **git submodule under `external/`, pinned to a release tag**. The superproject records the exact
commit, so every clone builds against identical sources: there are no downloads at build time, the build works offline, and
upgrading a dependency is an ordinary reviewable commit.

Clone with submodules:

```bash
git clone --recurse-submodules <url>
# or, in an existing clone:
git submodule update --init --recursive
```

`tools/build.ps1` runs `git submodule update --init --recursive` by itself when a checkout is missing, and
`Directory.Build.targets` fails the build with an actionable message if one is still absent.

---

## Pinned dependencies

| Path | Repository | Tag | Commit | License | How MSBuild consumes it |
|---|---|---|---|---|---|
| `external/spdlog` | https://github.com/gabime/spdlog | `v1.15.3` | `6fa36017cfd5731d617e1a934f0e5ea9c4445b13` | MIT | header-only (`include/`, bundled fmt) + the precompiled header |
| `external/tomlplusplus` | https://github.com/marzer/tomlplusplus | `v3.4.0` | `30172438cee64926dc41fdd9c11fb3ba5b2ba9de` | MIT | header-only (`include/`) |
| `external/minhook` | https://github.com/TsudaKageyu/minhook | *(master, no recent tag)* | `8af6b4acae5a9388fd742b56fa79ece89d96f823` | BSD-2-Clause | `projects/minhook/minhook.vcxproj` compiles `src/*.c` + `src/hde/*.c` |
| `external/Catch2` | https://github.com/catchorg/Catch2 | `v3.16.0` | `317ac1ed4c0bb6e6b91eafc817e05c488feffcb3` | BSL-1.0 | amalgamated: `extras/catch_amalgamated.cpp` is compiled into `spl_tests` and supplies `main()` |
| `external/lua` | https://github.com/lua/lua | `v5.4.9` | `312b9efaa1061c2c4cad08554dbc1351c3270eef` | MIT | `projects/lua/lua.vcxproj` (static lib) compiling the library sources |
| `external/miniz` | https://github.com/richgel999/miniz | `3.1.2` | `77d0dce8627735138c51770d1799a1ef48f2117d` | MIT | `miniz.c` + `miniz_tdef.c` + `miniz_tinfl.c` compiled into `spl_core` (warnings off, no PCH); `external/miniz-compat/miniz_export.h` stands in for the CMake-generated export header |

`external/scripthookv/` is the exception: the ScriptHookV SDK is not a git repository, so it is **vendored** (committed).
See [`scripthookv/README.md`](scripthookv/README.md).

### Why these versions

- **spdlog `v1.15.3`** — the newest `v1.15.x` patch at bootstrap time. Used header-only, so there is no separate project;
  `src/Pch.h` absorbs the compile-time cost. If build times become a problem, switch to `SPDLOG_COMPILED_LIB` with a
  `projects/spdlog/spdlog.vcxproj`.
- **toml++ `v3.4.0`** — the version the plan pins. Built with `TOML_EXCEPTIONS=1`; throwing parses are caught at the call site
  in `config/` and converted to `Result` (conventions section 1).
- **MinHook** — the last release tag is old, so a `master` commit is pinned by SHA. It is compiled by our own project rather than
  MinHook's, so it gets the same toolset, static CRT and x64 settings as the rest of the solution and cannot cause `LNK2038`
  runtime-library mismatches.
- **Catch2 `v3.16.0`** — the newest `v3.x` release. The amalgamated build keeps the test project to a single extra source file
  and needs no CMake.
- **Lua `v5.4.9`** — manifests are Lua, and FiveM executes them, so reading one exactly means running it
  (`manifest/LuaManifestLoader.cpp`). The official repository is the upstream source; only the library sources are built, never
  `lua.c`, `onelua.c` or `ltests.c`. Which standard libraries a manifest can reach is decided at runtime, in the sandbox, not by
  what gets compiled.

`fmt` is **not** a direct dependency: spdlog's bundled fmt covers all formatting.

---

## Upgrading a dependency

```bash
git -C external/spdlog fetch --tags
git -C external/spdlog checkout v1.16.0
git add external/spdlog
```

Then rebuild, run `tools/test.ps1`, update the table above (tag **and** commit), and commit with a message such as
`chore(deps): spdlog v1.15.3 -> v1.16.0`.

Nothing tracks a branch: `.gitmodules` sets `shallow = true` for every submodule and deliberately sets no `branch = ...`,
so the recorded commit is the only pin.
