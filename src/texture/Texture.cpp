#include <VPGLoader/Texture.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace vpgloader {
namespace {

std::size_t CheckedMultiply(std::size_t left, std::size_t right)
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw std::overflow_error("Texture data size overflows size_t.");
    }
    return left * right;
}

std::size_t ExpectedMipByteSize(const TextureMipLevel& mip, const TextureFormat& format)
{
    std::size_t size = CheckedMultiply(mip.width, mip.height);
    size = CheckedMultiply(size, mip.depth);
    return CheckedMultiply(size, format.bytesPerPixel());
}

void ValidateTexture(const TextureInfo& info, const Texture::ByteBuffer& imageData)
{
    if (!info.format.isValid()) {
        throw std::invalid_argument("Texture format must have between one and four channels.");
    }
    if (info.width == 0 || info.height == 0 || info.depth == 0) {
        throw std::invalid_argument("Texture dimensions must be non-zero.");
    }
    if (info.mipLevels.empty()) {
        throw std::invalid_argument("Texture must contain at least one mip level.");
    }

    const TextureMipLevel& baseLevel = info.mipLevels.front();
    if (baseLevel.width != info.width || baseLevel.height != info.height
        || baseLevel.depth != info.depth) {
        throw std::invalid_argument("Texture base dimensions must match mip level zero.");
    }

    std::size_t nextOffset = 0;
    for (const TextureMipLevel& mip : info.mipLevels) {
        if (mip.width == 0 || mip.height == 0 || mip.depth == 0) {
            throw std::invalid_argument("Texture mip dimensions must be non-zero.");
        }
        if (mip.byteOffset != nextOffset || mip.byteSize != ExpectedMipByteSize(mip, info.format)) {
            throw std::invalid_argument("Texture mip layout is not a contiguous pixel buffer.");
        }
        if (mip.byteSize > std::numeric_limits<std::size_t>::max() - nextOffset) {
            throw std::overflow_error("Texture mip layout overflows size_t.");
        }
        nextOffset += mip.byteSize;
    }

    if (nextOffset != imageData.size()) {
        throw std::invalid_argument("Texture imageData size does not match its mip layout.");
    }
}

} // namespace

std::size_t TextureFormat::bytesPerComponent() const noexcept
{
    switch (componentType) {
    case TextureComponentType::UInt8:
        return 1;
    case TextureComponentType::UInt16:
    case TextureComponentType::Float16:
        return 2;
    case TextureComponentType::Float32:
        return 4;
    }
    return 0;
}

std::size_t TextureFormat::bytesPerPixel() const noexcept
{
    return bytesPerComponent() * channels;
}

bool TextureFormat::isValid() const noexcept
{
    return channels >= 1 && channels <= 4 && bytesPerComponent() != 0;
}

TextureHandle Texture::Create(TextureInfo info, ByteBuffer&& imageData)
{
    ValidateTexture(info, imageData);
    return TextureHandle(new Texture(std::move(info), std::move(imageData)));
}

Texture::Texture(TextureInfo&& info, ByteBuffer&& imageData) noexcept
    : info_(std::move(info))
    , imageData_(std::move(imageData))
{
}

const TextureInfo& Texture::info() const noexcept
{
    return info_;
}

const Texture::ByteBuffer& Texture::imageData() const noexcept
{
    return imageData_;
}

const std::uint8_t* Texture::data() const noexcept
{
    return imageData_.data();
}

std::size_t Texture::byteSize() const noexcept
{
    return imageData_.size();
}

std::size_t Texture::mipCount() const noexcept
{
    return info_.mipLevels.size();
}

} // namespace vpgloader
