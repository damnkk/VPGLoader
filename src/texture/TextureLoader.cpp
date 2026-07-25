#include <VPGLoader/TextureLoader.hpp>

#include <VPGLoader/TextureCache.hpp>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>

#include <filesystem>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace vpgloader {
namespace {

OIIO::TypeDesc ToOiioType(TextureComponentType type)
{
    switch (type) {
    case TextureComponentType::UInt8:
        return OIIO::TypeDesc::UINT8;
    case TextureComponentType::UInt16:
        return OIIO::TypeDesc::UINT16;
    case TextureComponentType::Float16:
        return OIIO::TypeDesc::HALF;
    case TextureComponentType::Float32:
        return OIIO::TypeDesc::FLOAT;
    }
    return OIIO::TypeDesc::UNKNOWN;
}

std::size_t CheckedMipByteSize(const OIIO::ImageSpec& spec, const TextureFormat& format)
{
    const auto checkedMultiply = [](std::size_t left, std::size_t right) {
        if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
            throw TextureLoadError("Texture is too large for this platform.");
        }
        return left * right;
    };

    std::size_t size = checkedMultiply(static_cast<std::size_t>(spec.width),
                                       static_cast<std::size_t>(spec.height));
    size = checkedMultiply(size, static_cast<std::size_t>(spec.depth));
    return checkedMultiply(size, format.bytesPerPixel());
}

std::filesystem::path ResolveSourcePath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::absolute(path, error);
    if (error) {
        return path;
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, error);
    return error ? resolved.lexically_normal() : canonical;
}

TextureColorSpace GetColorSpace(const OIIO::ImageSpec& spec)
{
    const std::string colorSpace = spec.get_string_attribute("oiio:ColorSpace", "");
    if (colorSpace == "sRGB" || colorSpace == "srgb") {
        return TextureColorSpace::SRGB;
    }
    if (colorSpace.find("linear") != std::string::npos || colorSpace == "Linear") {
        return TextureColorSpace::Linear;
    }
    return TextureColorSpace::Unknown;
}

[[noreturn]] void ThrowImageError(const std::string& source, const std::string& detail)
{
    throw TextureLoadError("Failed to load texture '" + source + "': " + detail);
}

