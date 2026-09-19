#include "AssimpMaterials.hpp"
#include "ModelUtils.hpp"
#include "TextureLoading.hpp"
#include "../PathUtils.hpp"

#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/scene.h>

#include <cmath>
#include <limits>
#include <utility>

namespace vpgloader::detail {
namespace {

std::filesystem::path ResolveTexturePath(const std::filesystem::path& modelPath,
                                         const aiString& importedPath)
{
    std::filesystem::path result = std::filesystem::u8path(importedPath.C_Str());
    if (result.is_relative()) {
        result = modelPath.parent_path() / result;
    }
    return ResolvePath(result);
}

std::string FormatHint(const aiTexture& texture)
{
    const char* begin = texture.achFormatHint;
    const char* end = begin;
    const char* maximum = begin + sizeof(texture.achFormatHint);
    while (end != maximum && *end != '\0') {
        ++end;
    }
    return LowerAscii(std::string(begin, end));
}

TextureHandle LoadEmbeddedTexture(const aiTexture& importedTexture,
                                  const std::string& assetName,
                                  bool isSrgb,
                                  const TextureLoadOptions& options)
{
    if (importedTexture.mHeight == 0) {
        std::string decodeName = assetName;
        const std::string hint = FormatHint(importedTexture);
        if (!hint.empty() && std::filesystem::path(decodeName).extension().empty()) {
            decodeName += "." + hint;
        }
        return TextureLoader::LoadFromMemory(
            decodeName,
            reinterpret_cast<const std::uint8_t*>(importedTexture.pcData),
            importedTexture.mWidth,
            options);
    }

    // aiTexel stores tightly packed BGRA8; pixel conversion and texture layout
    // belong to TextureLoader, independently of the model importer.
    static_assert(sizeof(aiTexel) == 4);
    TexturePixelView pixels;
    pixels.data = reinterpret_cast<const std::uint8_t*>(importedTexture.pcData);
    pixels.size = static_cast<std::size_t>(importedTexture.mWidth)
                  * importedTexture.mHeight * sizeof(aiTexel);
    pixels.width = importedTexture.mWidth;
    pixels.height = importedTexture.mHeight;
    pixels.layout = TexturePixelLayout::Bgra8;
    pixels.colorSpace = isSrgb ? TextureColorSpace::SRGB : TextureColorSpace::Linear;
    return TextureLoader::LoadFromPixels(assetName, pixels);
}

struct ImportedTextureSlot {
    std::uint32_t textureIndex = InvalidModelIndex;
    std::uint32_t texCoord = 0;
    glm::vec2 offset = glm::vec2(0.0f);
    glm::vec2 scale = glm::vec2(1.0f);
    float rotation = 0.0f;
    aiTextureType type = aiTextureType_NONE;

    bool hasTexture() const noexcept
    {
        return textureIndex != InvalidModelIndex;
    }
};

class MaterialImportSession {
public:
    MaterialImportSession(LoadedModel& model,
                          const std::filesystem::path& modelPath,
                          const aiScene& scene,
                          const ModelLoadOptions& options)
        : model_(model)
        , modelPath_(modelPath)
        , scene_(scene)
        , options_(options)
    {
    }

    ModelMaterial Import(const aiMaterial* material);
    void LoadTextures();

private:
    std::uint32_t EnsureTexture(const aiString& importedPath, bool isSrgb);
    ImportedTextureSlot ReadTextureSlot(const aiMaterial* material,
                                        aiTextureType type,
                                        bool isSrgb,
                                        unsigned int slotIndex = 0);
    std::uint16_t AppendTextureInfo(const ImportedTextureSlot& slot);

