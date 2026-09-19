#pragma once

#include <VPGLoader/ModelLoader.hpp>

#include <functional>

namespace vpgloader::detail {

TextureHandle LoadExternalTexture(const std::filesystem::path& path,
                                  const ModelLoadOptions& options);

// Decode independently on workers; commit results and diagnostics in input order.
void LoadTextureBatch(LoadedModel& model,
                      const std::vector<std::size_t>& pending,
                      const ModelLoadOptions& options,
                      const std::function<TextureHandle(std::size_t)>& decode);

} // namespace vpgloader::detail
