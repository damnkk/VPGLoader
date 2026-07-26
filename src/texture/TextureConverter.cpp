#include <VPGLoader/TextureConverter.hpp>

#include <ktx.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace vpgloader::texture {
namespace {

constexpr ktx_uint32_t GlR8 = 0x8229;
constexpr ktx_uint32_t GlRg8 = 0x822B;
constexpr ktx_uint32_t GlRgb8 = 0x8051;
constexpr ktx_uint32_t GlRgba8 = 0x8058;
constexpr ktx_uint32_t GlR16 = 0x822A;
constexpr ktx_uint32_t GlRg16 = 0x822C;
constexpr ktx_uint32_t GlRgb16 = 0x8054;
constexpr ktx_uint32_t GlRgba16 = 0x805B;
constexpr ktx_uint32_t GlR16f = 0x822D;
constexpr ktx_uint32_t GlRg16f = 0x822F;
constexpr ktx_uint32_t GlRgb16f = 0x881B;
constexpr ktx_uint32_t GlRgba16f = 0x881A;
constexpr ktx_uint32_t GlR32f = 0x822E;
constexpr ktx_uint32_t GlRg32f = 0x8230;
constexpr ktx_uint32_t GlRgb32f = 0x8815;
constexpr ktx_uint32_t GlRgba32f = 0x8814;
constexpr ktx_uint32_t GlSrgb8 = 0x8C41;
constexpr ktx_uint32_t GlSrgb8Alpha8 = 0x8C43;

// VkFormat values are part of the stable Vulkan ABI. Keeping these local
// avoids making the CPU-only VPGLoader target depend on a Vulkan SDK.
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

ktx_uint32_t ToGlInternalFormat(const TextureFormat& format,
                                TextureColorSpace colorSpace)
{
    if (!format.isValid()) {
        throw std::invalid_argument(
            "SaveAsKtx requires between one and four channels.");
    }

    if (format.componentType == TextureComponentType::UInt8
        && colorSpace == TextureColorSpace::SRGB) {
        if (format.channels == 3) {
            return GlSrgb8;
        }
        if (format.channels == 4) {
            return GlSrgb8Alpha8;
        }
    }

    const ktx_uint32_t formats[][4] = {
        {GlR8, GlRg8, GlRgb8, GlRgba8},
        {GlR16, GlRg16, GlRgb16, GlRgba16},
        {GlR16f, GlRg16f, GlRgb16f, GlRgba16f},
        {GlR32f, GlRg32f, GlRgb32f, GlRgba32f},
    };
    const std::size_t typeIndex =
        static_cast<std::size_t>(format.componentType);
    if (typeIndex >= std::size(formats)) {
        throw std::invalid_argument(
            "SaveAsKtx received an unsupported component type.");
    }
    return formats[typeIndex][format.channels - 1];
}

ktx_uint32_t ToVkFormat(const TextureFormat& format,
                        TextureColorSpace colorSpace)
{
    if (!format.isValid()) {
        throw std::invalid_argument(
            "SaveAsKtx2 requires between one and four channels.");
    }

    const ktx_uint32_t uint8Linear[] = {
        VkFormatR8Unorm,
        VkFormatR8g8Unorm,
        VkFormatR8g8b8Unorm,
        VkFormatR8g8b8a8Unorm,
    };
    const ktx_uint32_t uint8Srgb[] = {
        VkFormatR8Srgb,
        VkFormatR8g8Srgb,
        VkFormatR8g8b8Srgb,
        VkFormatR8g8b8a8Srgb,
    };
    const ktx_uint32_t uint16[] = {
        VkFormatR16Unorm,
        VkFormatR16g16Unorm,
        VkFormatR16g16b16Unorm,
        VkFormatR16g16b16a16Unorm,
    };
    const ktx_uint32_t float16[] = {
        VkFormatR16Sfloat,
        VkFormatR16g16Sfloat,
        VkFormatR16g16b16Sfloat,
        VkFormatR16g16b16a16Sfloat,
    };
    const ktx_uint32_t float32[] = {
        VkFormatR32Sfloat,
        VkFormatR32g32Sfloat,
        VkFormatR32g32b32Sfloat,
        VkFormatR32g32b32a32Sfloat,
    };
    const std::size_t channelIndex = format.channels - 1;

    switch (format.componentType) {
    case TextureComponentType::UInt8:
        return colorSpace == TextureColorSpace::SRGB
            ? uint8Srgb[channelIndex]
            : uint8Linear[channelIndex];
    case TextureComponentType::UInt16:
        return uint16[channelIndex];
    case TextureComponentType::Float16:
        return float16[channelIndex];
    case TextureComponentType::Float32:
        return float32[channelIndex];
    }
    throw std::invalid_argument(
        "SaveAsKtx2 received an unsupported component type.");
}

template <typename T>
struct KtxTextureDeleter {
    void operator()(T* texture) const noexcept
    {
        if (texture != nullptr) {
            ktxTexture_Destroy(ktxTexture(texture));
        }
    }
};

void CheckKtxResult(KTX_error_code result, const std::string& action)
{
    if (result != KTX_SUCCESS) {
        throw std::runtime_error(action + ": " + ktxErrorString(result));
    }
}

void SetMipData(ktxTexture* destination, const Texture& source)
{
    const TextureInfo& info = source.info();
    for (std::size_t mipLevel = 0; mipLevel < info.mipLevels.size(); ++mipLevel) {
        const TextureMipLevel& mip = info.mipLevels[mipLevel];
        CheckKtxResult(
            ktxTexture_SetImageFromMemory(
                destination,
                static_cast<ktx_uint32_t>(mipLevel),
                0,
                0,
                source.data() + mip.byteOffset,
                mip.byteSize),
            "Unable to set KTX mip level");
    }
}

} // namespace

void TextureConverter::SaveAsKtx2(
    const Texture& texture,
    const std::filesystem::path& destination)
{
    const TextureInfo& info = texture.info();
    if (info.mipLevels.empty()) {
        throw std::invalid_argument("Cannot save a texture without mip levels.");
    }

    ktxTextureCreateInfo createInfo{};
    createInfo.vkFormat = ToVkFormat(info.format, info.colorSpace);
    createInfo.baseWidth = info.width;
    createInfo.baseHeight = info.height;
    createInfo.baseDepth = info.depth;
    createInfo.numDimensions =
        info.depth > 1 ? 3U : (info.height > 1 ? 2U : 1U);
    createInfo.numLevels =
        static_cast<ktx_uint32_t>(info.mipLevels.size());
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture2* rawTexture = nullptr;
    CheckKtxResult(
        ktxTexture2_Create(
            &createInfo,
            KTX_TEXTURE_CREATE_ALLOC_STORAGE,
            &rawTexture),
        "Unable to create KTX2 texture");
    std::unique_ptr<ktxTexture2, KtxTextureDeleter<ktxTexture2>>
        ktxTextureHandle(rawTexture);

    SetMipData(ktxTexture(ktxTextureHandle.get()), texture);

    const std::string destinationUtf8 = destination.u8string();
    CheckKtxResult(
        ktxTexture2_WriteToNamedFile(
            ktxTextureHandle.get(), destinationUtf8.c_str()),
        "Unable to write KTX2 file");
}

void TextureConverter::SaveAsKtx2(
    const TextureHandle& texture,
    const std::filesystem::path& destination)
{
    if (!texture) {
        throw std::invalid_argument("Cannot save an empty TextureHandle.");
    }
    SaveAsKtx2(*texture, destination);
}

void TextureConverter::SaveAsKtx(const Texture& texture,
                                 const std::filesystem::path& destination)
{
    const TextureInfo& info = texture.info();
    if (info.mipLevels.empty()) {
        throw std::invalid_argument("Cannot save a texture without mip levels.");
    }

    ktxTextureCreateInfo createInfo {};
    createInfo.glInternalformat =
        ToGlInternalFormat(info.format, info.colorSpace);
    createInfo.baseWidth = info.width;
    createInfo.baseHeight = info.height;
    createInfo.baseDepth = info.depth;
    createInfo.numDimensions = info.depth > 1 ? 3U : (info.height > 1 ? 2U : 1U);
    createInfo.numLevels = static_cast<ktx_uint32_t>(info.mipLevels.size());
    createInfo.numLayers = 1;
    createInfo.numFaces = 1;
    createInfo.isArray = KTX_FALSE;
    createInfo.generateMipmaps = KTX_FALSE;

    ktxTexture1* rawTexture = nullptr;
    CheckKtxResult(ktxTexture1_Create(&createInfo, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &rawTexture),
                   "Unable to create KTX texture");
    std::unique_ptr<ktxTexture1, KtxTextureDeleter<ktxTexture1>>
        ktxTextureHandle(rawTexture);

    SetMipData(ktxTexture(ktxTextureHandle.get()), texture);
    const std::string destinationUtf8 = destination.u8string();
    CheckKtxResult(ktxTexture_WriteToNamedFile(ktxTexture(ktxTextureHandle.get()),
                                                destinationUtf8.c_str()),
                   "Unable to write KTX file");
}

void TextureConverter::SaveAsKtx(const TextureHandle& texture,
                                 const std::filesystem::path& destination)
{
    if (!texture) {
        throw std::invalid_argument("Cannot save an empty TextureHandle.");
    }
    SaveAsKtx(*texture, destination);
}

} // namespace vpgloader::texture
