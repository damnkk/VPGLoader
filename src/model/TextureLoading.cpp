#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <utility>

#include "TextureLoading.hpp"

namespace vpgloader::detail {
namespace {

// C++17 threads must be joined even if starting another worker throws.
struct JoiningThreads {
  std::vector<std::thread> threads;
  ~JoiningThreads() {
    for (std::thread& thread : threads) {
      thread.join();
    }
  }
};

}  // namespace

TextureHandle LoadExternalTexture(const std::filesystem::path& path,
                                  const ModelLoadOptions& options) {
  return options.cacheExternalTextures
             ? TextureLoader::LoadCached(path, options.textureLoadOptions)
             : TextureLoader::Load(path, options.textureLoadOptions);
}

void LoadTextureBatch(LoadedModel& model,
                      const std::vector<std::size_t>& pending,
                      const ModelLoadOptions& options,
                      const std::function<TextureHandle(std::size_t)>& decode) {
  if (pending.empty()) {
    return;
  }
  struct Result {
    TextureHandle texture;
    std::exception_ptr failure;
  };
  std::vector<Result> results(pending.size());
  std::atomic<std::size_t> next{0};
  const auto worker = [&] {
    for (;;) {
      const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
      if (index >= pending.size()) {
        return;
      }
      try {
        results[index].texture = decode(pending[index]);
      } catch (...) {
        results[index].failure = std::current_exception();
      }
    }
  };

  std::size_t workerCount = options.maxTextureLoadConcurrency;
  if (workerCount == 0) {
    workerCount = std::max(1U, std::thread::hardware_concurrency());
  }
  workerCount = std::min(workerCount, pending.size());
  {
    JoiningThreads workers;
    workers.threads.reserve(workerCount - 1);
    for (std::size_t index = 1; index < workerCount; ++index) {
      workers.threads.emplace_back(worker);
    }
    worker();
  }

  for (std::size_t index = 0; index < pending.size(); ++index) {
    ModelTextureAsset& asset = model.textures[pending[index]];
    Result& result = results[index];
    asset.texture = std::move(result.texture);
    asset.loadError.clear();
    if (!result.failure) {
      continue;
    }
    try {
      std::rethrow_exception(result.failure);
    } catch (const std::exception& error) {
      asset.loadError =
          "Failed to load model texture '" + asset.name + "': " + error.what();
    }
    if (options.failOnMissingTextures) {
      throw ModelLoadError(asset.loadError);
    }
    model.warnings.push_back(asset.loadError);
  }
}

}  // namespace vpgloader::detail
