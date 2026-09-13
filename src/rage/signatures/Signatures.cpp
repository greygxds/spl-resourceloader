#include <array>

#include "rage/signatures/SignatureSpec.h"

namespace spl::rage
{
namespace
{
/// THE signature table. No pattern string exists anywhere else in the codebase, so checking
/// a new game build means reading this one array.
constexpr std::array<SignatureSpec, 50> kSignatures = {{
    // The LEA at match+8 loads the manager object itself, not a pointer to it, so the
    // rip-relative target is the instance.
    {.name = "strStreamingInfoManager::sm_instance",
     .pattern = "74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41",
     .offset = 11,
     .kind = ResolveKind::RipRelative,
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:266"},
    {.name = "strStreamingModuleMgr::GetModuleFromExtension",
     .pattern = "74 15 48 8D 50 01 48 8D",
     .offset = 13,
     .kind = ResolveKind::CallTarget,
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:43"},
    {.name = "strStreamingModuleMgr::GetModule",
     .pattern = "45 33 C0 41 FF C9 41 8B C1 D1 F8 48",
     .offset = -0xD,
     .required = false, // diagnostics only
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:38"},
    {.name = "rage::fiDevice::GetDevice",
     .pattern = "41 B8 07 00 00 00 48 8B F1 E8",
     .offset = -0x1F,
     .source = "FiveM rage-device-five/src/fiDevice.cpp:10"},

    // The mount point. The vtable is the one thing we copy out of the game rather
    // than call, because we placement-construct an fiDeviceRelative of our own.
    {.name = "rage::fiDeviceRelative::vftable",
     .pattern = "48 85 C0 74 11 48 83 63 08 00 48",
     .offset = 13,
     .kind = ResolveKind::RipRelative,
     .source = "FiveM rage-device-five/src/fiDeviceClasses.cpp:134"},
    {.name = "rage::fiDeviceRelative::SetPath",
     .pattern = "49 8B F9 48 8B D9 4C 8B CA 48",
     .offset = -0x17,
     .source = "FiveM rage-device-five/src/fiDeviceClasses.cpp:20"},
    {.name = "rage::fiDeviceRelative::Mount",
     .pattern = "44 8A 81 14 01 00 00 48 8B DA 48 8B F9 48 8B D1",
     .offset = -0xD,
     .source = "FiveM rage-device-five/src/fiDeviceClasses.cpp:30"},
    {.name = "rage::fiDevice::Unmount",
     .pattern = "E8 ? ? ? ? 85 C0 75 23 48 83",
     .offset = -0x22,
     .required = false, // nothing unmounts yet
     .source = "FiveM rage-device-five/src/fiDevice.cpp:27"},

    // Registration. FiveM resolves this one to the function itself, not to a call.
    {.name = "rage::RegisterRawStreamingFile",
     .pattern = "B2 01 48 8B CD 45 8A E0 4D 0F 45 F9 E8",
     .offset = -0x25,
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:48"},
    {.name = "rage::pgRawStreamer::GetInstance",
     .pattern = "48 8B D3 4C 8B 00 48 8B C8 41 FF 90 ? 01 00 00 8B D8 E8",
     .offset = -5,
     .kind = ResolveKind::CallTarget,
     .required = false, // diagnostics and overrides
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:1694"},
    // Overrides: only read, for where the entry list sits. FiveM replaces this function.
    {.name = "rage::pgRawStreamer::GetEntryNameToBuffer",
     .pattern = "4D 63 C1 41 8B C2 41 81 E2 FF 03 00 00",
     .offset = -0xD,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3787"},

    // Proving in game that a registered asset really loads. All three are optional,
    // because nothing in the registration path needs them.
    {.name = "strStreamingInfoManager::RequestObject",
     .pattern = "41 B8 14 00 00 00 03 D3 E8",
     .offset = 8,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:18"},
    {.name = "strStreamingInfoManager::LoadAllRequestedObjects",
     .pattern = "41 B8 14 00 00 00 03 D3 E8",
     .offset = 0xF,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:8"},
    {.name = "strStreamingInfoManager::ReleaseObject",
     .pattern = "45 33 C0 03 D7 E8 ? ? ? ? 48 8B 03 48 8B CB",
     .offset = 5,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/Streaming.cpp:23"},

    // Data files. The type table is data, not code: its first row is
    // {joaat("RPF_FILE"), 0}, which is exactly what the pattern spells.
    {.name = "CDataFileMgr::sm_TypeTable",
     .pattern = "61 44 DF 04 00 00 00 00",
     .section = memory::SectionKind::Data,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3641"},
    // The array is addressed as an RVA inside "mov rcx, [r8+rax*8+disp32]".
    {.name = "CDataFileMount::sm_Interfaces",
     .pattern = "48 63 82 90 00 00 00 49 8B 8C C0 ? ? ? ? 48",
     .offset = 11,
     .kind = ResolveKind::RvaFromImageBase,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3721"},

    // The patches raw .ytyp loading needs. The first is the "jz" to nop out.
    {.name = "RawMapTypesLoadingCheck",
     .pattern = "D1 E8 A8 01 74 ? 48 8B 84",
     .offset = 4,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3821"},
    {.name = "fwMapTypesStore::vftable",
     .pattern = "45 8D 41 1C 48 8B D9 C7 40 D8 00 01 00 00",
     .offset = 22,
     .kind = ResolveKind::RipRelative,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3800"},

    // Maps. Every row is optional: a build where one of them misses keeps textures and
    // models, and only map support reports itself unavailable (MapStoreReloader::Verify).
    {.name = "fwMapDataStore::vftable",
     .pattern = "44 8D 46 0E C7 40 D8 C7 01 00 00 E8",
     .offset = 19,
     .kind = ResolveKind::RipRelative,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3812"},
    // The "jnz" that stops a box streamer from taking new bounds once it is set up.
    {.name = "BoxStreamerBoundsReconfig",
     .pattern = "80 B9 0F 01 00 00 00 75 40 4C 8B",
     .offset = 7,
     .required = false,
     .source = "FiveM gta-streaming-five/src/CacheLoader.cpp:176"},
    {.name = "CFileLoader::LoadChangeSet",
     .pattern = "48 81 EC 50 03 00 00 49 8B F0 4C",
     .offset = -0x18,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3512"},
    {.name = "ReloadMapIfNeeded",
     .pattern = "74 1F 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05",
     .offset = -0xB,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:1029"},

    // One anchor, four addresses: FiveM reads them all relative to match - 0x30
    // (LoadStreamingFile.cpp:3729). The E8 inside the pattern is EnableContentGroup's call.
    {.name = "CExtraContentManager::sm_instance",
     .pattern = "79 91 C8 BC E8 ? ? ? ? 48 8D",
     .offset = -0x30 + 0x1A,
     .kind = ResolveKind::RipRelative,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3732"},
    {.name = "CExtraContentManager::DisableContentGroup",
     .pattern = "79 91 C8 BC E8 ? ? ? ? 48 8D",
     .offset = -0x30 + 0x23,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3736"},
    {.name = "CExtraContentManager::EnableContentGroup",
     .pattern = "79 91 C8 BC E8 ? ? ? ? 48 8D",
     .offset = -0x30 + 0x34,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3737"},
    {.name = "CExtraContentManager::ClearContentCache",
     .pattern = "79 91 C8 BC E8 ? ? ? ? 48 8D",
     .offset = -0x30 + 0x5C,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .minBuild = 2189,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3738"},
    {.name = "CExtraContentManager::ClearContentCache",
     .pattern = "79 91 C8 BC E8 ? ? ? ? 48 8D",
     .offset = -0x30 + 0x50,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .maxBuild = 2188,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3738"},

    // Diagnostics: the console's map report lists interior proxies.
    {.name = "CInteriorProxy::sm_pPool",
     .pattern = "BA A1 85 94 52 41 B8 01",
     .offset = 0x34,
     .kind = ResolveKind::RipRelative,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3520"},

    // .ymf packfile manifests.
    {.name = "rage::fiDevice::MountGlobal",
     .pattern = "41 8A F0 48 8B F9 E8 ? ? ? ? 33 DB 85 C0",
     .offset = -0x28,
     .required = false,
     .source = "FiveM rage-device-five/src/fiDevice.cpp:19"},
    // The four bytes before the match are the disp32 of the LEA that loads the chunk object.
    {.name = "ManifestChunk",
     .pattern = "C7 80 ? 01 00 00 02 00 00 00 E8 ? ? ? ? 8B 06",
     .offset = -4,
     .kind = ResolveKind::RipRelative,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3614"},
    {.name = "LoadPackfileManifest",
     .pattern = "49 8B F0 4C 8B F1 48 85 D2 0F 84",
     .offset = -0x23,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:2210"},
    {.name = "InitManifestChunk",
     .pattern = "48 8D 4F 10 B2 01 48 89 2F",
     .offset = -0x2E,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:936"},
    {.name = "LoadManifestChunk",
     .pattern = "45 38 AE C0 00 00 00 0F 95 C3 E8",
     .offset = -5,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:945"},
    {.name = "ClearManifestChunk",
     .pattern = "33 FF 48 8D 4B 10 B2 01",
     .offset = -0x15,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:954"},

    // Overriding game .ytyp, .ymap and .ybn files. Optional: a miss only disables
    // that kind of override, which is then skipped with the reason.
    {.name = "fwMapTypes::ConstructArchetypes",
     .pattern = "FF 50 28 0F B7 46 20 33 ED",
     .offset = -0x21,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3873"},
    {.name = "fwArchetypeManager::FreeArchetypes",
     .pattern = "8B F9 8B DE 66 41 3B F0 73 33",
     .offset = -0x19,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:2975"},
    {.name = "fwStaticBoundsStore::ModifyHierarchyStatus",
     .pattern = "45 8B E8 4C 8B F1 83 FA FF 0F 84",
     .offset = -0x18,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3885"},
    {.name = "fwMapDataStore::ModifyHierarchyStatusRecursive",
     .pattern = "45 33 D2 84 C0 0F 84 ? 01 00 00 4C",
     .offset = -0x28,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3886"},
    // The E8 at the end of the pattern is the call that is redirected.
    {.name = "PackfileDependencyAddMapBoolEntry",
     .pattern = "48 8B CE C6 85 ? ? 00 00 01 89 44 24 20 E8",
     .offset = 14,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3879"},

    // Content that needs more than a registration. Optional: a miss disables only
    // that one thing, which then logs why.
    {.name = "CScaleformStore::InitGfxTexture",
     .pattern = "4C 23 C0 41 83 78 10 FF",
     .offset = -0x57,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:1757"},
    {.name = "CDataFileMgr::AddPackfile",
     .pattern = "EB 15 48 8B 0B 40 38 7B 0C 74 07 E8",
     .offset = 11,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:918"},
    {.name = "CVehicleModelInfo::InitPaintRamps",
     .pattern = "83 F9 FF 74 52",
     .offset = -0x34,
     .required = false,
     .minBuild = 2545,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:1473"},
    // The four bytes before the match are the disp32 of the load of the factory array.
    {.name = "fwArchetypeManager::ms_ArchetypeFactories",
     .pattern = "48 8B 0C C8 48 8B 01 FF 50 08 41 B1 01 4C",
     .offset = -4,
     .kind = ResolveKind::RipRelative,
     .required = false,
     .source = "FiveM gta-streaming-five/src/PlacementHacks.cpp:596"},
    {.name = "CPedModelInfoFactory::GetAllArchetypes",
     .pattern = "44 8B E0 4C 89 6C 24 20 44 89 6C 24 28 E8",
     .offset = 13,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:2394"},

    // Starting with the game. Optional: without them the loader starts with story
    // mode, as before.
    {.name = "rage::gameSkeleton::RunInitFunctions",
     .pattern = "BA 04 00 00 00 E8 ? ? ? ? E8 ? ? ? ? E8",
     .offset = 5,
     .kind = ResolveKind::CallTarget,
     .required = false,
     .source = "FiveM gta-core-five/src/GameSkeleton.cpp:224"},
    // The call itself (E8 rel32), redirected; the builds before 2802 call a different function.
    {.name = "rage::fiDevice::InitialMountCall",
     .pattern = "0F B7 05 ? ? ? ? 48 03 C3 44 88 34 38 66",
     .offset = 0x15,
     .required = false,
     .minBuild = 2802,
     .source = "FiveM rage-device-five/src/HookInitialMount.cpp:229"},
    {.name = "CDataFileMgr::LoadDatCall",
     .pattern = "E8 ? ? ? ? 48 8B 0D ? ? ? ? 41 B0 01 48 8B D3",
     .offset = 18,
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3622"},
    {.name = "CDataFileMgr::LoadDefDatCall",
     .pattern = "E8 ? ? ? ? 48 8B 1D ? ? ? ? 41 8B F7",
     .required = false,
     .source = "FiveM gta-streaming-five/src/LoadStreamingFile.cpp:3630"},
    // The call that sorts the update:/ relative devices in front of every other mount, which
    // would put the game's files back in front of a mod's overlay. Nopped.
    {.name = "rage::fiDevice::SortRelativeDevicesCall",
     .pattern = "C6 80 F0 00 00 00 01 E8 ? ? ? ? E8",
     .offset = 12,
     .required = false,
     .source = "FiveM rage-device-five/src/HookInitialMount.cpp:228"},
    // The imm32 of the "mov [rip+x], 100" that caps the non-DLC mounts, raised.
    {.name = "rage::fiDevice::MountLimit",
     .pattern = "C7 05 ? ? ? ? 64 00 00 00 48 8B",
     .offset = 6,
     .required = false,
     .source = "FiveM rage-device-five/src/HookInitialMount.cpp:216"},
}};
} // namespace

std::string_view ToString(ResolveKind kind)
{
    using enum ResolveKind;
    switch (kind)
    {
    case Direct:
        return "direct";
    case CallTarget:
        return "call target";
    case JumpTarget:
        return "jump target";
    case RipRelative:
        return "rip-relative";
    case RvaFromImageBase:
        return "RVA from image base";
    }
    return "direct";
}

std::span<const SignatureSpec> AllSignatures()
{
    return kSignatures;
}
} // namespace spl::rage
