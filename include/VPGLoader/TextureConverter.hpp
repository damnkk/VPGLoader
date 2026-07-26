#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Texture.hpp>

#include <filesystem>

namespace vpgloader::texture {

class VPGLOADER_API TextureConverter final {
public:
    // Writes an uncompressed KTX 2.0 file using an explicit Vulkan format.
    static void SaveAsKtx2(const Texture& texture,
                           const std::filesystem::path& destination);
    static void SaveAsKtx2(const TextureHandle& texture,
                           const std::filesystem::path& destination);

    // Legacy KTX 1.1 writer retained for existing callers and assets.
    static void SaveAsKtx(const Texture& texture, const std::filesystem::path& destination);
    static void SaveAsKtx(const TextureHandle& texture, const std::filesystem::path& destination);
};

} // namespace vpgloader::texture
