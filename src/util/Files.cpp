#include "util/Files.h"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace spl::util
{
bool WriteFileAtomically(const std::filesystem::path& file, std::string_view contents)
{
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        stream << contents;
        stream.flush();
        if (!stream)
        {
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
} // namespace spl::util
