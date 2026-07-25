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

ktx_uint32_t ToGlInternalFormat(const TextureFormat& format)
{
    if (format.componentType != TextureComponentType::UInt8) {
        throw std::invalid_argument("SaveAsKtx currently supports UInt8 textures only.");
    }

    switch (format.channels) {
    case 1:
        return GlR8;
    case 2:
        return GlRg8;
    case 3:
        return GlRgb8;
    case 4:
        return GlRgba8;
    default:
        throw std::invalid_argument("SaveAsKtx requires between one and four channels.");
    }
}

struct KtxTextureDeleter {
    void operator()(ktxTexture1* texture) const noexcept
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

} // namespace

void TextureConverter::SaveAsKtx(const Texture& texture,
                                 const std::filesystem::path& destination)
{
    const TextureInfo& info = texture.info();
    if (info.mipLevels.empty()) {
        throw std::invalid_argument("Cannot save a texture without mip levels.");
    }

    ktxTextureCreateInfo createInfo {};
    createInfo.glInternalformat = ToGlInternalFormat(info.format);
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
    std::unique_ptr<ktxTexture1, KtxTextureDeleter> ktxTextureHandle(rawTexture);

    for (std::size_t mipLevel = 0; mipLevel < info.mipLevels.size(); ++mipLevel) {
        const TextureMipLevel& mip = info.mipLevels[mipLevel];
        CheckKtxResult(ktxTexture_SetImageFromMemory(
                           ktxTexture(ktxTextureHandle.get()), static_cast<ktx_uint32_t>(mipLevel), 0, 0,
                           texture.data() + mip.byteOffset, mip.byteSize),
                       "Unable to set KTX mip level");
    }

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
