# spl::
#### a.k.a resourceLoader

This mod allows you to use FiveM style server-side **resources** and client-side **mods** in singleplayer. You don't need to adapt your mods or modify anything - it just loads them out of the box.

This tool supports **only GTA V Legacy** at this moment.

## Resources
Resources are the folders that you store on your server inside `\server-data\resources`. resourceLoader supports categories (`[category_name]`), so you can easily organize your resources.
Pretty much everything that you would need works - clothing packs, custom weapons, custom vehicles. 

Lua scripts are **not supported** at this moment. You can't run scripts using this tool and trying to run resources that heavily depend on scripts will most likely result in the resource not loading at all or just not working as expected.

## Mods
Mods are the `.rpf` archives stored inside your `FiveM\FiveM.app\mods` - things like NVE, QuantV or whatever else you use. 

## How to Install

1. Install [ScriptHookV](http://dev-c.com/gtav/scripthookv/) (put `ScriptHookV.dll` + `dinput8.dll`) in the GTA V folder (next to `GTA5.exe`).
2. From a [release](../../releases) zip, copy `resourceLoader.asi` and the `resourceLoader` folder to the GTA V folder (next to `GTA5.exe`).
3. Put resources in `<GTA V>\resourceLoader\resources\` and mods in `<GTA V>\resourceLoader\mods\`.
4. Launch your game.

If you have installed the mod and it doesn't work check the log in `resourceLoader\resourceLoader.log`. If the log states that the mod never initialized, **read this section again** because you obviously didn't install it properly.

## Configuration
You can configure this tool by modifying `resourceLoader/config.toml`.


## FAQ

### Does it work with OpenIV `mods` folder?
> You can use both at the same time, yes. Note that OpenIV `mods` folder stores actual GTA V .rpfs while FiveM `mods` folder stores resources packaged in .rpf archives. **resourceLoader is not a replacement for OpenIV mods folder** - they both do different things.

### Will lua scripts be supported?
> Probably in the future, yes. Just keep in mind that there won't be any NUI/CEF support and serverside scripts obviously won't work.

### My resource / mod isn't working, what to do?
> I tested all mods that I could think of, but it's possible that I missed something. Please create an **issue** in this repo and provide me with as much details as possible:
> - resourceLoader.log
> - description of the resource - or even more preferably a link to download the resource / mod (so I can actually troubleshoot and fix it faster)

### How do I create new resources or mods?
> This mod accepts standard FiveM resources and mods, just find a tutorial on the internet - there's plenty of them.


## Building (Developers only)

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 (Community or higher), or Build Tools 2022 | 17.10+ | Workload **Desktop development with C++** (MSVC v143, Windows 11 SDK), plus the component **C++ Clang tools for Windows** (supplies `clang-format.exe`) |
| Git | 2.30+ | Dependencies are submodules: clone with `--recurse-submodules` |

Nothing else has to be installed: the ScriptHookV SDK is vendored in
[`external/scripthookv/`](external/scripthookv/README.md), and the dependencies are pinned submodules, so the build needs no
network access after the clone.

### Build

```bash
git clone --recurse-submodules <url>
```

```powershell
tools\build.ps1 -Configuration Release
```

Or open `SpResourceLoader.sln` in Visual Studio 2022 and build `Release|x64`. The output is
`dist\<Configuration>\resourceLoader.asi` with its PDB next to it.

Everything the build produces lives under `dist/`, which is git-ignored:

```text
dist/
├── Debug/           resourceLoader.asi, spl_tests.exe, spl_sigcheck.exe and their PDBs — what you run
├── Dev/
├── Release/
├── package/         release zips from tools\package.ps1
└── intermediate/    MSBuild scratch: object files, PCHs, and the static libraries
                     that resourceLoader.asi and spl_tests.exe are linked from
```

Only the executables in `dist\<Configuration>\` are of any use. The `spl_core`, `spl_memory`,
`spl_game`, `minhook` and `lua` static libraries are link inputs, not deliverables, so they live
with the intermediates.

`SpResourceLoader.Core.slnf` is a solution filter with just the game-independent projects (`spl_core`, `spl_memory`,
`minhook`, `lua`, `spl_tests`, `spl_sigcheck`) for a fast edit/test loop: `tools\build.ps1 -Core`.

`spl_sigcheck` resolves the signature table outside the loader, to check a game build: `spl_sigcheck --process` while
the game runs (a retail `GTA5.exe` is packed on disk), or `spl_sigcheck <path to exe>` for an unpacked one. It compiles the
signature files itself and never links ScriptHookV, which keeps them free of game dependencies.

### Configurations (x64 only)

| Configuration | Use | Settings |
|---|---|---|
| `Debug` | unit tests, stepping | `/Od`, `/MTd`, `_DEBUG`. Works in game, but slowly |
| `Dev` | day-to-day in-game testing | Release codegen + `SPL_DEV_TOOLS=1` (reports itself as "Dev") |
| `Release` | shipping | `/O2`, `/MT`, `NDEBUG`, PDB, `/OPT:REF`, `/OPT:ICF` |

The CRT is linked statically, so no VC++ redistributable is needed.

### Tools

| Script | Does |
|---|---|
| `tools\build.ps1 [-Configuration Debug\|Dev\|Release] [-Core] [-Rebuild] [-Stamp] [-Clean]` | builds; `-Stamp` writes a local `src\core\BuildVersion.h`; `-Clean` removes `dist/` and that header |
| `tools\test.ps1 [-Configuration Debug]` | builds the Core filter and runs `spl_tests.exe` |
| `tools\format.ps1 [-Check]` | runs the VS-bundled clang-format over `src/` and `tests/` |
| `tools\check-projects.ps1` | verifies every `.cpp` belongs to exactly one project |
| `tools\package.ps1 [-SkipBuild] [-SkipTests] [-Version <v>]` | builds Release, runs the tests, writes the release zip, a symbols zip and SHA-256 files to `dist\package\` |

Every script finds Visual Studio through `vswhere`, so no Developer Prompt is needed.

To install a build, copy `dist\<Configuration>\resourceLoader.asi` (and its PDB, for debugging) next to `GTA5.exe`. The game
must not be running, or the file is locked. The loader creates `<GTA V>\resourceLoader\` and its default `config.toml` itself
on first run.

To make a resource to test against, create `<GTA V>\resourceLoader\resources\my_map\fxmanifest.lua`:

```lua
fx_version 'cerulean'
game 'gta5'
this_is_a_map 'yes'
```

and put assets in a `stream\` folder beside it. The repository ships no example resource, because the assets that would make
one useful cannot be redistributed.

### Testing and debugging

```powershell
tools\test.ps1
```

Catch2 options work directly: `dist\Debug\spl_tests.exe "[manifest]"`. In the IDE, the Catch2 Test Adapter extension shows the
tests in Test Explorer.

To debug in game: build `Dev`, copy the `.asi` and PDB to the game folder, start GTA V, then *Debug → Attach to Process → GTA5.exe* with the solution open. The
PDB next to the `.asi` gives full symbols.

---

## Layout

```text
src/         core, logging, config, resource, manifest, streaming, memory, hooking, rage, util
tests/       Catch2 unit tests for the game-independent code
projects/    everything MSBuild needs, and nothing else
├── *.vcxproj      one folder per project; the sources stay in src/ and tests/
├── props/         shared property sheets (Common, Dependencies, ScriptHookV)
└── Directory.Build.props/.targets
external/    pinned submodules + the vendored ScriptHookV SDK
tools/       build, test, format, check-projects, package
packaging/   the text files that go into the release zip
.github/     CI: build, tests, ASan, and a release on every push to main
dist/        all build output (git-ignored)
AGENTS/      coding conventions
```

MSBuild finds `Directory.Build.props` by walking up from each `.vcxproj`, so it works from
`projects/` and does not have to sit in the repository root.

`spl_core` and `spl_memory` never touch the game, which is why they can be unit-tested outside GTA; only `spl_game` knows
about signatures, game layouts and ScriptHookV. The split is enforced by the linker, not by discipline.

Manifests are read by running them in a sandboxed Lua 5.4 interpreter, the way FiveM does, so a manifest that computes its
values works. The sandbox opens no `io`, `os`, `package`, `debug` or `coroutine`, refuses precompiled bytecode, and caps both
instructions and memory. A chunk that will not run falls back to a text parser that recovers statement by statement.

Code style, naming and commit conventions are in [`AGENTS/CODING-CONVENTIONS.md`](AGENTS/CODING-CONVENTIONS.md).
---

## Versioning

Builds are stamped by CI as `<UTC date>_<run number>` (`Build 2026.09.12_57`), which sorts chronologically and needs no manual
bump. A local build reports `Development (local)` unless you run `tools\build.ps1 -Stamp`. The version is the first line of the
log (the console shows it under the ASCII-art banner), and the first thing to include in a bug report. Windows file properties of `resourceLoader.asi` show it
too.

CI (`.github/workflows/build.yml`) builds Release and Debug with warnings as errors, runs the tests (also under
AddressSanitizer), checks formatting and project files, and packages the zip. Every push to `main` publishes it as a GitHub
release tagged `build-<version>`, with notes grouped by commit type.


The loader is **not** affiliated with Cfx.re or FiveM. FiveM's open source was studied as a technical reference; see
[`NOTICE.md`](NOTICE.md).



