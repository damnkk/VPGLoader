#include <VPGLoader/ModelLoader.hpp>

#include "ModelBinary.hpp"

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace vpgloader {
namespace {

std::string LowerAscii(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

bool EqualsIgnoreCase(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    while (*left != '\0' && *right != '\0') {
        char leftCharacter = *left++;
        char rightCharacter = *right++;
        if (leftCharacter >= 'A' && leftCharacter <= 'Z') {
            leftCharacter = static_cast<char>(leftCharacter - 'A' + 'a');
        }
        if (rightCharacter >= 'A' && rightCharacter <= 'Z') {
            rightCharacter = static_cast<char>(rightCharacter - 'A' + 'a');
        }
        if (leftCharacter != rightCharacter) {
            return false;
        }
    }
    return *left == '\0' && *right == '\0';
}

std::filesystem::path ResolvePath(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path result = std::filesystem::absolute(path, error);
    if (error) {
        return path.lexically_normal();
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(result, error);
    return error ? result.lexically_normal() : canonical;
}

std::filesystem::path ResolveTexturePath(const std::filesystem::path& modelPath,
                                         const aiString& importedPath)
{
    std::filesystem::path result = std::filesystem::u8path(importedPath.C_Str());
    if (result.is_relative()) {
        result = modelPath.parent_path() / result;
    }
    return ResolvePath(result);
}

std::uint32_t CheckedIndex(std::size_t value, const char* description)
{
    if (value >= static_cast<std::size_t>(InvalidModelIndex)) {
        throw ModelLoadError(std::string(description) + " exceeds the 32-bit model index range.");
    }
    return static_cast<std::uint32_t>(value);
}

std::string IndexedName(const char* prefix, std::uint32_t index)
{
    return std::string(prefix) + "_" + std::to_string(index);
}

std::string ImportedName(const aiString& name, const char* prefix, std::uint32_t index)
{
    return name.length > 0 ? std::string(name.C_Str()) : IndexedName(prefix, index);
}

Matrix4 ToMatrix4(const aiMatrix4x4& matrix) noexcept
{
    Matrix4 result;
    result.values = {
        matrix.a1, matrix.b1, matrix.c1, matrix.d1,
        matrix.a2, matrix.b2, matrix.c2, matrix.d2,
        matrix.a3, matrix.b3, matrix.c3, matrix.d3,
        matrix.a4, matrix.b4, matrix.c4, matrix.d4,
    };
    return result;
}

Float3 Cross(const Float3& left, const Float3& right) noexcept
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

float Dot(const Float3& left, const Float3& right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

std::uint32_t PackColor(const aiColor4D& color) noexcept
{
    const auto toByte = [](float value) {
        value = std::max(0.0f, std::min(1.0f, value));
        return static_cast<std::uint32_t>(value * 255.0f + 0.5f);
    };
    return toByte(color.r) | (toByte(color.g) << 8) | (toByte(color.b) << 16)
           | (toByte(color.a) << 24);
}

void ReserveGeometry(const aiScene& scene, ModelGeometryData& geometry)
{
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    for (unsigned int meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene.mMeshes[meshIndex];
        if (mesh == nullptr) {
            continue;
        }
        if (mesh->mNumVertices > std::numeric_limits<std::size_t>::max() - vertexCount) {
            throw ModelLoadError("Model vertex count exceeds addressable memory.");
        }
        vertexCount += mesh->mNumVertices;
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            if (mesh->mFaces[faceIndex].mNumIndices == 3) {
                if (indexCount > std::numeric_limits<std::size_t>::max() - 3) {
                    throw ModelLoadError("Model index count exceeds addressable memory.");
                }
                indexCount += 3;
            }
        }
    }

    geometry.positions.reserve(vertexCount);
    geometry.normals.reserve(vertexCount);
    geometry.tangents.reserve(vertexCount);
    geometry.texCoords0.reserve(vertexCount);
    geometry.texCoords1.reserve(vertexCount);
    geometry.colors.reserve(vertexCount);
    geometry.indices.reserve(indexCount);
}

std::uint32_t AppendMeshGeometry(const aiMesh& mesh,
                                 std::uint32_t materialIndex,
                                 LoadedModel& model)
{
    ModelGeometryData& geometry = model.geometry;
    ModelMeshAsset meshAsset;
    meshAsset.firstVertex = CheckedIndex(geometry.positions.size(), "First vertex");
    meshAsset.firstIndex = CheckedIndex(geometry.indices.size(), "First index");
    meshAsset.materialIndex = materialIndex;

    for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices; ++vertexIndex) {
        const aiVector3D importedPosition =
            mesh.HasPositions() ? mesh.mVertices[vertexIndex] : aiVector3D();
        const Float3 position = {
            importedPosition.x,
            importedPosition.y,
            importedPosition.z,
        };
        geometry.positions.push_back(position);
        ExpandAABB(meshAsset.bounds, position);

        Float3 normal = {0.0f, 1.0f, 0.0f};
        if (mesh.HasNormals()) {
            normal = {
                mesh.mNormals[vertexIndex].x,
                mesh.mNormals[vertexIndex].y,
                mesh.mNormals[vertexIndex].z,
            };
        }
        geometry.normals.push_back(normal);

        Float4 tangent = {1.0f, 0.0f, 0.0f, 1.0f};
        if (mesh.HasTangentsAndBitangents()) {
            const Float3 tangentDirection = {
                mesh.mTangents[vertexIndex].x,
                mesh.mTangents[vertexIndex].y,
                mesh.mTangents[vertexIndex].z,
            };
            const Float3 bitangent = {
                mesh.mBitangents[vertexIndex].x,
                mesh.mBitangents[vertexIndex].y,
                mesh.mBitangents[vertexIndex].z,
            };
            tangent = {
                tangentDirection.x,
                tangentDirection.y,
                tangentDirection.z,
                Dot(Cross(normal, tangentDirection), bitangent) < 0.0f ? -1.0f : 1.0f,
            };
        }
        geometry.tangents.push_back(tangent);

        if (mesh.HasTextureCoords(0)) {
            geometry.texCoords0.push_back({
                mesh.mTextureCoords[0][vertexIndex].x,
                mesh.mTextureCoords[0][vertexIndex].y,
            });
        } else {
            geometry.texCoords0.push_back({});
        }

        if (mesh.HasTextureCoords(1)) {
            geometry.texCoords1.push_back({
                mesh.mTextureCoords[1][vertexIndex].x,
                mesh.mTextureCoords[1][vertexIndex].y,
            });
        } else {
            geometry.texCoords1.push_back({});
        }

        geometry.colors.push_back(mesh.HasVertexColors(0)
                                      ? PackColor(mesh.mColors[0][vertexIndex])
                                      : 0xFFFFFFFFu);
    }

    bool skippedNonTriangle = false;
    for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex) {
        const aiFace& face = mesh.mFaces[faceIndex];
        if (face.mNumIndices != 3) {
            skippedNonTriangle = true;
            continue;
        }
        for (unsigned int corner = 0; corner < 3; ++corner) {
            if (face.mIndices[corner] >= mesh.mNumVertices) {
                throw ModelLoadError("Assimp returned a mesh index outside its vertex range.");
            }
            geometry.indices.push_back(face.mIndices[corner]);
        }
    }
    if (skippedNonTriangle) {
        model.warnings.push_back("A non-triangle primitive remained after Assimp processing and was skipped.");
    }

    meshAsset.vertexCount = mesh.mNumVertices;
    meshAsset.indexCount =
        CheckedIndex(geometry.indices.size() - meshAsset.firstIndex, "Mesh index count");
    const std::uint32_t meshIndex = CheckedIndex(model.meshes.size(), "Mesh count");
    model.meshes.push_back(std::move(meshAsset));
    return meshIndex;
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
    if (importedTexture.pcData == nullptr || importedTexture.mWidth == 0) {
        throw TextureLoadError("Embedded texture '" + assetName + "' is empty.");
    }

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

    if (options.outputComponentType != TextureComponentType::UInt8) {
        throw TextureLoadError(
            "Uncompressed embedded Assimp textures currently require UInt8 output.");
    }

    const std::size_t width = importedTexture.mWidth;
    const std::size_t height = importedTexture.mHeight;
    if (height > std::numeric_limits<std::size_t>::max() / width
        || width * height > std::numeric_limits<std::size_t>::max() / 4) {
        throw TextureLoadError("Embedded texture '" + assetName + "' is too large.");
    }

    const std::size_t pixelCount = width * height;
    Texture::ByteBuffer imageData(pixelCount * 4);
    for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
        const aiTexel& source = importedTexture.pcData[pixelIndex];
        imageData[pixelIndex * 4 + 0] = source.r;
        imageData[pixelIndex * 4 + 1] = source.g;
        imageData[pixelIndex * 4 + 2] = source.b;
        imageData[pixelIndex * 4 + 3] = source.a;
    }

    TextureInfo info;
    info.sourceFileName = assetName;
    info.sourceFileSize = imageData.size();
    info.width = importedTexture.mWidth;
    info.height = importedTexture.mHeight;
    info.depth = 1;
    info.format = {TextureComponentType::UInt8, 4};
    info.colorSpace = isSrgb ? TextureColorSpace::SRGB : TextureColorSpace::Linear;
    info.mipLevels.push_back({
        importedTexture.mWidth,
        importedTexture.mHeight,
        1,
        0,
        imageData.size(),
    });
    return Texture::Create(std::move(info), std::move(imageData));
}

struct ImportedTextureSlot {
    std::uint32_t textureIndex = InvalidModelIndex;
    std::uint32_t texCoord = 0;
    Float2 offset;
    Float2 scale = {1.0f, 1.0f};
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

    ModelMaterial Import(const aiMaterial* material, std::uint32_t materialIndex);
    void LoadTextures();

private:
    std::uint32_t EnsureTexture(const aiString& importedPath, bool isSrgb);
    ImportedTextureSlot ReadTextureSlot(const aiMaterial* material,
                                        aiTextureType type,
                                        bool isSrgb);
    std::uint32_t AppendTextureInfo(const ImportedTextureSlot& slot);

    LoadedModel& model_;
    const std::filesystem::path& modelPath_;
    const aiScene& scene_;
    const ModelLoadOptions& options_;
    std::vector<const aiTexture*> embeddedSources_;
};

std::uint32_t MaterialImportSession::EnsureTexture(const aiString& importedPath, bool isSrgb)
{
    if (!options_.loadTextures || importedPath.length == 0) {
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
    struct PendingTexture {
        std::size_t textureIndex = 0;
        const aiTexture* embeddedSource = nullptr;
    };
    struct TextureLoadResult {
        TextureHandle texture;
        std::string error;
    };

    std::vector<PendingTexture> pendingTextures;
    pendingTextures.reserve(model_.textures.size());
    for (std::size_t textureIndex = 0; textureIndex < model_.textures.size(); ++textureIndex) {
        const ModelTextureAsset& asset = model_.textures[textureIndex];
        if (asset.embedded && !options_.loadEmbeddedTextures) {
            continue;
        }
        pendingTextures.push_back({textureIndex, embeddedSources_[textureIndex]});
    }
    if (pendingTextures.empty()) {
        return;
    }

    std::vector<TextureLoadResult> results(pendingTextures.size());
    std::atomic<std::size_t> nextTexture{0};
    const auto worker = [&]() {
        for (;;) {
            const std::size_t pendingIndex =
                nextTexture.fetch_add(1, std::memory_order_relaxed);
            if (pendingIndex >= pendingTextures.size()) {
                return;
            }

            const PendingTexture& pending = pendingTextures[pendingIndex];
            const ModelTextureAsset& asset = model_.textures[pending.textureIndex];
            TextureLoadResult& result = results[pendingIndex];
            try {
                if (asset.embedded) {
                    if (pending.embeddedSource == nullptr) {
                        throw TextureLoadError(
                            "Assimp did not provide the registered embedded texture data.");
                    }
                    result.texture = LoadEmbeddedTexture(
                        *pending.embeddedSource,
                        asset.name,
                        asset.isSrgb,
                        options_.textureLoadOptions);
                } else if (options_.cacheExternalTextures) {
                    result.texture =
                        TextureLoader::LoadCached(asset.sourcePath, options_.textureLoadOptions);
                } else {
                    result.texture =
                        TextureLoader::Load(asset.sourcePath, options_.textureLoadOptions);
                }
            } catch (const std::exception& error) {
                result.error =
                    "Failed to load model texture '" + asset.name + "': " + error.what();
            } catch (...) {
                result.error =
                    "Failed to load model texture '" + asset.name + "': unknown error";
            }
        }
    };

    std::size_t requestedWorkers = options_.maxTextureLoadConcurrency;
    if (requestedWorkers == 0) {
        requestedWorkers = std::thread::hardware_concurrency();
        if (requestedWorkers == 0) {
            requestedWorkers = 1;
        }
    }
    const std::size_t workerCount =
        std::min(requestedWorkers, pendingTextures.size());

    std::vector<std::thread> workers;
    workers.reserve(workerCount > 0 ? workerCount - 1 : 0);
    try {
        for (std::size_t workerIndex = 1; workerIndex < workerCount; ++workerIndex) {
            workers.emplace_back(worker);
        }
        worker();
    } catch (...) {
        for (std::thread& thread : workers) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        throw;
    }
    for (std::thread& thread : workers) {
        thread.join();
    }

    std::string firstFailure;
    for (std::size_t pendingIndex = 0; pendingIndex < pendingTextures.size(); ++pendingIndex) {
        ModelTextureAsset& asset =
            model_.textures[pendingTextures[pendingIndex].textureIndex];
        TextureLoadResult& result = results[pendingIndex];
        asset.texture = std::move(result.texture);
        if (!result.error.empty()) {
            asset.loadError = std::move(result.error);
            model_.warnings.push_back(asset.loadError);
            if (firstFailure.empty()) {
                firstFailure = asset.loadError;
            }
        }
    }

    if (options_.failOnMissingTextures && !firstFailure.empty()) {
        throw ModelLoadError(firstFailure);
    }
}

ImportedTextureSlot MaterialImportSession::ReadTextureSlot(const aiMaterial* material,
                                                           aiTextureType type,
                                                           bool isSrgb)
{
    ImportedTextureSlot result;
    if (material == nullptr || material->GetTextureCount(type) == 0) {
        return result;
    }

    aiString importedPath;
    unsigned int uvIndex = 0;
    if (material->GetTexture(type, 0, &importedPath, nullptr, &uvIndex) != AI_SUCCESS) {
        return result;
    }

    result.textureIndex = EnsureTexture(importedPath, isSrgb);
    result.texCoord = uvIndex;
    result.type = type;

    aiUVTransform transform;
    if (material->Get(AI_MATKEY_UVTRANSFORM(type, 0), transform) == AI_SUCCESS) {
        result.offset = {transform.mTranslation.x, transform.mTranslation.y};
        result.scale = {transform.mScaling.x, transform.mScaling.y};
        result.rotation = transform.mRotation;
    }
    return result;
}

std::uint32_t MaterialImportSession::AppendTextureInfo(const ImportedTextureSlot& slot)
{
    if (!slot.hasTexture()) {
        return InvalidModelIndex;
    }

    const std::uint32_t index = CheckedIndex(model_.textureInfos.size(), "Texture-info count");
    ModelTextureInfo info;
    info.textureIndex = slot.textureIndex;
    info.texCoord = slot.texCoord;
    info.offset = slot.offset;
    info.scale = slot.scale;
    info.rotation = slot.rotation;
    model_.textureInfos.push_back(info);
    return index;
}

ModelMaterial MaterialImportSession::Import(const aiMaterial* material,
                                            std::uint32_t materialIndex)
{
    ModelMaterial result;
    result.name = IndexedName("Material", materialIndex);
    if (material == nullptr) {
        return result;
    }

    aiString materialName;
    if (aiGetMaterialString(material, AI_MATKEY_NAME, &materialName) == AI_SUCCESS
        && materialName.length > 0) {
        result.name = materialName.C_Str();
    }

    aiColor4D importedBaseColor;
    if (aiGetMaterialColor(material, AI_MATKEY_BASE_COLOR, &importedBaseColor) == AI_SUCCESS
        || aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &importedBaseColor)
               == AI_SUCCESS) {
        result.baseColorFactor = {
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

    aiGetMaterialFloat(material, AI_MATKEY_METALLIC_FACTOR, &result.metallicFactor);
    aiGetMaterialFloat(material, AI_MATKEY_ROUGHNESS_FACTOR, &result.roughnessFactor);

    float opacity = 1.0f;
    if (aiGetMaterialFloat(material, AI_MATKEY_OPACITY, &opacity) == AI_SUCCESS) {
        result.baseColorFactor.w = opacity;
    }

    int twoSided = 0;
    if (aiGetMaterialInteger(material, AI_MATKEY_TWOSIDED, &twoSided) == AI_SUCCESS) {
        result.doubleSided = twoSided != 0;
    }

    int unlit = 0;
    if (material->Get("$mat.gltf.unlit", 0, 0, unlit) == AI_SUCCESS) {
        result.unlit = unlit != 0;
    }

    aiGetMaterialFloat(material, AI_MATKEY_GLTF_ALPHACUTOFF, &result.alphaCutoff);
    aiString alphaMode;
    if (aiGetMaterialString(material, AI_MATKEY_GLTF_ALPHAMODE, &alphaMode) == AI_SUCCESS) {
        if (EqualsIgnoreCase(alphaMode.C_Str(), "MASK")) {
            result.alphaMode = AlphaMode::Mask;
        } else if (EqualsIgnoreCase(alphaMode.C_Str(), "BLEND")) {
            result.alphaMode = AlphaMode::Blend;
        }
    } else if (result.baseColorFactor.w < 1.0f) {
        result.alphaMode = AlphaMode::Blend;
    }

    if (!options_.loadTextures) {
        return result;
    }

    ImportedTextureSlot baseColorSlot =
        ReadTextureSlot(material, aiTextureType_BASE_COLOR, options_.srgbBaseColorTextures);
    if (!baseColorSlot.hasTexture()) {
        baseColorSlot =
            ReadTextureSlot(material, aiTextureType_DIFFUSE, options_.srgbBaseColorTextures);
    }
    result.baseColorTexture = AppendTextureInfo(baseColorSlot);

    ImportedTextureSlot normal = ReadTextureSlot(material, aiTextureType_NORMALS, false);
    if (!normal.hasTexture()) {
        normal = ReadTextureSlot(material, aiTextureType_NORMAL_CAMERA, false);
    }
    result.normalTexture = AppendTextureInfo(normal);
    if (normal.hasTexture()) {
        material->Get(
            AI_MATKEY_GLTF_TEXTURE_SCALE(normal.type, 0),
            result.normalScale);
    }

    ImportedTextureSlot metallicRoughness =
        ReadTextureSlot(material, aiTextureType_GLTF_METALLIC_ROUGHNESS, false);
    if (!metallicRoughness.hasTexture()) {
        metallicRoughness =
            ReadTextureSlot(material, aiTextureType_DIFFUSE_ROUGHNESS, false);
    }
    result.metallicRoughnessTexture = AppendTextureInfo(metallicRoughness);

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

    const ImportedTextureSlot specular =
        ReadTextureSlot(material, aiTextureType_SPECULAR, false);
    result.specularTexture = AppendTextureInfo(specular);
    return result;
}

struct NodeImportContext {
    LoadedModel& model;
    const std::vector<std::uint32_t>& meshToSubmesh;
};

std::uint32_t AppendNode(const aiNode* importedNode,
                         std::uint32_t parentIndex,
                         NodeImportContext& context)
{
    if (importedNode == nullptr) {
        return InvalidModelIndex;
    }

    ModelAsset& asset = context.model.asset;
    const std::uint32_t nodeIndex = CheckedIndex(asset.nodes.size(), "Node count");

    aiVector3D scale;
    aiQuaternion rotation;
    aiVector3D translation;
    importedNode->mTransformation.Decompose(scale, rotation, translation);

    ModelNodeAsset node;
    node.name = ImportedName(importedNode->mName, "Node", nodeIndex);
    node.parent = parentIndex;
    node.transformIndex = CheckedIndex(asset.transforms.size(), "Transform count");
    node.translation = {translation.x, translation.y, translation.z};
    node.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
    node.scale = {scale.x, scale.y, scale.z};
    asset.transforms.push_back(ToMatrix4(importedNode->mTransformation));
    asset.nodes.push_back(std::move(node));

    for (unsigned int meshSlot = 0; meshSlot < importedNode->mNumMeshes; ++meshSlot) {
        const unsigned int importedMeshIndex = importedNode->mMeshes[meshSlot];
        if (importedMeshIndex < context.meshToSubmesh.size()
            && context.meshToSubmesh[importedMeshIndex] != InvalidModelIndex) {
            asset.nodes[nodeIndex].submeshIndices.push_back(
                context.meshToSubmesh[importedMeshIndex]);
        }
    }

    std::uint32_t previousChild = InvalidModelIndex;
    for (unsigned int childSlot = 0; childSlot < importedNode->mNumChildren; ++childSlot) {
        const std::uint32_t childIndex =
            AppendNode(importedNode->mChildren[childSlot], nodeIndex, context);
        if (childIndex == InvalidModelIndex) {
            continue;
        }
        if (previousChild == InvalidModelIndex) {
            asset.nodes[nodeIndex].firstChild = childIndex;
        } else {
            asset.nodes[previousChild].nextSibling = childIndex;
        }
        previousChild = childIndex;
    }
    return nodeIndex;
}

void AccumulateModelBounds(LoadedModel& model,
                           std::uint32_t nodeIndex,
                           const Matrix4& parentToModel)
{
    if (nodeIndex >= model.asset.nodes.size()) {
        return;
    }

    const ModelNodeAsset& node = model.asset.nodes[nodeIndex];
    const Matrix4 localTransform =
        node.transformIndex < model.asset.transforms.size()
            ? model.asset.transforms[node.transformIndex]
            : Matrix4();
    const Matrix4 localToModel = Multiply(parentToModel, localTransform);

    for (const std::uint32_t submeshIndex : node.submeshIndices) {
        if (submeshIndex >= model.asset.submeshes.size()) {
            continue;
        }
        ExpandAABB(
            model.asset.bounds,
            TransformAABB(model.asset.submeshes[submeshIndex].bounds, localToModel));
    }

    std::uint32_t childIndex = node.firstChild;
    while (childIndex != InvalidModelIndex && childIndex < model.asset.nodes.size()) {
        const std::uint32_t nextSibling = model.asset.nodes[childIndex].nextSibling;
        AccumulateModelBounds(model, childIndex, localToModel);
        childIndex = nextSibling;
    }
}

bool IsFbxImport(const std::filesystem::path& path, ModelFileFormat format)
{
    if (format == ModelFileFormat::Fbx) {
        return true;
    }
    if (format != ModelFileFormat::Auto) {
        return false;
    }
    return LowerAscii(path.extension().u8string()) == ".fbx";
}

void ConfigureImporter(Assimp::Importer& importer,
                       const std::filesystem::path& path,
                       const ModelLoadOptions& options)
{
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, options.globalScale);
    importer.SetPropertyInteger(
        AI_CONFIG_PP_SBP_REMOVE,
        aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    if (IsFbxImport(path, options.format)) {
        importer.SetPropertyInteger(
            AI_CONFIG_IMPORT_FBX_READ_MATERIALS,
            options.loadMaterials ? 1 : 0);
        importer.SetPropertyInteger(
            AI_CONFIG_IMPORT_FBX_READ_TEXTURES,
            options.loadTextures && options.loadEmbeddedTextures ? 1 : 0);
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, 0);
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, 0);
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, 0);
        importer.SetPropertyInteger(AI_CONFIG_IMPORT_FBX_READ_WEIGHTS, 0);
    }
}

} // namespace