TextureHandle DecodeImage(std::unique_ptr<OIIO::ImageInput> input,
                          TextureInfo info,
                          const std::string& sourceName,
                          const TextureLoadOptions& options)
{
    const OIIO::ImageSpec baseSpec = input->spec_dimensions(0, 0);
    if (baseSpec.format == OIIO::TypeDesc::UNKNOWN || baseSpec.width <= 0 || baseSpec.height <= 0
        || baseSpec.depth <= 0 || baseSpec.nchannels <= 0 || baseSpec.nchannels > 4) {
        ThrowImageError(sourceName, "unsupported dimensions or channel count");
    }
    if (baseSpec.deep) {
        ThrowImageError(sourceName, "deep images are not supported by TextureLoader");
    }

    const OIIO::TypeDesc outputType = ToOiioType(options.outputComponentType);
    if (outputType == OIIO::TypeDesc::UNKNOWN) {
        ThrowImageError(sourceName, "unsupported output component type");
    }

    info.width = static_cast<std::uint32_t>(baseSpec.width);
    info.height = static_cast<std::uint32_t>(baseSpec.height);
    info.depth = static_cast<std::uint32_t>(baseSpec.depth);
    info.format = {options.outputComponentType, static_cast<std::uint8_t>(baseSpec.nchannels)};
    info.colorSpace = GetColorSpace(baseSpec);
    info.sourceWasTiled = baseSpec.tile_width > 0;

    std::vector<OIIO::ImageSpec> mipSpecs;
    for (int mipLevel = 0;; ++mipLevel) {
        const OIIO::ImageSpec spec = input->spec_dimensions(0, mipLevel);
        if (spec.format == OIIO::TypeDesc::UNKNOWN) {
            break;
        }
        if (spec.width <= 0 || spec.height <= 0 || spec.depth <= 0 || spec.deep
            || spec.nchannels != baseSpec.nchannels) {
            ThrowImageError(sourceName, "incompatible mip level");
        }
        mipSpecs.push_back(spec);
        if (!options.loadMipmaps) {
            break;
        }
    }
    if (mipSpecs.empty()) {
        ThrowImageError(sourceName, "image contains no readable mip levels");
    }

    Texture::ByteBuffer imageData;
    std::size_t totalBytes = 0;
    for (const OIIO::ImageSpec& spec : mipSpecs) {
        const std::size_t mipBytes = CheckedMipByteSize(spec, info.format);
        if (mipBytes > std::numeric_limits<std::size_t>::max() - totalBytes) {
            ThrowImageError(sourceName, "texture data exceeds addressable memory");
        }
        info.mipLevels.push_back({static_cast<std::uint32_t>(spec.width),
                                  static_cast<std::uint32_t>(spec.height),
                                  static_cast<std::uint32_t>(spec.depth), totalBytes, mipBytes});
        totalBytes += mipBytes;
    }
    imageData.resize(totalBytes);

    for (std::size_t mipLevel = 0; mipLevel < mipSpecs.size(); ++mipLevel) {
        const TextureMipLevel& mip = info.mipLevels[mipLevel];
        if (!input->read_image(0, static_cast<int>(mipLevel), 0, mipSpecs[mipLevel].nchannels,
                               outputType, imageData.data() + mip.byteOffset)) {
            ThrowImageError(sourceName, input->geterror());
        }
    }

    return Texture::Create(std::move(info), std::move(imageData));
}

} // namespace

TextureLoadError::TextureLoadError(const std::string& message)
    : std::runtime_error(message)
{
}

TextureHandle TextureLoader::Load(const std::filesystem::path& path,
                                  const TextureLoadOptions& options)
{
    const std::string utf8Path = path.u8string();
    std::unique_ptr<OIIO::ImageInput> input = OIIO::ImageInput::open(utf8Path);
    if (!input) {
        ThrowImageError(utf8Path, OIIO::geterror());
    }

    TextureInfo info;
    info.sourcePath = ResolveSourcePath(path);
    info.sourceFileName = info.sourcePath.filename().u8string();
    info.sourceExtension = info.sourcePath.extension().u8string();
    std::error_code fileSizeError;
    info.sourceFileSize = std::filesystem::file_size(info.sourcePath, fileSizeError);
    if (fileSizeError) {
        info.sourceFileSize = 0;
    }
    return DecodeImage(std::move(input), std::move(info), utf8Path, options);
}

TextureHandle TextureLoader::LoadCached(const std::filesystem::path& path,
                                        const TextureLoadOptions& options)
{
    return TextureCache::Default().Load(path, options);
}

TextureHandle TextureLoader::LoadFromMemory(std::string_view name,
                                            const std::uint8_t* data,
                                            std::size_t size,
                                            const TextureLoadOptions& options)
{
    const std::string sourceName = name.empty() ? std::string("embedded-image") : std::string(name);
    if (data == nullptr || size == 0) {
        ThrowImageError(sourceName, "encoded image data is empty");
    }

    OIIO::Filesystem::IOMemReader memoryReader(data, size);
    std::unique_ptr<OIIO::ImageInput> input =
        OIIO::ImageInput::open(sourceName, nullptr, &memoryReader);
    if (!input) {
        ThrowImageError(sourceName, OIIO::geterror());
    }

    TextureInfo info;
    const std::filesystem::path sourceHint(sourceName);
    info.sourceFileName = sourceHint.filename().u8string();
    info.sourceExtension = sourceHint.extension().u8string();
    info.sourceFileSize = size;
    return DecodeImage(std::move(input), std::move(info), sourceName, options);
}

} // namespace vpgloader
