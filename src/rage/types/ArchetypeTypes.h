#pragma once

#include <cstddef>
#include <cstdint>

#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// The start of CPedModelInfo, up to the stream folder. FiveM
/// gta-streaming-five/src/LoadStreamingFile.cpp:2373 (the same layout on every build it supports).
struct PedModelInfoView
{
    uintptr_t vftable;    // +0x00
    uint8_t pad_0x08[16]; // +0x08
    uint32_t hash;        // +0x18, the model name's JoaatLower
    uint8_t pad_0x1C[428];
    atArrayView<char> streamFolder; // +0x1C8, "myped" and its terminator
};
static_assert(offsetof(PedModelInfoView, hash) == 0x18);
static_assert(offsetof(PedModelInfoView, streamFolder) == 0x1C8);

namespace ArchetypeLayout
{
/// fwArchetypeManager's factory array: index 6 is the ped model info factory (FiveM
/// LoadStreamingFile.cpp:2399).
constexpr std::size_t kPedFactoryIndex = 6;

/// A ped factory holding more archetypes than this is not a ped factory.
constexpr uint16_t kMaxPedArchetypes = 16000;

/// Anything longer is not a folder name a resource would use.
constexpr std::size_t kMaxStreamFolderLength = 64;
} // namespace ArchetypeLayout
} // namespace spl::rage
