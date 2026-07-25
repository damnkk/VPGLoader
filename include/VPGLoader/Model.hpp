#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Texture.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace vpgloader {

inline constexpr std::uint32_t InvalidModelIndex =
    std::numeric_limits<std::uint32_t>::max();

struct Float2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Float4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// Column-major 4x4 matrix. Translation occupies values[12..14].
struct Matrix4 {
    std::array<float, 16> values = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
};

struct AABB {
    Float3 min;
    Float3 max;
    bool valid = false;
};

VPGLOADER_API Matrix4 Multiply(const Matrix4& left, const Matrix4& right) noexcept;
VPGLOADER_API Float3 TransformPoint(const Matrix4& transform, const Float3& point) noexcept;
VPGLOADER_API AABB TransformAABB(const AABB& bounds, const Matrix4& transform) noexcept;
VPGLOADER_API void ExpandAABB(AABB& bounds, const Float3& point) noexcept;
VPGLOADER_API void ExpandAABB(AABB& bounds, const AABB& other) noexcept;

struct ModelSubmeshAsset {
    std::uint32_t meshIndex = InvalidModelIndex;
    AABB bounds;
};

struct ModelNodeAsset {
    std::string name;
    std::uint32_t parent = InvalidModelIndex;
    std::uint32_t firstChild = InvalidModelIndex;
    std::uint32_t nextSibling = InvalidModelIndex;
    std::uint32_t transformIndex = InvalidModelIndex;
    Float3 translation;
    Quaternion rotation;
    Float3 scale = {1.0f, 1.0f, 1.0f};
    std::vector<std::uint32_t> submeshIndices;
};

// Range in the model-wide geometry arrays. Indices remain local to this
// mesh's vertex range, so an uploader can use firstVertex as its base vertex.
struct ModelMeshAsset {
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t materialIndex = 0;
    AABB bounds;
};

// All vertex streams have the same element count. Missing source attributes
// are replaced with stable defaults during import.
struct ModelGeometryData {
    std::vector<Float3> positions;
    std::vector<Float3> normals;
    std::vector<Float4> tangents;
    std::vector<Float2> texCoords0;
    std::vector<Float2> texCoords1;
    std::vector<std::uint32_t> colors;
    std::vector<std::uint32_t> indices;
};

struct ModelTextureAsset {
    std::string name;
    std::filesystem::path sourcePath;
    TextureHandle texture;
    bool isSrgb = true;
    bool embedded = false;
    std::uint32_t embeddedIndex = InvalidModelIndex;
    std::string loadError;
};

// A material points here rather than directly at a texture so that UV-set and
// transform metadata remain attached to each material texture use.
struct ModelTextureInfo {
    std::uint32_t textureIndex = InvalidModelIndex;
    std::uint32_t texCoord = 0;
    Float2 offset;
    Float2 scale = {1.0f, 1.0f};
    float rotation = 0.0f;

    bool hasTexture() const noexcept
    {
        return textureIndex != InvalidModelIndex;
    }
};

enum class AlphaMode : std::uint8_t {
    Opaque,
    Mask,
    Blend,
};

struct ModelMaterial {
    std::string name;
    Float4 baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
    Float3 emissiveFactor;
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;

    std::uint32_t baseColorTexture = InvalidModelIndex;
    std::uint32_t normalTexture = InvalidModelIndex;
    std::uint32_t metallicRoughnessTexture = InvalidModelIndex;
    std::uint32_t emissiveTexture = InvalidModelIndex;
    std::uint32_t occlusionTexture = InvalidModelIndex;
    std::uint32_t specularTexture = InvalidModelIndex;
};

struct ModelAsset {
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<ModelSubmeshAsset> submeshes;
    std::vector<ModelNodeAsset> nodes;
    std::vector<Matrix4> transforms;
    AABB bounds;
    std::uint32_t rootNode = InvalidModelIndex;
};

class LoadedModel;
using ModelHandle = std::shared_ptr<const LoadedModel>;

// Complete immutable-by-handle CPU product. It deliberately contains no GPU
// objects, upload state, renderer handles, or scene ownership.
class VPGLOADER_API LoadedModel final {
public:
    LoadedModel();
    ~LoadedModel();

    LoadedModel(const LoadedModel&) = delete;
    LoadedModel& operator=(const LoadedModel&) = delete;
    LoadedModel(LoadedModel&&) noexcept;
    LoadedModel& operator=(LoadedModel&&) noexcept;

    ModelAsset asset;
    ModelGeometryData geometry;
    std::vector<ModelMeshAsset> meshes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTextureInfo> textureInfos;
    std::vector<ModelTextureAsset> textures;
    std::vector<std::string> warnings;
};

} // namespace vpgloader
