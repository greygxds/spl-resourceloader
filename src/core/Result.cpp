#include "core/Result.h"

#include <string_view>

namespace spl
{
std::string_view ToString(ErrorCode code)
{
    using enum ErrorCode;
    switch (code)
    {
    case Unknown:
        return "unknown";
    case InvalidArgument:
        return "invalid argument";
    case NotFound:
        return "not found";
    case Ambiguous:
        return "ambiguous";
    case NotSupported:
        return "not supported";
    case Io:
        return "I/O failure";
    case Parse:
        return "parse failure";
    case AccessDenied:
        return "access denied";
    case Unavailable:
        return "unavailable";
    case Refused:
        return "refused";
    }
    return "unknown";
}
} // namespace spl
