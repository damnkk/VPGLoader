#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Texture.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace vpgloader {

struct TextureLoadOptions {
    TextureComponentType outputComponentType = TextureComponentType::UInt8;
    bool loadMipmaps = true;
};

class VPGLOADER_API TextureLoadError : public std::runtime_error {
public:
    explicit TextureLoadError(const std::string& message);
};

class VPGLOADER_API TextureLoader final {
public:
    // Loads a new texture. The returned handle is the only ownership token.
    static TextureHandle Load(const std::filesystem::path& path,
                              const TextureLoadOptions& options = {});

    // Loads through the process-wide weak cache. This never copies image data.
    static TextureHandle LoadCached(const std::filesystem::path& path,
                                    const TextureLoadOptions& options = {});
};

} // namespace vpgloader
