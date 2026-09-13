#include "hooking/HookManager.h"

#include <algorithm>

#include <MinHook.h>

#include "logging/Logger.h"

namespace spl::hooking
{
namespace
{
[[nodiscard]] Error FromMinHook(MH_STATUS status, std::string_view what)
{
    return MakeError(ErrorCode::Unavailable, "{} failed: {}", what, ::MH_StatusToString(status));
}
} // namespace

std::string_view ToString(HookStatus status)
{
    using enum HookStatus;
    switch (status)
    {
    case Created:
        return "created";
    case Enabled:
        return "enabled";
    case Failed:
        return "failed";
    }
    return "created";
}

HookManager& HookManager::Instance()
{
    static HookManager instance;
    return instance;
}

Result<void> HookManager::Initialize()
{
    if (m_initialized)
    {
        return {};
    }
    const MH_STATUS status = ::MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
    {
        return FromMinHook(status, "MinHook initialization");
    }
    m_initialized = true;
    SPL_LOG_DEBUG(Hook, "MinHook initialized");
    return {};
}

HookRecord* HookManager::FindRecord(std::string_view name)
{
    const auto match =
        std::ranges::find_if(m_hooks, [name](const HookRecord& hook) { return hook.name == name; });
    return match != m_hooks.end() ? &*match : nullptr;
}

std::optional<HookStatus> HookManager::FindStatus(std::string_view name) const
{
    const auto match =
        std::ranges::find_if(m_hooks, [name](const HookRecord& hook) { return hook.name == name; });
    if (match == m_hooks.end())
    {
        return std::nullopt;
    }
    return match->status;
}

Result<void> HookManager::CreateRaw(std::string_view name, uintptr_t target, void* detour,
                                    void** original)
{
    if (!m_initialized)
    {
        return MakeError(ErrorCode::Unavailable, "hook '{}' requested before MinHook was ready",
                         name);
    }
    if (target == 0 || detour == nullptr || original == nullptr)
    {
        return MakeError(ErrorCode::InvalidArgument, "hook '{}' has no target or no detour", name);
    }
    if (FindRecord(name) != nullptr)
    {
        return MakeError(ErrorCode::InvalidArgument, "hook '{}' already exists", name);
    }

    HookRecord record{.name = std::string(name), .target = target, .status = HookStatus::Created};
    const MH_STATUS status = ::MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
    if (status != MH_OK)
    {
        record.status = HookStatus::Failed;
        m_hooks.push_back(std::move(record));
        SPL_LOG_ERROR(Hook, "Hook '{}' at {:#x} could not be created: {}", name, target,
                      ::MH_StatusToString(status));
        return FromMinHook(status, fmt::format("creating hook '{}'", name));
    }

    m_hooks.push_back(std::move(record));
    SPL_LOG_DEBUG(Hook, "Hook '{}' created at {:#x}", name, target);
    return {};
}

Result<void> HookManager::Enable(std::string_view name)
{
    HookRecord* record = FindRecord(name);
    if (record == nullptr)
    {
        return MakeError(ErrorCode::NotFound, "no hook named '{}'", name);
    }
    if (record->status == HookStatus::Enabled)
    {
        return {};
    }

    const MH_STATUS status = ::MH_EnableHook(reinterpret_cast<void*>(record->target));
    if (status != MH_OK)
    {
        record->status = HookStatus::Failed;
        SPL_LOG_ERROR(Hook, "Hook '{}' could not be enabled: {}", name,
                      ::MH_StatusToString(status));
        return FromMinHook(status, fmt::format("enabling hook '{}'", name));
    }
    record->status = HookStatus::Enabled;
    SPL_LOG_DEBUG(Hook, "Hook '{}' enabled", name);
    return {};
}

Result<void> HookManager::EnableAll()
{
    if (!m_initialized)
    {
        return MakeError(ErrorCode::Unavailable, "MinHook is not initialized");
    }

    size_t queued = 0;
    for (HookRecord& hook : m_hooks)
    {
        if (hook.status != HookStatus::Created)
        {
            continue;
        }
        const MH_STATUS status = ::MH_QueueEnableHook(reinterpret_cast<void*>(hook.target));
        if (status != MH_OK)
        {
            hook.status = HookStatus::Failed;
            SPL_LOG_ERROR(Hook, "Hook '{}' could not be queued: {}", hook.name,
                          ::MH_StatusToString(status));
            continue;
        }
        ++queued;
    }
    if (queued == 0)
    {
        return {};
    }

    // One MH_ApplyQueued suspends the other threads once for the whole batch.
    const MH_STATUS status = ::MH_ApplyQueued();
    if (status != MH_OK)
    {
        for (HookRecord& hook : m_hooks)
        {
            if (hook.status == HookStatus::Created)
            {
                hook.status = HookStatus::Failed;
            }
        }
        return FromMinHook(status, "applying the queued hooks");
    }

    for (HookRecord& hook : m_hooks)
    {
        if (hook.status == HookStatus::Created)
        {
            hook.status = HookStatus::Enabled;
        }
    }
    SPL_LOG_DEBUG(Hook, "Enabled {} hook(s)", queued);
    return {};
}

void HookManager::DisableAll()
{
    if (!m_initialized)
    {
        return;
    }
    const MH_STATUS status = ::MH_DisableHook(MH_ALL_HOOKS);
    if (status != MH_OK)
    {
        SPL_LOG_ERROR(Hook, "Could not disable the hooks: {}", ::MH_StatusToString(status));
        return;
    }
    for (HookRecord& hook : m_hooks)
    {
        if (hook.status == HookStatus::Enabled)
        {
            hook.status = HookStatus::Created;
        }
    }
    SPL_LOG_DEBUG(Hook, "All hooks disabled");
}

void HookManager::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }
    DisableAll();
    ::MH_Uninitialize();
    m_hooks.clear();
    m_initialized = false;
    SPL_LOG_DEBUG(Hook, "MinHook shut down");
}

void HookManager::LogState() const
{
    SPL_LOG_DEBUG(Hook, "{} hook(s) registered", m_hooks.size());
    for (const HookRecord& hook : m_hooks)
    {
        SPL_LOG_DEBUG(Hook, "  '{}' at {:#x} ({})", hook.name, hook.target, ToString(hook.status));
    }
}
} // namespace spl::hooking
