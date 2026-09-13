#pragma once

// Stand-in for miniz's CMake-generated export header (upstream creates it with
// GenerateExportHeader). miniz is built statically here, so the macro is empty.
// Only MINIZ_EXPORT is used by the headers we compile (miniz.h, miniz_tinfl.h).
#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif
