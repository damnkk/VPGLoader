#include <VPGLoader/ModelExporter.hpp>

#include "ModelBinary.hpp"

#include <exception>
#include <stdexcept>

namespace vpgloader {

ModelExportError::ModelExportError(const std::string& message)
    : std::runtime_error(message)
{
}

void ModelExporter::Save(const LoadedModel& model,
                         const std::filesystem::path& destination)
{
    try {
        detail::SaveVpgModel(model, destination);
    } catch (const ModelExportError&) {
        throw;
    } catch (const std::exception& error) {
        throw ModelExportError(
            "Failed to export model '" + destination.u8string() + "': "
            + error.what());
    }
}

void ModelExporter::Save(const ModelHandle& model,
                         const std::filesystem::path& destination)
{
    if (!model) {
        throw ModelExportError("Cannot export an empty ModelHandle.");
    }
    Save(*model, destination);
}

} // namespace vpgloader
