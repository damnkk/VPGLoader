#pragma once

#include <VPGLoader/Api.hpp>
#include <VPGLoader/TextureLoader.hpp>

#include <condition_variable>
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vpgloader {

struct TextureCacheOptions {
    // Zero keeps only weak references: CPU data is freed when callers drop handles.
    std::size_t maxResidentBytes = 0;
};

class VPGLOADER_API TextureCache final {
public:
    explicit TextureCache(TextureCacheOptions options = {});
    ~TextureCache();

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    TextureHandle Load(const std::filesystem::path& path,
                       const TextureLoadOptions& options = {});
    TextureHandle Find(const std::filesystem::path& path,
                       const TextureLoadOptions& options = {});

    // Removes all cached variants of a source path. Existing handles stay valid.
    void Invalidate(const std::filesystem::path& path);
    void Clear();
    std::size_t PurgeExpired();

    void SetMaxResidentBytes(std::size_t bytes);
    std::size_t maxResidentBytes() const noexcept;
    std::size_t residentBytes() const noexcept;

    static TextureCache& Default();

private:
    struct Entry;

    static std::string BuildSourceKey(const std::filesystem::path& path);
    static std::string BuildCacheKey(const std::filesystem::path& path,
                                     const TextureLoadOptions& options);

    void RetainLocked(const std::string& key, const std::shared_ptr<Entry>& entry,
                      const TextureHandle& texture);
    void RemoveResidentLocked(const std::string& key, const std::shared_ptr<Entry>& entry);
    void TrimLocked();

    TextureCacheOptions options_;
    std::size_t residentBytes_ = 0;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
    std::list<std::string> lru_;
    mutable std::mutex mutex_;
};

} // namespace vpgloader
