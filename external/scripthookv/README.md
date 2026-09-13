# ScriptHookV SDK (vendored)

ScriptHookV is not a git repository, so its SDK is **committed here** rather than added as a submodule. A fresh
`git clone --recurse-submodules` therefore builds `resourceLoader.asi` with no manual setup, and CI can build and publish
releases without any secret or download step (Phase 0, section 2.2).

- **Version:** SDK v1.0.617.1a
- **Origin:** the official ScriptHookV distribution from Alexander Blade's site, http://dev-c.com/gtav/scripthookv/
- **Terms:** the SDK's own `readme.txt` states that **redistributing the archive is not allowed** and that users should be
  pointed at dev-c.com instead. Only `inc/` and `lib/` are committed here, which is the same thing other ASI mods do (for
  example MenyooSP's `Solution/external/ScriptHookV/SDK/`), so that a clone builds with no setup. The archive itself, its
  `readme.txt` and its `samples/` are git-ignored and are not part of this repository. Anyone wanting the SDK should download
  it from dev-c.com.

## What is committed here

```text
external/scripthookv/
├── inc/
│   ├── main.h            # scriptRegister / scriptUnregister / WAIT / getGameVersion / keyboard + texture APIs
│   ├── nativeCaller.h
│   ├── natives.h
│   ├── types.h
│   └── enums.h
└── lib/
    └── ScriptHookV.lib   # import library
```

`ScriptHookV.dll` and `dinput8.dll` are **never** committed. Those are runtime files that users download themselves from
dev-c.com and place in the game directory next to `resourceLoader.asi`.

## How the build uses it

`props/ScriptHookV.props` adds `inc/` as an *external* include path (so `/W4` and code analysis ignore the SDK headers) and
links `lib/ScriptHookV.lib`. Only `spl_game` and `spl_asi` import that property sheet; `spl_core`, `spl_memory` and
`spl_tests` must never see ScriptHookV headers (architecture doc, section 3).

`Directory.Build.targets` checks for `inc/main.h` and `lib/ScriptHookV.lib` before building those two projects and fails with
a message pointing here, which catches a corrupted or partial checkout.

## Updating

Download the current SDK from dev-c.com, replace `inc/` and `lib/`, update the version above, and commit deliberately:

```text
chore(deps): ScriptHookV SDK 1.0.3258.0
```
