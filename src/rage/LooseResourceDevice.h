#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace spl::rage
{
class LooseResourceDevice;

// NOLINTBEGIN(readability-identifier-naming): the object the game calls through
/// What the game sees: a vtable pointer where every rage::fiDevice has one, then our state.
struct LooseResourceDeviceObject
{
    const void* const* vtable;  // +0x00
    LooseResourceDevice* owner; // +0x08
};
// NOLINTEND(readability-identifier-naming)

/// A rage::fiDevice in front of one of our fiDeviceRelative mounts that lets the game stream
/// loose resources larger than 16 MiB.
///
/// The streamer keeps a file's size in a 24-bit field. For anything larger it expects the size
/// 0xFFFFFF, and then reads the real size from a header in place of the file's first 16 bytes,
/// the way resources that large sit inside an RPF. A loose file has its RSC7 header there, and a
/// plain device reports the true length, which the field truncates: the game inflates a cut-off
/// stream and stops with ERR_GEN_ZLIB_2. This device answers such files the way FiveM's resource
/// cache device does (citizen-resources-client/src/ResourceCacheDeviceV2.cpp:194-206): 0xFFFFFF
/// for their length, and the size header for a 16-byte bulk read at offset 0.
///
/// Every other call is forwarded to the real device unchanged. Pure memory code, like
/// ForcedDevice, so it is tested without the game.
class LooseResourceDevice
{
public:
    static constexpr std::string_view kName = "SplLooseResourceDevice";

    /// The size the streamer's 24-bit field uses to say "read the real size from the data".
    static constexpr uint32_t kLargeSizeMarker = 0xFFFFFF;

    /// realDevice is the mounted fiDeviceRelative this device stands in front of. Not owned.
    explicit LooseResourceDevice(void* realDevice);

    // The game holds a pointer to m_object, so the device must never move.
    LooseResourceDevice(const LooseResourceDevice&) = delete;
    LooseResourceDevice& operator=(const LooseResourceDevice&) = delete;
    LooseResourceDevice(LooseResourceDevice&&) = delete;
    LooseResourceDevice& operator=(LooseResourceDevice&&) = delete;
    ~LooseResourceDevice() = default;

    /// The pointer to mount.
    [[nodiscard]] void* GetGameDevice()
    {
        return &m_object;
    }

    [[nodiscard]] void* GetRealDevice() const
    {
        return m_realDevice;
    }

    /// The on-disk size of path when it is an RSC7 resource the 24-bit field cannot hold, asked of
    /// the real device once per path.
    [[nodiscard]] std::optional<uint32_t> FindLargeResourceSize(const char* path);

    void RememberHandle(uint64_t handle, uint32_t sizeBytes);
    [[nodiscard]] std::optional<uint32_t> FindHandle(uint64_t handle);
    void ForgetHandle(uint64_t handle);

private:
    LooseResourceDeviceObject m_object;
    void* m_realDevice;

    std::mutex m_mutex; ///< the streamer reads from several threads
    std::unordered_map<std::string, std::optional<uint32_t>> m_sizes; ///< by lower-case path
    std::unordered_map<uint64_t, uint32_t> m_handles;                 ///< open large resources
};
} // namespace spl::rage
