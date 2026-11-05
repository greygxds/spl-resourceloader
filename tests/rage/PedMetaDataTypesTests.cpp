#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <catch_amalgamated.hpp>

#include "rage/types/PedMetaDataTypes.h"

using namespace spl::rage;
using namespace spl::rage::PedMetaDataLayout;

namespace
{
constexpr uint32_t kFlagLoaded = 1;
constexpr uint32_t kFlagRequested = 2;
constexpr uint32_t kFirstDependency = 1000;

[[nodiscard]] std::vector<uint32_t> MakeDependencies(int count)
{
    std::vector<uint32_t> dependencies;
    for (int index = 0; index < count; ++index)
    {
        dependencies.push_back(kFirstDependency + static_cast<uint32_t>(index));
    }
    return dependencies;
}

struct SplitResult
{
    DependencySplit split;
    std::vector<uint32_t> out;
    std::vector<uint32_t> rearm;
};

/// Runs SplitDependencies with the given resident set and per-entry flags (0 when absent).
[[nodiscard]] SplitResult Split(const std::vector<uint32_t>& all, int callerCount,
                                const std::unordered_set<uint32_t>& resident = {},
                                const std::unordered_map<uint32_t, uint32_t>& flags = {})
{
    std::array<uint32_t, kExtendedMaxDependencies> out{};
    std::array<uint32_t, kExtendedMaxDependencies> rearm{};

    SplitResult result;
    result.split = SplitDependencies(
        all, callerCount, out, rearm, [&](uint32_t index) { return resident.contains(index); },
        [&](uint32_t index)
        {
            const auto found = flags.find(index);
            return found == flags.end() ? 0u : found->second;
        });
    result.out.assign(out.begin(), out.begin() + result.split.written);
    result.rearm.assign(rearm.begin(), rearm.begin() + result.split.rearmCount);
    return result;
}
} // namespace

TEST_CASE("PedMetaDataTypes: the request flags combine to the values the game takes", "[rage]")
{
    CHECK(ToInt(kResidentRequest) == 7);
    CHECK(ToInt(kRelease) == 3);
    CHECK(kRequiredFlagsMask == (0xF1u << 16));
    CHECK(kExtendedMaxDependencies > kVanillaMaxDependencies);
}

TEST_CASE("PedMetaDataTypes: required flags and load state are read from the flags word", "[rage]")
{
    CHECK_FALSE(HasRequiredFlags(0));
    CHECK_FALSE(HasRequiredFlags(kFlagLoaded | 0xFFFC)); // dependents and low flags do not count
    CHECK(HasRequiredFlags(0x00010000));
    CHECK(HasRequiredFlags(0x00800000));
    CHECK_FALSE(HasRequiredFlags(0x00020000)); // outside 0xF1

    CHECK_FALSE(IsEntryLoaded(0));
    CHECK(IsEntryLoaded(kFlagLoaded));
    CHECK_FALSE(IsEntryLoaded(kFlagRequested));
    CHECK_FALSE(IsEntryLoaded(3));
    CHECK(IsEntryLoaded(0x00F10001 | (7u << 2)));
}

TEST_CASE("PedMetaDataTypes: a slot of -1 has no global index", "[rage]")
{
    CHECK_FALSE(ToGlobalIndex(-1, 500));
    CHECK_FALSE(ToGlobalIndex(-2, 500));
    CHECK(ToGlobalIndex(0, 500) == GlobalIndex{500});
    CHECK(ToGlobalIndex(12, 500) == GlobalIndex{512});
}

TEST_CASE("PedMetaDataTypes: a list that fits is handed over whole", "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(5);

    const SplitResult result = Split(all, kVanillaMaxDependencies);

    CHECK(result.split.written == 5);
    CHECK(result.split.overflowBegin == 5);
    CHECK(result.out == all);
    CHECK(result.rearm.empty());
}

TEST_CASE("PedMetaDataTypes: a list that fits is not re-armed", "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(3);

    const SplitResult result = Split(all, kVanillaMaxDependencies, {1000, 1001}, {});

    CHECK(result.out == all);
    CHECK(result.rearm.empty());
}

TEST_CASE("PedMetaDataTypes: what does not fit starts at overflowBegin", "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(130);

    const SplitResult result = Split(all, kVanillaMaxDependencies);

    CHECK(result.split.written == kVanillaMaxDependencies);
    CHECK(result.split.overflowBegin == kVanillaMaxDependencies);
    CHECK(result.out.front() == kFirstDependency);
    CHECK(result.out.back() == kFirstDependency + 99);
    CHECK(result.rearm.empty());
}