    LoadedModel& model_;
    const std::filesystem::path& modelPath_;
    const aiScene& scene_;
    const ModelLoadOptions& options_;
    std::vector<const aiTexture*> embeddedSources_;
};

std::uint32_t MaterialImportSession::EnsureTexture(const aiString& importedPath, bool isSrgb)
{
    if (importedPath.length == 0) {
        return InvalidModelIndex;
    }

    const auto embeddedResult = scene_.GetEmbeddedTextureAndIndex(importedPath.C_Str());
    const aiTexture* embeddedTexture = embeddedResult.first;
    const bool embedded = embeddedTexture != nullptr;
    const std::uint32_t embeddedIndex =
        embeddedResult.second >= 0
            ? static_cast<std::uint32_t>(embeddedResult.second)
            : InvalidModelIndex;
    const std::filesystem::path sourcePath =
        embedded ? std::filesystem::path() : ResolveTexturePath(modelPath_, importedPath);

    std::string name;
    if (embedded && embeddedTexture->mFilename.length > 0) {
        name = embeddedTexture->mFilename.C_Str();
    }
    if (name.empty()) {
        name = embedded ? std::string(importedPath.C_Str())
                        : sourcePath.filename().u8string();
    }

    for (std::size_t index = 0; index < model_.textures.size(); ++index) {
        const ModelTextureAsset& candidate = model_.textures[index];
        const bool sameSource =
            embedded ? (candidate.embedded && candidate.embeddedIndex == embeddedIndex)
                     : (!candidate.embedded && candidate.sourcePath == sourcePath);
        if (sameSource && candidate.isSrgb == isSrgb) {
            return CheckedIndex(index, "Texture count");
        }
    }

    const std::uint32_t textureIndex = CheckedIndex(model_.textures.size(), "Texture count");
    ModelTextureAsset asset;
    asset.name = std::move(name);
    asset.sourcePath = sourcePath;
    asset.isSrgb = isSrgb;
    asset.embedded = embedded;
    asset.embeddedIndex = embeddedIndex;
    model_.textures.push_back(std::move(asset));
    embeddedSources_.push_back(embeddedTexture);

    return textureIndex;
}

void MaterialImportSession::LoadTextures()
{
    if (!options_.loadTextures) {
        return;
    }
    std::vector<std::size_t> pending;
    for (std::size_t index = 0; index < model_.textures.size(); ++index) {
        if (!model_.textures[index].embedded || options_.loadEmbeddedTextures) {
            pending.push_back(index);
        }
    }
    LoadTextureBatch(model_, pending, options_, [&](std::size_t index) {
        const ModelTextureAsset& asset = model_.textures[index];
        return asset.embedded
            ? LoadEmbeddedTexture(*embeddedSources_[index], asset.name, asset.isSrgb,
                                  options_.textureLoadOptions)
            : LoadExternalTexture(asset.sourcePath, options_);
    });
}

ImportedTextureSlot MaterialImportSession::ReadTextureSlot(const aiMaterial* material,
                                                           aiTextureType type,
                                                           bool isSrgb,
                                                           unsigned int slotIndex)
{
    ImportedTextureSlot result;
    if (material->GetTextureCount(type) <= slotIndex) {
        return result;
    }

    aiString importedPath;
    unsigned int uvIndex = 0;
    if (material->GetTexture(type, slotIndex, &importedPath, nullptr, &uvIndex)
        != AI_SUCCESS) {
        return result;
    }

    result.textureIndex = EnsureTexture(importedPath, isSrgb);
    result.texCoord = uvIndex;
    result.type = type;

    aiUVTransform transform;
    if (material->Get(AI_MATKEY_UVTRANSFORM(type, slotIndex), transform) == AI_SUCCESS) {
        result.offset = {transform.mTranslation.x, transform.mTranslation.y};
        result.scale = {transform.mScaling.x, transform.mScaling.y};
        result.rotation = transform.mRotation;
    }
    return result;
}

std::uint16_t MaterialImportSession::AppendTextureInfo(const ImportedTextureSlot& slot)
{
    if (!slot.hasTexture()) {
        return InvalidMaterialTexture;
    }

    if (model_.textureInfos.empty()) {
        model_.textureInfos.emplace_back();
    }
    const std::uint32_t index = CheckedIndex(model_.textureInfos.size(), "Texture-info count");
    if (index > std::numeric_limits<std::uint16_t>::max()) {
        throw ModelLoadError(
            "A material texture-info index exceeds the 16-bit GltfShadeMaterial limit.");
    }
    ModelTextureInfo info;
    info.textureIndex = slot.textureIndex;
    info.texCoord = slot.texCoord;
    info.offset = slot.offset;
    info.scale = slot.scale;
    info.rotation = slot.rotation;
    model_.textureInfos.push_back(info);
    return static_cast<std::uint16_t>(index);
}

ModelMaterial MaterialImportSession::Import(const aiMaterial* material)
{
    ModelMaterial result;

    aiColor4D importedBaseColor;
    if (aiGetMaterialColor(material, AI_MATKEY_BASE_COLOR, &importedBaseColor) == AI_SUCCESS
        || aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &importedBaseColor)
               == AI_SUCCESS) {
        result.pbrBaseColorFactor = {
            importedBaseColor.r,
            importedBaseColor.g,
            importedBaseColor.b,
            importedBaseColor.a,
        };
    }

    aiColor4D emissive;
    if (aiGetMaterialColor(material, AI_MATKEY_COLOR_EMISSIVE, &emissive) == AI_SUCCESS) {
        result.emissiveFactor = {emissive.r, emissive.g, emissive.b};
    }

    aiGetMaterialFloat(material, AI_MATKEY_METALLIC_FACTOR, &result.pbrMetallicFactor);
    aiGetMaterialFloat(material, AI_MATKEY_ROUGHNESS_FACTOR, &result.pbrRoughnessFactor);