ModelLoadError::ModelLoadError(const std::string& message)
    : std::runtime_error(message)
{
}

std::uint32_t ModelLoader::DefaultAssimpPostProcessFlags() noexcept
{
    return static_cast<std::uint32_t>(
        aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace
        | aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality
        | aiProcess_SortByPType | aiProcess_FindInvalidData | aiProcess_GenBoundingBoxes
        | aiProcess_FlipUVs);
}

ModelHandle ModelLoader::Load(const std::filesystem::path& path,
                              const ModelLoadOptions& options)
{
    if (path.empty()) {
        throw ModelLoadError("Model path is empty.");
    }
    if (!std::isfinite(options.globalScale) || options.globalScale <= 0.0f) {
        throw ModelLoadError("Model globalScale must be finite and greater than zero.");
    }
    if (options.format == ModelFileFormat::VpgModel
        || (options.format == ModelFileFormat::Auto
            && detail::IsVpgModelPath(path))) {
        return detail::LoadVpgModel(path, options);
    }

    const std::filesystem::path sourcePath = ResolvePath(path);
    Assimp::Importer importer;
    ConfigureImporter(importer, sourcePath, options);

    std::uint32_t postProcessFlags = options.assimpPostProcessFlags == 0
                                         ? DefaultAssimpPostProcessFlags()
                                         : options.assimpPostProcessFlags;
    postProcessFlags |= options.extraAssimpPostProcessFlags;
    if (options.globalScale != 1.0f) {
        postProcessFlags |= aiProcess_GlobalScale;
    }

    const aiScene* importedScene = importer.ReadFile(sourcePath.u8string(), postProcessFlags);
    if (importedScene == nullptr || importedScene->mRootNode == nullptr) {
        const std::string detail = importer.GetErrorString();
        throw ModelLoadError(
            "Failed to load model '" + sourcePath.u8string() + "': "
            + (detail.empty() ? std::string("Assimp returned no scene.") : detail));
    }

    std::shared_ptr<LoadedModel> model = std::make_shared<LoadedModel>();
    model->asset.name = sourcePath.stem().u8string();
    model->asset.sourcePath = sourcePath;
    if ((importedScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        model->warnings.push_back("Assimp marked the imported scene as incomplete.");
    }

    MaterialImportSession materialSession(*model, sourcePath, *importedScene, options);
    if (options.loadMaterials) {
        model->materials.reserve(importedScene->mNumMaterials);
        for (unsigned int materialIndex = 0; materialIndex < importedScene->mNumMaterials;
             ++materialIndex) {
            model->materials.push_back(materialSession.Import(
                importedScene->mMaterials[materialIndex],
                materialIndex));
        }
    }
    if (model->materials.empty()) {
        ModelMaterial defaultMaterial;
        defaultMaterial.name = "DefaultMaterial";
        model->materials.push_back(std::move(defaultMaterial));
    }

    ReserveGeometry(*importedScene, model->geometry);
    model->meshes.reserve(importedScene->mNumMeshes);
    model->asset.submeshes.reserve(importedScene->mNumMeshes);
    std::vector<std::uint32_t> meshToSubmesh;
    meshToSubmesh.reserve(importedScene->mNumMeshes);

    for (unsigned int importedMeshIndex = 0;
         importedMeshIndex < importedScene->mNumMeshes;
         ++importedMeshIndex) {
        const aiMesh* importedMesh = importedScene->mMeshes[importedMeshIndex];
        if (importedMesh == nullptr) {
            meshToSubmesh.push_back(InvalidModelIndex);
            continue;
        }

        const std::uint32_t materialIndex =
            importedMesh->mMaterialIndex < model->materials.size()
                ? importedMesh->mMaterialIndex
                : 0;
        const std::uint32_t meshIndex =
            AppendMeshGeometry(*importedMesh, materialIndex, *model);

        ModelSubmeshAsset submesh;
        submesh.meshIndex = meshIndex;
        submesh.bounds = model->meshes[meshIndex].bounds;
        const std::uint32_t submeshIndex =
            CheckedIndex(model->asset.submeshes.size(), "Submesh count");
        model->asset.submeshes.push_back(std::move(submesh));
        meshToSubmesh.push_back(submeshIndex);
    }

    model->asset.nodes.reserve(importedScene->mNumMeshes + 1);
    model->asset.transforms.reserve(importedScene->mNumMeshes + 1);
    NodeImportContext nodeContext{*model, meshToSubmesh};
    model->asset.rootNode =
        AppendNode(importedScene->mRootNode, InvalidModelIndex, nodeContext);
    if (model->asset.rootNode == InvalidModelIndex) {
        throw ModelLoadError("Model node hierarchy import failed.");
    }

    AccumulateModelBounds(*model, model->asset.rootNode, Matrix4());
    materialSession.LoadTextures();
    return model;
}

} // namespace vpgloader
