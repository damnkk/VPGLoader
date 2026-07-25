#pragma once

#include <VPGLoader/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace vpgloader {

enum class TextureComponentType : std::uint8_t {
    UInt8,
    UInt16,
    Float16,
    Float32,
};

enum class TextureColorSpace : std::uint8_t {
    Unknown,
    Linear,
    SRGB,
};

struct TextureFormat {
    TextureComponentType componentType = TextureComponentType::UInt8;
    std::uint8_t channels = 4;

    std::size_t bytesPerComponent() const noexcept;
    std::size_t bytesPerPixel() const noexcept;
    bool isValid() const noexcept;
};

struct TextureMipLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::size_t byteOffset = 0;
    std::size_t byteSize = 0;
};

struct TextureInfo {
    std::filesystem::path sourcePath;
    std::string sourceFileName;
    std::string sourceExtension;
    std::uintmax_t sourceFileSize = 0;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    TextureFormat format;
    TextureColorSpace colorSpace = TextureColorSpace::Unknown;
    bool sourceWasTiled = false;
    std::vector<TextureMipLevel> mipLevels;
};

class Texture;
using TextureHandle = std::shared_ptr<const Texture>;

class VPGLOADER_API Texture final {
public:
    using ByteBuffer = std::vector<std::uint8_t>;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) noexcept = default;
    Texture& operator=(Texture&&) noexcept = default;

    static TextureHandle Create(TextureInfo info, ByteBuffer&& imageData);

    const TextureInfo& info() const noexcept;
    const ByteBuffer& imageData() const noexcept;
    const std::uint8_t* data() const noexcept;
    std::size_t byteSize() const noexcept;
    std::size_t mipCount() const noexcept;

private:
    Texture(TextureInfo&& info, ByteBuffer&& imageData) noexcept;

    TextureInfo info_;
    ByteBuffer imageData_;
};

} // namespace vpgloader