TEST_CASE("PedMetaDataTypes: loaded resident dependencies take no slot", "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(130);
    std::unordered_set<uint32_t> resident;
    std::unordered_map<uint32_t, uint32_t> flags;
    for (uint32_t dependency = 1000; dependency < 1030; ++dependency)
    {
        resident.insert(dependency);
        flags[dependency] = kFlagLoaded | kRequiredFlagsMask;
    }

    const SplitResult result = Split(all, kVanillaMaxDependencies, resident, flags);

    CHECK(result.split.written == kVanillaMaxDependencies);
    CHECK(result.split.overflowBegin == 130); // all of them fit once the loaded ones are skipped
    CHECK(result.out.front() == 1030);
    CHECK(result.out.back() == 1129);
    CHECK(result.rearm.empty());
}

TEST_CASE("PedMetaDataTypes: a resident dependency without its flags is queued again", "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(120);
    const std::unordered_set<uint32_t> resident = {1000, 1001, 1002};
    const std::unordered_map<uint32_t, uint32_t> flags = {
        {1000, kFlagLoaded},                      // loaded, flags gone: skipped and queued
        {1001, kFlagRequested},                   // not loaded, flags gone: kept and queued
        {1002, kFlagLoaded | kRequiredFlagsMask}, // loaded and flagged: skipped, nothing to do
    };

    const SplitResult result = Split(all, kVanillaMaxDependencies, resident, flags);

    CHECK(result.rearm == std::vector<uint32_t>{1000, 1001});
    CHECK(result.out.front() == 1001);
    CHECK(result.out[1] == 1003);
    CHECK(result.split.written == kVanillaMaxDependencies);
    CHECK(result.split.overflowBegin == 102); // 1000 and 1002 were skipped, so 102 went through
}

TEST_CASE("PedMetaDataTypes: a resident dependency that is not loaded still takes its slot",
          "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(110);
    const std::unordered_set<uint32_t> resident = {1000};
    const std::unordered_map<uint32_t, uint32_t> flags = {
        {1000, kFlagRequested | kRequiredFlagsMask}};

    const SplitResult result = Split(all, kVanillaMaxDependencies, resident, flags);

    CHECK(result.out.front() == 1000);
    CHECK(result.split.written == kVanillaMaxDependencies);
    CHECK(result.split.overflowBegin == kVanillaMaxDependencies);
    CHECK(result.rearm.empty());
}

TEST_CASE("PedMetaDataTypes: the caller's count bounds what is written", "[rage]")
{
    const std::vector<uint32_t> all = MakeDependencies(40);

    const SplitResult result = Split(all, 10);

    CHECK(result.split.written == 10);
    CHECK(result.split.overflowBegin == 10);
    CHECK(result.out == MakeDependencies(10));
}

TEST_CASE("PedMetaOwners: a file is released only when its last owner lets go", "[rage]")
{
    PedMetaOwners owners;
    constexpr GlobalIndex kFile{7};
    owners.AddOwner(kFile, 0x1000);
    owners.AddOwner(kFile, 0x2000);

    CHECK(owners.IsOwned(kFile));
    CHECK_FALSE(owners.RemoveOwner(kFile, 0x1000));
    CHECK(owners.IsOwned(kFile));
    CHECK(owners.RemoveOwner(kFile, 0x2000));
    CHECK_FALSE(owners.IsOwned(kFile));
}

TEST_CASE("PedMetaOwners: an owner added twice counts once", "[rage]")
{
    PedMetaOwners owners;
    constexpr GlobalIndex kFile{7};
    owners.AddOwner(kFile, 0x1000);
    owners.AddOwner(kFile, 0x1000);

    CHECK(owners.RemoveOwner(kFile, 0x1000));
    CHECK_FALSE(owners.IsOwned(kFile));
}

TEST_CASE("PedMetaOwners: removing what was never recorded releases nothing", "[rage]")
{
    PedMetaOwners owners;
    owners.AddOwner(GlobalIndex{7}, 0x1000);

    CHECK_FALSE(owners.RemoveOwner(GlobalIndex{8}, 0x1000));    // another file
    CHECK_FALSE(owners.RemoveOwner(GlobalIndex{7}, 0x3000));    // another owner
    CHECK(owners.IsOwned(GlobalIndex{7}));
    CHECK_FALSE(PedMetaOwners{}.RemoveOwner(GlobalIndex{7}, 0x1000)); // nobody owns anything

    CHECK(owners.RemoveOwner(GlobalIndex{7}, 0x1000));
    CHECK_FALSE(owners.RemoveOwner(GlobalIndex{7}, 0x1000)); // released once, not twice
}

TEST_CASE("PedMetaOwners: files are tracked independently", "[rage]")
{
    PedMetaOwners owners;
    owners.AddOwner(GlobalIndex{1}, 0x1000);
    owners.AddOwner(GlobalIndex{2}, 0x1000);

    CHECK(owners.RemoveOwner(GlobalIndex{1}, 0x1000));
    CHECK(owners.IsOwned(GlobalIndex{2}));
}
