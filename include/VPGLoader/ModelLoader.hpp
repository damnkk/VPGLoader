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
    VpgModel,
};

struct ModelLoadOptions {
    ModelFileFormat format = ModelFileFormat::Auto;

    // Assimp imports only. Zero selects DefaultAssimpPostProcessFlags().
    std::uint32_t assimpPostProcessFlags = 0;
    std::uint32_t extraAssimpPostProcessFlags = 0;
    // Uniform model-space scale applied to drawable matrices, not vertex data.
    float globalScale = 1.0f;

    bool loadMaterials = true;
    // Decode pixels. Texture references and UV metadata are retained when false.
    bool loadTextures = true;
    bool cacheExternalTextures = true;
    // Embedded images use TextureLoader too: encoded bytes go to LoadFromMemory,
    // while uncompressed Assimp pixels go to LoadFromPixels.
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
    // Auto detects the native .vpgmodel format and bypasses Assimp for it.
    static ModelHandle Load(const std::filesystem::path& path,
                            const ModelLoadOptions& options = {});
};

} // namespace vpgloader