    float opacity = 1.0f;
    if (aiGetMaterialFloat(material, AI_MATKEY_OPACITY, &opacity) == AI_SUCCESS) {
        result.pbrBaseColorFactor.w = opacity;
    }

    int twoSided = 0;
    if (aiGetMaterialInteger(material, AI_MATKEY_TWOSIDED, &twoSided) == AI_SUCCESS) {
        result.doubleSided = twoSided != 0 ? 1 : 0;
    }

    int unlit = 0;
    if (material->Get("$mat.gltf.unlit", 0, 0, unlit) == AI_SUCCESS) {
        result.unlit = unlit != 0 ? 1 : 0;
    } else {
        int shadingMode = aiShadingMode_PBR_BRDF;
        if (aiGetMaterialInteger(material, AI_MATKEY_SHADING_MODEL, &shadingMode)
                == AI_SUCCESS
            && shadingMode == aiShadingMode_Unlit) {
            result.unlit = 1;
        }
    }

    aiGetMaterialFloat(material, AI_MATKEY_GLTF_ALPHACUTOFF, &result.alphaCutoff);
    aiString alphaMode;
    if (aiGetMaterialString(material, AI_MATKEY_GLTF_ALPHAMODE, &alphaMode) == AI_SUCCESS) {
        if (LowerAscii(alphaMode.C_Str()) == "mask") {
            result.alphaMode = static_cast<std::int32_t>(AlphaMode::Mask);
        } else if (LowerAscii(alphaMode.C_Str()) == "blend") {
            result.alphaMode = static_cast<std::int32_t>(AlphaMode::Blend);
        }
    } else if (result.pbrBaseColorFactor.w < 1.0f) {
        result.alphaMode = static_cast<std::int32_t>(AlphaMode::Blend);
    }

    aiGetMaterialFloat(material, AI_MATKEY_REFRACTI, &result.ior);
    aiGetMaterialFloat(material, AI_MATKEY_TRANSMISSION_FACTOR, &result.transmissionFactor);
    aiGetMaterialFloat(material, AI_MATKEY_VOLUME_THICKNESS_FACTOR, &result.thicknessFactor);
    aiGetMaterialFloat(
        material, AI_MATKEY_VOLUME_ATTENUATION_DISTANCE, &result.attenuationDistance);
    aiGetMaterialFloat(material, AI_MATKEY_CLEARCOAT_FACTOR, &result.clearcoatFactor);
    aiGetMaterialFloat(
        material, AI_MATKEY_CLEARCOAT_ROUGHNESS_FACTOR, &result.clearcoatRoughness);
    aiGetMaterialFloat(material, AI_MATKEY_SPECULAR_FACTOR, &result.specularFactor);
    aiGetMaterialFloat(
        material, AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, &result.sheenRoughnessFactor);
    aiGetMaterialFloat(material, AI_MATKEY_ANISOTROPY_FACTOR, &result.anisotropyStrength);

    float anisotropyAngle = 0.0f;
    if (aiGetMaterialFloat(material, AI_MATKEY_ANISOTROPY_ROTATION, &anisotropyAngle)
        == AI_SUCCESS) {
        result.anisotropyRotation = {std::cos(anisotropyAngle), std::sin(anisotropyAngle)};
    }

    aiColor4D color;
    if (aiGetMaterialColor(material, AI_MATKEY_VOLUME_ATTENUATION_COLOR, &color)
        == AI_SUCCESS) {
        result.attenuationColor = {color.r, color.g, color.b};
    }
    if (aiGetMaterialColor(material, AI_MATKEY_COLOR_SPECULAR, &color) == AI_SUCCESS) {
        result.specularColorFactor = {color.r, color.g, color.b};
        result.pbrSpecularFactor = result.specularColorFactor;
    }
    if (aiGetMaterialColor(material, AI_MATKEY_SHEEN_COLOR_FACTOR, &color)
        == AI_SUCCESS) {
        result.sheenColorFactor = {color.r, color.g, color.b};
    }

    if (aiGetMaterialFloat(material, AI_MATKEY_GLOSSINESS_FACTOR,
                           &result.pbrGlossinessFactor) == AI_SUCCESS) {
        result.usePbrSpecularGlossiness = 1;
        aiColor4D diffuse;
        if (aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &diffuse)
            == AI_SUCCESS) {
            result.pbrDiffuseFactor = {diffuse.r, diffuse.g, diffuse.b, diffuse.a};
        }
    }

