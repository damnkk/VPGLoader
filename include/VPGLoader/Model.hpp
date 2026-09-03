#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Texture.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace vpgloader {

inline constexpr std::uint32_t InvalidModelIndex =
    std::numeric_limits<std::uint32_t>::max();

struct AABB {
    glm::vec3 min = glm::vec3(0.0f);
    glm::vec3 max = glm::vec3(0.0f);
    bool valid = false;
};

VPGLOADER_API glm::mat4 Multiply(const glm::mat4& left,
                                 const glm::mat4& right) noexcept;
VPGLOADER_API glm::vec3 TransformPoint(const glm::mat4& transform,
                                        const glm::vec3& point) noexcept;
VPGLOADER_API AABB TransformAABB(const AABB& bounds,
                                 const glm::mat4& transform) noexcept;
VPGLOADER_API void ExpandAABB(AABB& bounds, const glm::vec3& point) noexcept;
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
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
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
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
    std::vector<glm::vec2> texCoords0;
    std::vector<glm::vec2> texCoords1;
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
    glm::vec2 offset = glm::vec2(0.0f);
    glm::vec2 scale = glm::vec2(1.0f);
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

// Texture-info slot zero is reserved as the GPU-visible "no texture" value.
inline constexpr std::uint16_t InvalidMaterialTexture = 0;

// GPU-ready glTF shading material. Keep this layout synchronized with the
// shader-side GltfShadeMaterial definition.
struct ModelMaterial {
    glm::vec4 pbrBaseColorFactor = glm::vec4(1.0f);
    glm::vec3 emissiveFactor = glm::vec3(0.0f);
    float normalTextureScale = 1.0f;

    float pbrRoughnessFactor = 1.0f;
    float pbrMetallicFactor = 1.0f;
    std::int32_t alphaMode = static_cast<std::int32_t>(AlphaMode::Opaque);
    float alphaCutoff = 0.5f;

    glm::vec3 attenuationColor = glm::vec3(1.0f);
    float ior = 1.5f;

    float transmissionFactor = 0.0f;
    float thicknessFactor = 0.0f;
    float attenuationDistance = std::numeric_limits<float>::infinity();
    float clearcoatFactor = 0.0f;

    glm::vec3 specularColorFactor = glm::vec3(1.0f);
    float clearcoatRoughness = 0.0f;

    float specularFactor = 1.0f;
    std::int32_t unlit = 0;
    float iridescenceFactor = 0.0f;
    float iridescenceThicknessMaximum = 400.0f;

    float iridescenceThicknessMinimum = 100.0f;
    float iridescenceIor = 1.3f;
    glm::vec2 anisotropyRotation = glm::vec2(1.0f, 0.0f);

    glm::vec3 sheenColorFactor = glm::vec3(0.0f);
    float anisotropyStrength = 0.0f;

    float sheenRoughnessFactor = 0.0f;
    float occlusionStrength = 1.0f;
    float dispersion = 0.0f;
    std::int32_t usePbrSpecularGlossiness = 0;

    glm::vec4 pbrDiffuseFactor = glm::vec4(1.0f);
    glm::vec3 pbrSpecularFactor = glm::vec3(1.0f);
    float pbrGlossinessFactor = 1.0f;

    glm::vec3 diffuseTransmissionColor = glm::vec3(1.0f);
    float diffuseTransmissionFactor = 0.0f;

    glm::vec3 multiscatterColorFactor = glm::vec3(0.0f);
    float scatterAnisotropy = 0.0f;

    std::int32_t doubleSided = 0;
    std::uint16_t pbrBaseColorTexture = InvalidMaterialTexture;
    std::uint16_t normalTexture = InvalidMaterialTexture;
    std::uint16_t pbrMetallicRoughnessTexture = InvalidMaterialTexture;
    std::uint16_t emissiveTexture = InvalidMaterialTexture;
    std::uint16_t transmissionTexture = InvalidMaterialTexture;
    std::uint16_t thicknessTexture = InvalidMaterialTexture;
    std::uint16_t clearcoatTexture = InvalidMaterialTexture;
    std::uint16_t clearcoatRoughnessTexture = InvalidMaterialTexture;
    std::uint16_t clearcoatNormalTexture = InvalidMaterialTexture;
    std::uint16_t specularTexture = InvalidMaterialTexture;
    std::uint16_t specularColorTexture = InvalidMaterialTexture;
    std::uint16_t iridescenceTexture = InvalidMaterialTexture;
    std::uint16_t iridescenceThicknessTexture = InvalidMaterialTexture;
    std::uint16_t anisotropyTexture = InvalidMaterialTexture;
    std::uint16_t sheenColorTexture = InvalidMaterialTexture;
    std::uint16_t sheenRoughnessTexture = InvalidMaterialTexture;
    std::uint16_t occlusionTexture = InvalidMaterialTexture;
    std::uint16_t pbrDiffuseTexture = InvalidMaterialTexture;
    std::uint16_t pbrSpecularGlossinessTexture = InvalidMaterialTexture;
    std::uint16_t diffuseTransmissionTexture = InvalidMaterialTexture;
    std::uint16_t diffuseTransmissionColorTexture = InvalidMaterialTexture;
    std::uint16_t _pad1 = 0;
};

static_assert(sizeof(ModelMaterial) == 272);
static_assert(offsetof(ModelMaterial, emissiveFactor) == 16);
static_assert(offsetof(ModelMaterial, pbrRoughnessFactor) == 32);
static_assert(offsetof(ModelMaterial, attenuationColor) == 48);
static_assert(offsetof(ModelMaterial, transmissionFactor) == 64);
static_assert(offsetof(ModelMaterial, specularColorFactor) == 80);
static_assert(offsetof(ModelMaterial, specularFactor) == 96);
static_assert(offsetof(ModelMaterial, iridescenceThicknessMinimum) == 112);
static_assert(offsetof(ModelMaterial, sheenColorFactor) == 128);
static_assert(offsetof(ModelMaterial, sheenRoughnessFactor) == 144);
static_assert(offsetof(ModelMaterial, pbrDiffuseFactor) == 160);
static_assert(offsetof(ModelMaterial, pbrSpecularFactor) == 176);
static_assert(offsetof(ModelMaterial, diffuseTransmissionColor) == 192);
static_assert(offsetof(ModelMaterial, multiscatterColorFactor) == 208);
static_assert(offsetof(ModelMaterial, doubleSided) == 224);
static_assert(offsetof(ModelMaterial, pbrBaseColorTexture) == 228);
static_assert(offsetof(ModelMaterial, _pad1) == 270);

struct ModelAsset {
    std::string name;
    std::filesystem::path sourcePath;
    std::vector<ModelSubmeshAsset> submeshes;
    std::vector<ModelNodeAsset> nodes;
    std::vector<glm::mat4> transforms;
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
