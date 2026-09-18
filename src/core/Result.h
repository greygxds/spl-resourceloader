#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <spdlog/fmt/fmt.h>

namespace spl
{
/// Why an operation failed. Absence of a value is not a failure and uses std::optional
/// instead (conventions section 5).
enum class ErrorCode
{
    Unknown,
    InvalidArgument,
    NotFound,
    Ambiguous, ///< more candidates than the caller asked for, such as a pattern matching twice
    NotSupported,
    Io,
    Parse,
    AccessDenied,
    Unavailable, ///< the subsystem is not in a state where the call can work
    Refused      ///< the game ran the request and declined it, without saying why
};

[[nodiscard]] std::string_view ToString(ErrorCode code);

struct Error
{
    ErrorCode code = ErrorCode::Unknown;
    std::string message; ///< complete enough to log as it is, without added context
};

/// Builds an Error with a formatted message, so call sites stay one expression:
/// return MakeError(ErrorCode::NotFound, "no hook named '{}'", name);
template <typename... TArgs>
[[nodiscard]] Error MakeError(ErrorCode code, fmt::format_string<TArgs...> format, TArgs&&... args)
{
    return Error{code, fmt::format(format, std::forward<TArgs>(args)...)};
}

/// Either a value or an Error. The class is [[nodiscard]], which is what makes every
/// function returning it [[nodiscard]] as well.
template <typename T> class [[nodiscard]] Result
{
public:
    using ValueType = T;

    Result(T value) : m_state(std::move(value)) {}

    Result(Error error) : m_state(std::move(error)) {}

    [[nodiscard]] bool HasValue() const
    {
        return m_state.index() == 0;
    }

    explicit operator bool() const
    {
        return HasValue();
    }

    /// Only valid when HasValue(); throws otherwise, so absence must be checked first.
    [[nodiscard]] T& GetValue()
    {
        return std::get<0>(m_state);
    }

    [[nodiscard]] const T& GetValue() const
    {
        return std::get<0>(m_state);
    }

    [[nodiscard]] const Error& GetError() const
    {
        return std::get<1>(m_state);
    }

    [[nodiscard]] const std::string& GetMessage() const
    {
        return GetError().message;
    }

private:
    std::variant<T, Error> m_state;
};

/// An operation with no result value: default-constructed means success.
template <> class [[nodiscard]] Result<void>
{
public:
    Result() = default;

    Result(Error error) : m_error(std::move(error)) {}

    [[nodiscard]] bool HasValue() const
    {
        return !m_error.has_value();
    }

    explicit operator bool() const
    {
        return HasValue();
    }

    [[nodiscard]] const Error& GetError() const
    {
        return *m_error;
    }

    [[nodiscard]] const std::string& GetMessage() const
    {
        return m_error->message;
    }

private:
    std::optional<Error> m_error;
};
} // namespace spl
