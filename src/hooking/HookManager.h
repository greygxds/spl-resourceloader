#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Result.h"

namespace spl::hooking
{
enum class HookStatus
{
    Created, ///< MinHook knows the hook, but the original code is untouched
    Enabled,
    Failed
};

[[nodiscard]] std::string_view ToString(HookStatus status);

struct HookRecord
{
    std::string
        name; ///< the signature name of the target, e.g. "CStreaming::LoadAllRequestedObjects"
    uintptr_t target = 0;
    HookStatus status = HookStatus::Created;
};

/// MinHook's lifetime and every hook we install, by name. Hooks are only ever created from
/// rage/, which is the one place that knows what a game function is.
class HookManager
{
public:
    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;
    HookManager(HookManager&&) = delete;
    HookManager& operator=(HookManager&&) = delete;

    [[nodiscard]] static HookManager& Instance();

    /// Initializes MinHook. Idempotent, so a second ScriptMain entry is harmless.
    [[nodiscard]] Result<void> Initialize();

    /// Creates a disabled hook. The detour and original pointers must have the target's
    /// exact signature; the template only exists to keep the reinterpret_casts in one place.
    template <typename TFn>
    [[nodiscard]] Result<void> Create(std::string_view name, uintptr_t target, TFn detour,
                                      TFn* original)
    {
        return CreateRaw(name, target, reinterpret_cast<void*>(detour),
                         reinterpret_cast<void**>(original));
    }

    [[nodiscard]] Result<void> Enable(std::string_view name);

    /// Enables every created hook in one batch, so threads are suspended once instead of
    /// once per hook.
    [[nodiscard]] Result<void> EnableAll();

    void DisableAll();

    /// Disables every hook and uninitializes MinHook. Safe to call without Initialize().
    void Shutdown();

    /// One line per hook on the hook channel: name, address and status.
    void LogState() const;

    /// std::nullopt when no hook of that name was ever created.
    [[nodiscard]] std::optional<HookStatus> FindStatus(std::string_view name) const;

    [[nodiscard]] std::span<const HookRecord> GetHooks() const
    {
        return m_hooks;
    }

    [[nodiscard]] bool IsInitialized() const
    {
        return m_initialized;
    }

private:
    HookManager() = default;

    [[nodiscard]] Result<void> CreateRaw(std::string_view name, uintptr_t target, void* detour,
                                         void** original);

    /// nullptr when unknown. Non-owning, into m_hooks.
    [[nodiscard]] HookRecord* FindRecord(std::string_view name);

    std::vector<HookRecord> m_hooks;
    bool m_initialized = false;
};
} // namespace spl::hooking
