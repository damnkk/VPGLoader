#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace vpgloader::detail {

inline std::string LowerAscii(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

inline std::filesystem::path ResolvePath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path result = std::filesystem::absolute(path, error);
    if (error) {
        return path.lexically_normal();
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(result, error);
    return error ? result.lexically_normal() : canonical;
}

} // namespace vpgloader::detail
