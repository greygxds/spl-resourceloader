#pragma once

#include <cstddef>
#include <cstdint>

namespace spl::rage
{
/// The function in slot of object's vtable, typed as TFn. The only place a vtable is indexed,
/// so call sites stay readable and every slot number comes from a layout namespace
/// (conventions section 3).
///
/// The caller must have established that object is a live game object: this dereferences it.
template <typename TFn> [[nodiscard]] TFn GetVirtualFunction(uintptr_t object, size_t slot)
{
    void** const vtable = *reinterpret_cast<void***>(object);
    return reinterpret_cast<TFn>(vtable[slot]);
}

/// The vtable pointer itself, which verification checks against the game image.
[[nodiscard]] inline uintptr_t GetVtableAddress(uintptr_t object)
{
    return *reinterpret_cast<uintptr_t*>(object);
}
} // namespace spl::rage
