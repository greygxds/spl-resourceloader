#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "rage/types/StreamingTypes.h"

namespace spl::rage
{
/// The numbers behind the ped metadata limit.
namespace PedMetaDataLayout
{
/// How many dependencies the streamer asks a model for. A ped with more loses the rest without
/// a word, which is how clothing past the limit never shows up.
constexpr int kVanillaMaxDependencies = 100;

/// The buffer the hook has the game fill instead. Anything past it is still lost.
constexpr int kExtendedMaxDependencies = 256;

/// The flags a streaming request carries.
enum class RequestFlag : int
{
    KeepLoaded = 1 << 0,
    ForceLoad = 1 << 1,
    Priority = 1 << 2, ///< ahead of the queue
};

[[nodiscard]] constexpr RequestFlag operator|(RequestFlag left, RequestFlag right)
{
    return static_cast<RequestFlag>(static_cast<int>(left) | static_cast<int>(right));
}

/// What the game's request and release calls take.
[[nodiscard]] constexpr int ToInt(RequestFlag flags)
{
    return static_cast<int>(flags);
}

/// Keep it loaded, load it now, and ahead of the queue.
constexpr RequestFlag kResidentRequest =
    RequestFlag::KeepLoaded | RequestFlag::ForceLoad | RequestFlag::Priority;

/// What a release clears: the first two flags, which the game itself clears when it removes a
/// ped pack's expression dictionary. The priority flag is left out, because ClearRequiredFlag is
/// not meant to clear it.
constexpr RequestFlag kRelease = RequestFlag::KeepLoaded | RequestFlag::ForceLoad;

/// The "required" bits of an entry's flags: 0xF1 in the flags half, which sits in the top 16 bits
/// of StreamingDataEntry::flags.
constexpr uint32_t kRequiredFlagsMask = 0x00F10000;
} // namespace PedMetaDataLayout

/// True when an entry's flags hold any of the required bits.
[[nodiscard]] constexpr bool HasRequiredFlags(uint32_t flags)
{
    return (flags & PedMetaDataLayout::kRequiredFlagsMask) != 0;
}

/// True when an entry's flags say it is loaded.
[[nodiscard]] constexpr bool IsEntryLoaded(uint32_t flags)
{
    return LoadStateOf(StreamingDataEntry{.handle = 0, .flags = flags}) == LoadState::Loaded;
}

/// The global index of a slot the game passed as an int32_t, or std::nullopt for its -1 "no slot".
[[nodiscard]] constexpr std::optional<GlobalIndex> ToGlobalIndex(int32_t rawLocalSlot,
                                                                 uint32_t moduleBaseIndex)
{
    if (rawLocalSlot < 0)
    {
        return std::nullopt;
    }
    return GlobalIndex{moduleBaseIndex + static_cast<uint32_t>(rawLocalSlot)};
}

/// How a model's full dependency list divides up when it is longer than the caller's array.
struct DependencySplit
{
    int written = 0;       ///< entries put in the caller's array
    int rearmCount = 0;    ///< entries of the rearm array to request again
    int overflowBegin = 0; ///< all[overflowBegin..] did not fit and has to be made resident
};

/// The GetDependencies hook without the game in it. A list that fits is handed over whole. A
/// longer one is copied in order, leaving out every dependency that is resident and already
/// loaded, because those need no slot. A resident dependency that lost its required flags is
/// queued in rearm. Whatever is still left when out is full starts at overflowBegin.
template <typename TIsResident, typename TFlagsOf>
    requires std::predicate<TIsResident&, uint32_t> && std::regular_invocable<TFlagsOf&, uint32_t>
[[nodiscard]] DependencySplit SplitDependencies(std::span<const uint32_t> all, int callerCount,
                                                std::span<uint32_t> out,
                                                std::span<uint32_t> rearm, TIsResident isResident,
                                                TFlagsOf flagsOf)
{
    const int total = static_cast<int>(all.size());
    const int capacity = std::min(callerCount, static_cast<int>(out.size()));

    DependencySplit split;
    if (total <= capacity)
    {
        std::ranges::copy(all, out.begin());
        split.written = total;
        split.overflowBegin = total;
        return split;
    }

    int index = 0;
    for (; index < total && split.written < capacity; ++index)
    {
        const uint32_t dependency = all[static_cast<std::size_t>(index)];
        if (isResident(dependency))
        {
            const uint32_t flags = flagsOf(dependency);
            if (!HasRequiredFlags(flags) && split.rearmCount < static_cast<int>(rearm.size()))
            {
                rearm[static_cast<std::size_t>(split.rearmCount++)] = dependency;
            }
            if (IsEntryLoaded(flags))
            {
                continue;
            }
        }
        out[static_cast<std::size_t>(split.written++)] = dependency;
    }
    split.overflowBegin = index;
    return split;
}

/// Which model infos hold each ped .ymt, so a file is released only when the last ped lets go of
/// it. An owner is a model info's address, kept as an identity and never dereferenced.
class PedMetaOwners
{
public:
    void AddOwner(GlobalIndex index, uintptr_t owner)
    {
        m_owners[index.value].insert(owner);
    }

    /// True when owner was the last one holding index, so nobody does now. False when others
    /// remain, or when the pair was never recorded.
    [[nodiscard]] bool RemoveOwner(GlobalIndex index, uintptr_t owner)
    {
        const auto found = m_owners.find(index.value);
        if (found == m_owners.end() || found->second.erase(owner) == 0 || !found->second.empty())
        {
            return false;
        }
        m_owners.erase(found);
        return true;
    }

    [[nodiscard]] bool IsOwned(GlobalIndex index) const
    {
        return m_owners.contains(index.value);
    }

private:
    std::unordered_map<uint32_t, std::unordered_set<uintptr_t>> m_owners;
};
} // namespace spl::rage
