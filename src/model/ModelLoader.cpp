#include <VPGLoader/ModelLoader.hpp>

#include "ModelBinary.hpp"
#include "AssimpMaterials.hpp"
#include "ModelUtils.hpp"
#include "../PathUtils.hpp"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vpgloader {
namespace {

using detail::CheckedIndex;
using detail::LowerAscii;
using detail::ResolvePath;

glm::mat4 ToMatrix4(const aiMatrix4x4& matrix) noexcept
{
    return glm::mat4(
        matrix.a1, matrix.b1, matrix.c1, matrix.d1,
        matrix.a2, matrix.b2, matrix.c2, matrix.d2,
        matrix.a3, matrix.b3, matrix.c3, matrix.d3,
        matrix.a4, matrix.b4, matrix.c4, matrix.d4);
}

std::uint32_t PackColor(const aiColor4D& color) noexcept
{
    const auto toByte = [](float value) {
        value = std::clamp(value, 0.0f, 1.0f);
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
        vertexCount += mesh->mNumVertices;
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            if (mesh->mFaces[faceIndex].mNumIndices == 3) {
                indexCount += 3;
            }
        }
    }

    CheckedIndex(vertexCount, "Vertex count");
    CheckedIndex(indexCount, "Index count");
    geometry.positions.reserve(vertexCount);
    geometry.normals.reserve(vertexCount);
    geometry.tangents.reserve(vertexCount);
    geometry.texCoords0.reserve(vertexCount);
    geometry.texCoords1.reserve(vertexCount);
    geometry.colors.reserve(vertexCount);
    geometry.indices.reserve(indexCount);
}

void AppendMeshGeometry(const aiMesh& mesh,
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
            mesh.mVertices[vertexIndex];
        const glm::vec3 position = {
            importedPosition.x,
            importedPosition.y,
            importedPosition.z,
        };
        geometry.positions.push_back(position);
        ExpandAABB(meshAsset.bounds, position);

        glm::vec3 normal = {0.0f, 1.0f, 0.0f};
        if (mesh.HasNormals()) {
            normal = {
                mesh.mNormals[vertexIndex].x,
                mesh.mNormals[vertexIndex].y,
                mesh.mNormals[vertexIndex].z,
            };
        }
        geometry.normals.push_back(normal);

        glm::vec4 tangent = {1.0f, 0.0f, 0.0f, 1.0f};
        if (mesh.HasTangentsAndBitangents()) {
            const glm::vec3 tangentDirection = {
                mesh.mTangents[vertexIndex].x,
                mesh.mTangents[vertexIndex].y,
                mesh.mTangents[vertexIndex].z,
            };
            const glm::vec3 bitangent = {
                mesh.mBitangents[vertexIndex].x,
                mesh.mBitangents[vertexIndex].y,
                mesh.mBitangents[vertexIndex].z,
            };
            tangent = {
                tangentDirection.x,
                tangentDirection.y,
                tangentDirection.z,
                glm::dot(glm::cross(normal, tangentDirection), bitangent) < 0.0f ? -1.0f : 1.0f,
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
    model.meshes.push_back(std::move(meshAsset));
}

void AppendDrawables(const aiNode& node,
                     const glm::mat4& modelFromParent,
                     LoadedModel& model)
{
    const glm::mat4 modelFromMesh = modelFromParent * ToMatrix4(node.mTransformation);
    for (unsigned int slot = 0; slot < node.mNumMeshes; ++slot) {
        const std::uint32_t meshIndex = node.mMeshes[slot];
        if (meshIndex >= model.meshes.size()) {
            throw ModelLoadError("Assimp returned an invalid node mesh reference.");
        }
        if (model.meshes[meshIndex].indexCount == 0) {
            continue;
        }
        model.drawables.push_back({node.mName.C_Str(), meshIndex, modelFromMesh});
    }
    for (unsigned int child = 0; child < node.mNumChildren; ++child) {
        AppendDrawables(*node.mChildren[child], modelFromMesh, model);
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
    importer.SetPropertyInteger(
        AI_CONFIG_PP_SBP_REMOVE,
        aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    if (IsFbxImport(path, options.format)) {
        importer.SetPropertyInteger(
            AI_CONFIG_IMPORT_FBX_READ_MATERIALS,
            options.loadMaterials ? 1 : 0);
        importer.SetPropertyInteger(
            AI_CONFIG_IMPORT_FBX_READ_TEXTURES,
            options.loadMaterials ? 1 : 0);
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
        | aiProcess_SortByPType | aiProcess_FindInvalidData
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

    const aiScene* importedScene = importer.ReadFile(sourcePath.u8string(), postProcessFlags);
    if (importedScene == nullptr || importedScene->mRootNode == nullptr) {
        const std::string detail = importer.GetErrorString();
        throw ModelLoadError(
            "Failed to load model '" + sourcePath.u8string() + "': "
            + (detail.empty() ? std::string("Assimp returned no scene.") : detail));
    }

    std::shared_ptr<LoadedModel> model = std::make_shared<LoadedModel>();
    model->name = sourcePath.stem().u8string();
    model->sourcePath = sourcePath;
    if ((importedScene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        model->warnings.push_back("Assimp marked the imported scene as incomplete.");
    }

    detail::ImportMaterials(*model, *importedScene, options);

    ReserveGeometry(*importedScene, model->geometry);
    model->meshes.reserve(importedScene->mNumMeshes);
    for (unsigned int index = 0; index < importedScene->mNumMeshes; ++index) {
        const aiMesh& mesh = *importedScene->mMeshes[index];
        const std::uint32_t materialIndex = options.loadMaterials ? mesh.mMaterialIndex : 0;
        if (materialIndex >= model->materials.size()) {
            throw ModelLoadError("Assimp returned an invalid mesh material reference.");
        }
        AppendMeshGeometry(mesh, materialIndex, *model);
    }

    model->drawables.reserve(importedScene->mNumMeshes);
    // Apply object scale without Assimp's GlobalScale TRS decomposition, which
    // can discard shear. Shared geometry stays in its original mesh space.
    glm::mat4 modelFromRoot(options.globalScale);
    modelFromRoot[3][3] = 1.0f;
    AppendDrawables(*importedScene->mRootNode, modelFromRoot, *model);
    model->bounds = model->CalculateBounds();
    return model;
}

} // namespace vpgloader
