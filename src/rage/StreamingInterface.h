#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "core/Result.h"
#include "memory/Module.h"
#include "rage/GameAddresses.h"
#include "rage/GameBuild.h"
#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// A non-owning view of one rage::strStreamingModule (an asset store such as fwTxdStore).
/// Copying it copies the address, never the store.
class StreamingModule
{
public:
    StreamingModule() = default;

    StreamingModule(uintptr_t module, size_t vtableShift)
        : m_module(module), m_vtableShift(vtableShift)
    {
    }

    [[nodiscard]] uintptr_t Raw() const
    {
        return m_module;
    }

    /// The vtable pointer, which verification checks against the game image.
    [[nodiscard]] uintptr_t GetVtable() const;

    /// The module's first global index: globalIndex = BaseIndex() + localSlot.
    [[nodiscard]] uint32_t BaseIndex() const;

    /// The slot an asset already occupies in this module. Never creates one, which is what
    /// separates it from the game's FindSlotFromHashKey.
    [[nodiscard]] std::optional<LocalSlot> FindSlot(std::string_view name) const;

    /// How many references the game holds on a loaded asset, or std::nullopt when the call
    /// could not be made.
    [[nodiscard]] std::optional<int32_t> GetNumRefs(LocalSlot slot) const;

    /// The flags of the store entry behind slot (fwAssetDef), or std::nullopt when the slot is
    /// free or out of range.
    [[nodiscard]] std::optional<uint16_t> GetAssetFlags(LocalSlot slot) const;

    /// Overwrites them. False when the slot is free, out of range, or the write faulted.
    bool SetAssetFlags(LocalSlot slot, uint16_t flags) const;

    /// How many entries the store's pool can hold.
    [[nodiscard]] uint32_t PoolSize() const;

    /// How many of them are in use.
    [[nodiscard]] uint32_t PoolUsed() const;

private:
    [[nodiscard]] const atPoolView* GetPool() const;

    /// Address of the flags field of slot's store entry, or 0 when the slot is not in use.
    [[nodiscard]] uintptr_t GetAssetFlagsAddress(LocalSlot slot) const;

    uintptr_t m_module = 0;
    size_t m_vtableShift = 0;
};

/// Access to the game's streaming manager: which modules exist, what they hold, and what a
/// global index refers to. The only writes are the registration call and SetHandle,
/// which takes a slot over for an override.
class StreamingInterface
{
public:
    /// Takes the resolved addresses and the build's vtable shift. Does not touch game memory,
    /// so a bad address only shows up in Verify().
    [[nodiscard]] Result<void> Initialize(const GameAddresses& addresses, const GameBuild& build);

    /// Proves the layout on this build before anything relies on it: the manager sits in the
    /// image, its entry array does not, the module array is plausible, the well-known modules
    /// resolve with non-overlapping index ranges, and FindSlot agrees with the vtable shift.
    /// An unusually high entry count is logged as a warning rather than rejected.
    [[nodiscard]] Result<void> Verify(const memory::Module& image) const;

    /// The asset store for an extension ("ytd"), or std::nullopt when the game has none.
    [[nodiscard]] std::optional<StreamingModule> GetModule(std::string_view extension) const;

    /// Registers a loose file with the raw streamer and gives it a slot in the module its
    /// extension belongs to. vfsPath names the file through a mount; fileName is the name the
    /// game will know it by ("spl_test.ytd"). std::nullopt means the game refused.
    [[nodiscard]] std::optional<GlobalIndex> RegisterRawFile(std::string_view vfsPath,
                                                             std::string_view fileName) const;

    /// True when RequestObject, LoadAllRequestedObjects and ReleaseObject all resolved.
    [[nodiscard]] bool CanLoadObjects() const
    {
        return m_requestObject != 0 && m_loadAllRequestedObjects != 0 && m_releaseObject != 0;
    }

    /// Asks the streamer to load one object. False when the call could not be made.
    [[nodiscard]] bool RequestObject(GlobalIndex index, int flags) const;

    /// Blocks until every requested object has loaded. False when the call could not be made.
    [[nodiscard]] bool LoadAllRequestedObjects() const;

    /// Drops the request RequestObject made, or frees an object nobody references. False when
    /// the call could not be made or the game kept the object.
    bool ReleaseObject(GlobalIndex index) const;

    /// Entries in strStreamingInfoManager, which is also the upper bound for a global index.
    [[nodiscard]] uint32_t EntryCount() const;

    /// A copy of one entry, or std::nullopt when the index is out of range.
    [[nodiscard]] std::optional<StreamingDataEntry> GetEntry(GlobalIndex index) const;

    /// Points an existing entry at another file. Only the handle changes: the load state and
    /// every other flag stay as the game left them (FiveM LoadStreamingFile.cpp:1939). False
    /// when the index is out of range or the write faulted.
    bool SetHandle(GlobalIndex index, StreamingHandle handle) const;

    /// How many asset stores the module manager holds.
    [[nodiscard]] uint32_t ModuleCount() const;

    [[nodiscard]] uintptr_t GetManagerAddress() const
    {
        return m_manager;
    }

    /// One info line per known extension: base index, pool size and pool usage. Driven by
    /// diagnostics.dump_streaming_modules.
    void DumpModules(const memory::Module& image) const;

private:
    [[nodiscard]] const strStreamingInfoManagerView* GetManager() const;

    uintptr_t m_manager = 0;
    uintptr_t m_getModuleFromExtension = 0;
    uintptr_t m_getModuleByIndex = 0; ///< optional; diagnostics only
    uintptr_t m_registerRawStreamingFile = 0;
    uintptr_t m_requestObject = 0;           ///< optional: map-store reload only
    uintptr_t m_loadAllRequestedObjects = 0; ///< optional: map-store reload only
    uintptr_t m_releaseObject = 0;           ///< optional: map-store reload only
    size_t m_vtableShift = 0;
};
} // namespace spl::rage
