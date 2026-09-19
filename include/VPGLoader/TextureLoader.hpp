#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Texture.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vpgloader {

struct TextureLoadOptions {
    bool loadMipmaps = true;
};

enum class TexturePixelLayout : std::uint8_t {
    Rgba8,
    Bgra8,
};

// Borrowed, tightly packed 2D pixels in row order. No padding or mip chain.
struct TexturePixelView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TexturePixelLayout layout = TexturePixelLayout::Rgba8;
    TextureColorSpace colorSpace = TextureColorSpace::Unknown;
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

    // Decodes an encoded image held in memory. The bytes are consumed before
    // this function returns and do not need to outlive the TextureHandle.
    static TextureHandle LoadFromMemory(std::string_view name,
                                        const std::uint8_t* data,
                                        std::size_t size,
                                        const TextureLoadOptions& options = {});

    // Copies raw pixels into an owned RGBA8 texture with one mip level.
    // Preserves row order and color-space metadata; performs no gamma conversion.
    // The source buffer may be released as soon as this function returns.
    static TextureHandle LoadFromPixels(std::string_view name,
                                        const TexturePixelView& pixels);
};

} // namespace vpgloader
