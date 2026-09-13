#pragma once

// The only place <Windows.h> is included from (AGENTS/CODING-CONVENTIONS.md §7).
// Never include this from a public header of spl_core.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
