#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/Model.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace vpgloader {

class VPGLOADER_API ModelExportError : public std::runtime_error {
public:
    explicit ModelExportError(const std::string& message);
};

class VPGLOADER_API ModelExporter final {
public:
    // Serializes the complete CPU-side model into a versioned .vpgmodel file.
    // Loaded textures are written as sibling KTX files and referenced by
    // filename from the model file.
    static void Save(const LoadedModel& model,
                     const std::filesystem::path& destination);
    static void Save(const ModelHandle& model,
                     const std::filesystem::path& destination);
};

} // namespace vpgloader
