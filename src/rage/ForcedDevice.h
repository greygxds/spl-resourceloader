#pragma once

#include <string>
#include <string_view>

namespace spl::rage
{
class ForcedDevice;

/// What the game sees: a vtable pointer where every rage::fiDevice has one, then our state.
struct ForcedDeviceObject
{
    const void* const* vtable; // +0x00
    ForcedDevice* owner;       // +0x08
};

/// A rage::fiDevice that opens one file whatever name it is asked for. The game's packfile
/// manifest loader reads a fixed name under "localPack:/"; mounting this there makes it read a
/// resource's .ymf instead.
///
/// Slots that take a file name forward to the real device with the forced path, write slots
/// refuse, and every other slot forwards unchanged, so handles stay the real device's own.
/// Pure memory code: no signature is involved, which is what lets it be tested without the game.
class ForcedDevice
{
public:
    static constexpr std::string_view kName = "SplForcedDevice";

    /// realDevice is the game's device for forcedPath (fiDevice::GetDevice). Not owned.
    ForcedDevice(void* realDevice, std::string forcedPath);

    // The game holds a pointer to m_object, so the device must never move.
    ForcedDevice(const ForcedDevice&) = delete;
    ForcedDevice& operator=(const ForcedDevice&) = delete;
    ForcedDevice(ForcedDevice&&) = delete;
    ForcedDevice& operator=(ForcedDevice&&) = delete;
    ~ForcedDevice() = default;

    /// The pointer to hand to fiDevice::MountGlobal.
    [[nodiscard]] void* GetGameDevice()
    {
        return &m_object;
    }

    [[nodiscard]] void* GetRealDevice() const
    {
        return m_realDevice;
    }

    [[nodiscard]] const char* GetForcedPath() const
    {
        return m_forcedPath.c_str();
    }

private:
    ForcedDeviceObject m_object;
    void* m_realDevice;
    std::string m_forcedPath;
};
} // namespace spl::rage
