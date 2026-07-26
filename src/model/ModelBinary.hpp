#pragma once

#include <VPGLoader/Model.hpp>

#include <filesystem>

namespace vpgloader {

struct ModelLoadOptions;

namespace detail {

bool IsVpgModelPath(const std::filesystem::path& path) noexcept;
void SaveVpgModel(const LoadedModel& model,
                  const std::filesystem::path& destination);
ModelHandle LoadVpgModel(const std::filesystem::path& path,
                         const ModelLoadOptions& options);

} // namespace detail
} // namespace vpgloader
