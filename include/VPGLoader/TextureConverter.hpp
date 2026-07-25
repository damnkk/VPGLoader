#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Texture.hpp>

#include <filesystem>

namespace vpgloader::texture {

class VPGLOADER_API TextureConverter final {
public:
    // Writes an uncompressed KTX 1.1 file from a UInt8 Texture.
    static void SaveAsKtx(const Texture& texture, const std::filesystem::path& destination);
    static void SaveAsKtx(const TextureHandle& texture, const std::filesystem::path& destination);
};

} // namespace vpgloader::texture
