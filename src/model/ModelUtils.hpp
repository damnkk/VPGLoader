#pragma once

#include <VPGLoader/ModelLoader.hpp>

namespace vpgloader::detail {

inline std::uint32_t CheckedIndex(std::size_t value, const char* description)
{
    if (value >= static_cast<std::size_t>(InvalidModelIndex)) {
        throw ModelLoadError(std::string(description) + " exceeds the 32-bit model index range.");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace vpgloader::detail
