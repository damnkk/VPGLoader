#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Model.hpp>
#include <VPGLoader/TextureLoader.hpp>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace vpgloader {

enum class ModelFileFormat : std::uint8_t {
    Auto,
    Gltf,
    Obj,
    Fbx,
};

struct ModelLoadOptions {
    ModelFileFormat format = ModelFileFormat::Auto;

    // Zero selects ModelLoader::DefaultAssimpPostProcessFlags().
    std::uint32_t assimpPostProcessFlags = 0;
    std::uint32_t extraAssimpPostProcessFlags = 0;
    float globalScale = 1.0f;

    bool loadMaterials = true;
    bool loadTextures = true;
    bool cacheExternalTextures = true;
    // Assimp only extracts encoded bytes for container-embedded images;
    // VPGLoader still performs the actual image decoding through OpenImageIO.
    bool loadEmbeddedTextures = true;
    // Zero uses std::thread::hardware_concurrency(). The actual worker count
    // never exceeds the number of registered texture requests.
    std::uint32_t maxTextureLoadConcurrency = 0;
    bool failOnMissingTextures = false;
    bool srgbBaseColorTextures = true;
    bool srgbEmissiveTextures = true;
    TextureLoadOptions textureLoadOptions;
};

class VPGLOADER_API ModelLoadError : public std::runtime_error {
public:
    explicit ModelLoadError(const std::string& message);
};

class VPGLOADER_API ModelLoader final {
public:
    static std::uint32_t DefaultAssimpPostProcessFlags() noexcept;

    // Parses a complete CPU model and returns shared immutable ownership.
    static ModelHandle Load(const std::filesystem::path& path,
                            const ModelLoadOptions& options = {});
};

} // namespace vpgloader
