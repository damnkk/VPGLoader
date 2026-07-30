#include <VPGLoader/TextureLoader.hpp>

#include <VPGLoader/TextureCache.hpp>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>
#include <ktx.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace vpgloader {
namespace {

constexpr ktx_uint32_t GlRed = 0x1903;
constexpr ktx_uint32_t GlRg = 0x8227;
constexpr ktx_uint32_t GlRgb = 0x1907;
constexpr ktx_uint32_t GlRgba = 0x1908;
constexpr ktx_uint32_t GlUnsignedByte = 0x1401;
constexpr ktx_uint32_t GlUnsignedShort = 0x1403;
constexpr ktx_uint32_t GlHalfFloat = 0x140B;
constexpr ktx_uint32_t GlFloat = 0x1406;
constexpr ktx_uint32_t GlSrgb8 = 0x8C41;
constexpr ktx_uint32_t GlSrgb8Alpha8 = 0x8C43;

constexpr ktx_uint32_t VkFormatR8Unorm = 9;
constexpr ktx_uint32_t VkFormatR8Srgb = 15;
constexpr ktx_uint32_t VkFormatR8g8Unorm = 16;
constexpr ktx_uint32_t VkFormatR8g8Srgb = 22;
constexpr ktx_uint32_t VkFormatR8g8b8Unorm = 23;
constexpr ktx_uint32_t VkFormatR8g8b8Srgb = 29;
constexpr ktx_uint32_t VkFormatR8g8b8a8Unorm = 37;
constexpr ktx_uint32_t VkFormatR8g8b8a8Srgb = 43;
constexpr ktx_uint32_t VkFormatR16Unorm = 70;
constexpr ktx_uint32_t VkFormatR16Sfloat = 76;
constexpr ktx_uint32_t VkFormatR16g16Unorm = 77;
constexpr ktx_uint32_t VkFormatR16g16Sfloat = 83;
constexpr ktx_uint32_t VkFormatR16g16b16Unorm = 84;
constexpr ktx_uint32_t VkFormatR16g16b16Sfloat = 90;
constexpr ktx_uint32_t VkFormatR16g16b16a16Unorm = 91;
constexpr ktx_uint32_t VkFormatR16g16b16a16Sfloat = 97;
constexpr ktx_uint32_t VkFormatR32Sfloat = 100;
constexpr ktx_uint32_t VkFormatR32g32Sfloat = 103;
constexpr ktx_uint32_t VkFormatR32g32b32Sfloat = 106;
constexpr ktx_uint32_t VkFormatR32g32b32a32Sfloat = 109;

std::string LowerAscii(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

TextureComponentType GetOiioComponentType(const OIIO::TypeDesc& type,
                                          const std::string& sourceName)
{
    if (type == OIIO::TypeDesc::UINT8) {
        return TextureComponentType::UInt8;
    }
    if (type == OIIO::TypeDesc::UINT16) {
        return TextureComponentType::UInt16;
    }
    if (type == OIIO::TypeDesc::HALF) {
        return TextureComponentType::Float16;
    }
    if (type == OIIO::TypeDesc::FLOAT) {
        return TextureComponentType::Float32;
    }
    throw TextureLoadError(
        "Failed to load texture '" + sourceName
        + "': unsupported source component type");
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

struct KtxTextureDeleter {
    void operator()(ktxTexture* texture) const noexcept
    {
        if (texture != nullptr) {
            ktxTexture_Destroy(texture);
        }
    }
};

TextureComponentType GetKtx1ComponentType(ktx_uint32_t glType,
                                          const std::string& sourceName)
{
    switch (glType) {
    case GlUnsignedByte:
        return TextureComponentType::UInt8;
    case GlUnsignedShort:
        return TextureComponentType::UInt16;
    case GlHalfFloat:
        return TextureComponentType::Float16;
    case GlFloat:
        return TextureComponentType::Float32;
    default:
        ThrowImageError(sourceName, "unsupported KTX component type");
    }
}

std::uint8_t GetKtx1ChannelCount(ktx_uint32_t glFormat,
                                 const std::string& sourceName)
{
    switch (glFormat) {
    case GlRed:
        return 1;
    case GlRg:
        return 2;
    case GlRgb:
        return 3;
    case GlRgba:
        return 4;
    default:
        ThrowImageError(sourceName, "unsupported KTX channel layout");
    }
}

struct KtxFormatInfo {
    TextureFormat format;
    TextureColorSpace colorSpace = TextureColorSpace::Unknown;
};

KtxFormatInfo GetKtx1Format(const ktxTexture1& texture,
                            const std::string& sourceName)
{
    KtxFormatInfo result;
    result.format.componentType =
        GetKtx1ComponentType(texture.glType, sourceName);
    result.format.channels =
        GetKtx1ChannelCount(texture.glFormat, sourceName);
    result.colorSpace =
        texture.glInternalformat == GlSrgb8
            || texture.glInternalformat == GlSrgb8Alpha8
        ? TextureColorSpace::SRGB
        : TextureColorSpace::Linear;
    return result;
}

KtxFormatInfo GetKtx2Format(ktxTexture2& texture,
                            const std::string& sourceName)
{
    if (ktxTexture2_NeedsTranscoding(&texture)) {
        ThrowImageError(
            sourceName,
            "Basis Universal KTX2 textures require transcoding before CPU loading");
    }

    KtxFormatInfo result;
    switch (texture.vkFormat) {
    case VkFormatR8Unorm:
    case VkFormatR8Srgb:
        result.format = {TextureComponentType::UInt8, 1};
        break;
    case VkFormatR8g8Unorm:
    case VkFormatR8g8Srgb:
        result.format = {TextureComponentType::UInt8, 2};
        break;
    case VkFormatR8g8b8Unorm:
    case VkFormatR8g8b8Srgb:
        result.format = {TextureComponentType::UInt8, 3};
        break;
    case VkFormatR8g8b8a8Unorm:
    case VkFormatR8g8b8a8Srgb:
        result.format = {TextureComponentType::UInt8, 4};
        break;
    case VkFormatR16Unorm:
        result.format = {TextureComponentType::UInt16, 1};
        break;
    case VkFormatR16g16Unorm:
        result.format = {TextureComponentType::UInt16, 2};
        break;
    case VkFormatR16g16b16Unorm:
        result.format = {TextureComponentType::UInt16, 3};
        break;
    case VkFormatR16g16b16a16Unorm:
        result.format = {TextureComponentType::UInt16, 4};
        break;
    case VkFormatR16Sfloat:
        result.format = {TextureComponentType::Float16, 1};
        break;
    case VkFormatR16g16Sfloat:
        result.format = {TextureComponentType::Float16, 2};
        break;
    case VkFormatR16g16b16Sfloat:
        result.format = {TextureComponentType::Float16, 3};
        break;
    case VkFormatR16g16b16a16Sfloat:
        result.format = {TextureComponentType::Float16, 4};
        break;
    case VkFormatR32Sfloat:
        result.format = {TextureComponentType::Float32, 1};
        break;
    case VkFormatR32g32Sfloat:
        result.format = {TextureComponentType::Float32, 2};
        break;
    case VkFormatR32g32b32Sfloat:
        result.format = {TextureComponentType::Float32, 3};
        break;
    case VkFormatR32g32b32a32Sfloat:
        result.format = {TextureComponentType::Float32, 4};
        break;
    default:
        ThrowImageError(
            sourceName,
            "unsupported KTX2 VkFormat " + std::to_string(texture.vkFormat));
    }

    switch (ktxTexture2_GetTransferFunction_e(&texture)) {
    case KHR_DF_TRANSFER_SRGB:
        result.colorSpace = TextureColorSpace::SRGB;
        break;
    case KHR_DF_TRANSFER_LINEAR:
        result.colorSpace = TextureColorSpace::Linear;
        break;
    default:
        result.colorSpace = TextureColorSpace::Unknown;
        break;
    }
    return result;
}

std::size_t CheckedKtxMultiply(std::size_t left,
                               std::size_t right,
                               const std::string& sourceName)
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left) {
        ThrowImageError(sourceName, "KTX texture data is too large");
    }
    return left * right;
}

TextureHandle LoadKtx(const std::filesystem::path& path,
                      const TextureLoadOptions& options)
{
    const std::string sourceName = path.u8string();
    ktxTexture* rawTexture = nullptr;
    const KTX_error_code result = ktxTexture_CreateFromNamedFile(
        sourceName.c_str(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &rawTexture);
    if (result != KTX_SUCCESS) {
        ThrowImageError(sourceName, ktxErrorString(result));
    }
    std::unique_ptr<ktxTexture, KtxTextureDeleter> texture(rawTexture);

    if (texture->isCompressed || texture->isArray || texture->isCubemap
        || texture->numLayers != 1 || texture->numFaces != 1
        || texture->numLevels == 0 || texture->baseWidth == 0
        || texture->baseHeight == 0 || texture->baseDepth == 0
        || texture->pData == nullptr) {
        ThrowImageError(
            sourceName,
            "unsupported compressed, array, cubemap, or empty KTX texture");
    }

    KtxFormatInfo ktxFormat;
    if (texture->classId == ktxTexture1_c) {
        ktxFormat = GetKtx1Format(
            *reinterpret_cast<const ktxTexture1*>(texture.get()),
            sourceName);
    } else if (texture->classId == ktxTexture2_c) {
        ktxFormat = GetKtx2Format(
            *reinterpret_cast<ktxTexture2*>(texture.get()),
            sourceName);
    } else {
        ThrowImageError(sourceName, "unknown KTX container version");
    }
    const TextureFormat format = ktxFormat.format;

    TextureInfo info;
    info.sourcePath = ResolveSourcePath(path);
    info.sourceFileName = info.sourcePath.filename().u8string();
    info.sourceExtension = info.sourcePath.extension().u8string();
    std::error_code fileSizeError;
    info.sourceFileSize =
        std::filesystem::file_size(info.sourcePath, fileSizeError);
    if (fileSizeError) {
        info.sourceFileSize = 0;
    }
    info.width = texture->baseWidth;
    info.height = texture->baseHeight;
    info.depth = texture->baseDepth;
    info.format = format;
    info.colorSpace = ktxFormat.colorSpace;

    std::uint32_t maximumLevels = 1;
    for (std::uint32_t dimension =
             std::max({texture->baseWidth, texture->baseHeight, texture->baseDepth});
         dimension > 1;
         dimension >>= 1) {
        ++maximumLevels;
    }
    if (texture->numLevels > maximumLevels) {
        ThrowImageError(sourceName, "KTX texture contains too many mip levels");
    }

    const std::uint32_t levelCount =
        options.loadMipmaps ? texture->numLevels : 1U;
    Texture::ByteBuffer imageData;
    std::size_t totalBytes = 0;
    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const std::uint32_t width =
            std::max(1U, texture->baseWidth >> level);
        const std::uint32_t height =
            std::max(1U, texture->baseHeight >> level);
        const std::uint32_t depth =
            std::max(1U, texture->baseDepth >> level);
        std::size_t byteSize = CheckedKtxMultiply(width, height, sourceName);
        byteSize = CheckedKtxMultiply(byteSize, depth, sourceName);
        byteSize = CheckedKtxMultiply(
            byteSize, format.bytesPerPixel(), sourceName);
        if (byteSize > std::numeric_limits<std::size_t>::max() - totalBytes) {
            ThrowImageError(sourceName, "KTX texture data is too large");
        }
        info.mipLevels.push_back(
            {width, height, depth, totalBytes, byteSize});
        totalBytes += byteSize;
    }
    imageData.resize(totalBytes);

    for (std::uint32_t level = 0; level < levelCount; ++level) {
        const TextureMipLevel& mip = info.mipLevels[level];
        ktx_size_t sourceOffset = 0;
        const KTX_error_code offsetResult = ktxTexture_GetImageOffset(
            texture.get(), level, 0, 0, &sourceOffset);
        if (offsetResult != KTX_SUCCESS) {
            ThrowImageError(sourceName, ktxErrorString(offsetResult));
        }

        const std::size_t tightRowSize =
            static_cast<std::size_t>(mip.width) * format.bytesPerPixel();
        // KTX1 rows follow GL_UNPACK_ALIGNMENT. KTX2 removed row padding,
        // while libktx's generic GetRowPitch still reports the GL-aligned
        // pitch for uncompressed three-channel formats.
        const std::size_t rowPitch =
            texture->classId == ktxTexture2_c
            ? tightRowSize
            : ktxTexture_GetRowPitch(texture.get(), level);
        std::size_t storedSize =
            CheckedKtxMultiply(rowPitch, mip.height, sourceName);
        storedSize =
            CheckedKtxMultiply(storedSize, mip.depth, sourceName);
        if (rowPitch < tightRowSize
            || sourceOffset > texture->dataSize
            || storedSize > texture->dataSize - sourceOffset) {
            ThrowImageError(
                sourceName,
                "invalid KTX mip layout at level " + std::to_string(level)
                    + " (rowPitch=" + std::to_string(rowPitch)
                    + ", tightRowSize=" + std::to_string(tightRowSize)
                    + ", offset=" + std::to_string(sourceOffset)
                    + ", storedSize=" + std::to_string(storedSize)
                    + ", dataSize=" + std::to_string(texture->dataSize)
                    + ")");
        }

        const std::uint8_t* source = texture->pData + sourceOffset;
        std::uint8_t* destination = imageData.data() + mip.byteOffset;
        for (std::uint32_t depth = 0; depth < mip.depth; ++depth) {
            for (std::uint32_t row = 0; row < mip.height; ++row) {
                const std::size_t sourceRow =
                    (static_cast<std::size_t>(depth) * mip.height + row)
                    * rowPitch;
                const std::size_t destinationRow =
                    (static_cast<std::size_t>(depth) * mip.height + row)
                    * tightRowSize;
                std::memcpy(
                    destination + destinationRow,
                    source + sourceRow,
                    tightRowSize);
            }
        }
    }

    return Texture::Create(std::move(info), std::move(imageData));
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

    const TextureComponentType componentType =
        GetOiioComponentType(baseSpec.format, sourceName);

    info.width = static_cast<std::uint32_t>(baseSpec.width);
    info.height = static_cast<std::uint32_t>(baseSpec.height);
    info.depth = static_cast<std::uint32_t>(baseSpec.depth);
    info.format = {componentType, static_cast<std::uint8_t>(baseSpec.nchannels)};
    info.colorSpace = GetColorSpace(baseSpec);
    info.sourceWasTiled = baseSpec.tile_width > 0;

    std::vector<OIIO::ImageSpec> mipSpecs;
    for (int mipLevel = 0;; ++mipLevel) {
        const OIIO::ImageSpec spec = input->spec_dimensions(0, mipLevel);
        if (spec.format == OIIO::TypeDesc::UNKNOWN) {
            break;
        }
        if (spec.width <= 0 || spec.height <= 0 || spec.depth <= 0 || spec.deep
            || spec.nchannels != baseSpec.nchannels || spec.format != baseSpec.format) {
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
                               baseSpec.format, imageData.data() + mip.byteOffset)) {
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
    const std::string extension =
        LowerAscii(path.extension().u8string());
    if (extension == ".ktx" || extension == ".ktx2") {
        return LoadKtx(path, options);
    }

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
