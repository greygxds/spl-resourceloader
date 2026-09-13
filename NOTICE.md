# Notice

## FiveM

This project is **not affiliated with, endorsed by, or derived from Cfx.re or FiveM**, and it ships no FiveM code or binaries.

FiveM's open source was used as a **technical reference only**, to understand how GTA V's RAGE streaming system can be driven
from outside the game. The behavior it documents is the game's, not FiveM's. Where a magic value, offset or ordering rule in
this repository came from reading that source, the code cites the file and line it was learned from, as required by
`AGENTS/CODING-CONVENTIONS.md` section 3.

Components studied:

| Component | What was learned |
|---|---|
| `code/components/gta-streaming-five` | how raw streaming files are registered, how streaming handles are formed, asset-store and module layout, map-store reloading |
| `code/components/citizen-resources-core` | the resource concept: a folder with a manifest, its lifecycle and ordering |
| `code/components/citizen-resources-gta` | how a resource's `stream/` assets reach the game's streaming system |
| `code/components/rage-device-five` | the `fiDevice` virtual file-system interface and how a custom device is mounted |
| `code/components/scripthookv` | how ScriptHookV coexists with a mod inside the game process |

None of this source was copied. Class names, structures and the overall architecture of this loader are our own, and are
deliberately not a mirror of FiveM's `fx::` / `Cfx*` types.

## Trademarks

Grand Theft Auto V is a trademark of Take-Two Interactive Software, Inc. This is an unofficial modification, and is not
affiliated with or endorsed by Rockstar Games or Take-Two Interactive.
