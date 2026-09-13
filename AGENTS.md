# Agent Guidelines

This is a FiveM resource loader for singleplayer GTA V.

## Reference material

The [FiveM source](https://github.com/citizenfx/fivem) may be used for research. If you have a local clone, treat it as read-only. Don't copy code 1:1. Pick class names, structs and designs that fit this project, and aim for a clean, maintainable codebase. When a magic value, offset or ordering rule comes from that source, cite the file and line it came from (see `NOTICE.md` and section 3 of the coding conventions).

## Conventions

All code must follow [`AGENTS/CODING-CONVENTIONS.md`](AGENTS/CODING-CONVENTIONS.md) (C++20, naming, `std::optional` instead of sentinel values, logging, commits).

## Comments

Don't comment everything. Only comment code that isn't obvious or that works around a quirk. Keep comments to one line, and put them at the end of the line they describe rather than on the line above when you can.
