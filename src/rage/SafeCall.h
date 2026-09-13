#pragma once

#include <optional>
#include <type_traits>
#include <utility>

namespace spl::rage
{
namespace detail
{
/// Runs thunk(context) under a structured-exception filter. False means the call faulted and
/// the filter has already logged where.
///
/// It lives in a .cpp, takes a function pointer and an untyped context because a frame that
/// contains __try cannot hold objects with destructors: the lambda stays in the caller's frame.
[[nodiscard]] bool InvokeGuarded(const char* what, void (*thunk)(void*), void* context);
} // namespace detail

/// Every call into game code goes through here. An access violation raised by the game or by
/// us is caught, logged with the faulting address, and turns into an empty result instead of
/// a crash; anything else keeps propagating, so ScriptHookV and the game still see it.
///
/// Returns std::optional<R> for a call with a result, and bool (true = the call completed)
/// for one without.
template <typename TFn> [[nodiscard]] auto SafeCall(const char* what, TFn&& fn)
{
    using Callable = std::remove_reference_t<TFn>;
    using ReturnType = std::invoke_result_t<Callable&>;

    if constexpr (std::is_void_v<ReturnType>)
    {
        return detail::InvokeGuarded(
            what, [](void* context) { (*static_cast<Callable*>(context))(); }, &fn);
    }
    else
    {
        std::optional<ReturnType> value;
        struct Invocation
        {
            Callable* callable;
            std::optional<ReturnType>* result;
        } invocation{&fn, &value};

        const bool completed = detail::InvokeGuarded(
            what,
            [](void* context)
            {
                auto& call = *static_cast<Invocation*>(context);
                *call.result = (*call.callable)();
            },
            &invocation);
        if (!completed)
        {
            value.reset();
        }
        return value;
    }
}

/// True once any SafeCall has caught a fault. The bridge refuses further game calls for the
/// rest of the session, because a faulted game call means our idea of the layout is wrong.
[[nodiscard]] bool HasFaulted();

/// True on a thread that is inside a SafeCall right now. The crash handler uses it to tell a
/// crash in a game call we made from one the game had on its own.
[[nodiscard]] bool IsInsideGameCall();

/// Only for tests and for the deliberate-failure check: forgets the recorded fault.
void ResetFaultState();
} // namespace spl::rage