    // These keys are used by Assimp builds that already expose the newer glTF
    // extensions even when their public material.h has not added named macros.
    material->Get("$mat.ior", 0, 0, result.ior);
    material->Get("$mat.iridescence.factor", 0, 0, result.iridescenceFactor);
    material->Get("$mat.iridescence.ior", 0, 0, result.iridescenceIor);
    material->Get("$mat.iridescence.thicknessMinimum", 0, 0,
                  result.iridescenceThicknessMinimum);
    material->Get("$mat.iridescence.thicknessMaximum", 0, 0,
                  result.iridescenceThicknessMaximum);
    material->Get("$mat.dispersion", 0, 0, result.dispersion);
    material->Get("$mat.diffuseTransmission.factor", 0, 0,
                  result.diffuseTransmissionFactor);
    material->Get("$mat.volume.scatterAnisotropy", 0, 0, result.scatterAnisotropy);

    if (material->Get("$clr.diffuseTransmission.factor", 0, 0, color) == AI_SUCCESS) {
        result.diffuseTransmissionColor = {color.r, color.g, color.b};
    }
    if (material->Get("$clr.volume.multiscatter", 0, 0, color) == AI_SUCCESS) {
        result.multiscatterColorFactor = {color.r, color.g, color.b};
    }


    ImportedTextureSlot baseColorSlot =
        ReadTextureSlot(material, aiTextureType_BASE_COLOR, options_.srgbBaseColorTextures);
    if (!baseColorSlot.hasTexture()) {
        baseColorSlot =
            ReadTextureSlot(material, aiTextureType_DIFFUSE, options_.srgbBaseColorTextures);
    }
    result.pbrBaseColorTexture = AppendTextureInfo(baseColorSlot);

    ImportedTextureSlot normal = ReadTextureSlot(material, aiTextureType_NORMALS, false);
    if (!normal.hasTexture()) {
        normal = ReadTextureSlot(material, aiTextureType_NORMAL_CAMERA, false);
    }
    result.normalTexture = AppendTextureInfo(normal);
    if (normal.hasTexture()) {
        material->Get(
            AI_MATKEY_GLTF_TEXTURE_SCALE(normal.type, 0),
            result.normalTextureScale);
    }

    ImportedTextureSlot metallicRoughness =
        ReadTextureSlot(material, aiTextureType_GLTF_METALLIC_ROUGHNESS, false);
    if (!metallicRoughness.hasTexture()) {
        metallicRoughness =
            ReadTextureSlot(material, aiTextureType_DIFFUSE_ROUGHNESS, false);
    }
    result.pbrMetallicRoughnessTexture = AppendTextureInfo(metallicRoughness);

    const ImportedTextureSlot emissiveTexture =
        ReadTextureSlot(material, aiTextureType_EMISSIVE, options_.srgbEmissiveTextures);
    result.emissiveTexture = AppendTextureInfo(emissiveTexture);

    const ImportedTextureSlot occlusion =
        ReadTextureSlot(material, aiTextureType_AMBIENT_OCCLUSION, false);
    result.occlusionTexture = AppendTextureInfo(occlusion);
    if (occlusion.hasTexture()) {
        material->Get(
            AI_MATKEY_GLTF_TEXTURE_STRENGTH(occlusion.type, 0),
            result.occlusionStrength);
    }

    result.transmissionTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_TRANSMISSION, false, 0));
    result.thicknessTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_TRANSMISSION, false, 1));
    result.clearcoatTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_CLEARCOAT, false, 0));
    result.clearcoatRoughnessTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_CLEARCOAT, false, 1));
    result.clearcoatNormalTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_CLEARCOAT, false, 2));
    result.specularTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_SPECULAR, false, 0));
    result.specularColorTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_SPECULAR, true, 1));
    result.anisotropyTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_ANISOTROPY, false));
    result.sheenColorTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_SHEEN, true, 0));
    result.sheenRoughnessTexture = AppendTextureInfo(
        ReadTextureSlot(material, aiTextureType_SHEEN, false, 1));

    if (result.usePbrSpecularGlossiness != 0) {
        result.pbrDiffuseTexture = AppendTextureInfo(
            ReadTextureSlot(material, aiTextureType_DIFFUSE, true));
        result.pbrSpecularGlossinessTexture = result.specularTexture;
    }
    return result;
}

} // namespace

void ImportMaterials(LoadedModel& model, const aiScene& scene,
                     const ModelLoadOptions& options)
{
    MaterialImportSession session(model, model.sourcePath, scene, options);
    if (options.loadMaterials) {
        model.materials.reserve(scene.mNumMaterials);
        for (unsigned int index = 0; index < scene.mNumMaterials; ++index) {
            model.materials.push_back(session.Import(scene.mMaterials[index]));
        }
    }
    if (model.materials.empty()) {
        model.materials.emplace_back();
    }
    session.LoadTextures();
}

} // namespace vpgloader::detail
