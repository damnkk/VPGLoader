#pragma once

#include <VPGLoader/ModelLoader.hpp>

struct aiScene;

namespace vpgloader::detail {

void ImportMaterials(LoadedModel& model, const aiScene& scene,
                     const ModelLoadOptions& options);

} // namespace vpgloader::detail
