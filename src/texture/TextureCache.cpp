#include <VPGLoader/TextureCache.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace vpgloader {

struct TextureCache::Entry {
    std::weak_ptr<const Texture> texture;
    TextureHandle resident;
    std::string sourceKey;
    std::list<std::string>::iterator lruPosition;
    bool isInLru = false;
    bool isLoading = false;
    std::condition_variable completion;
};

TextureCache::TextureCache(TextureCacheOptions options)
    : options_(options)
{
}

TextureCache::~TextureCache() = default;

TextureHandle TextureCache::Load(const std::filesystem::path& path,
                                 const TextureLoadOptions& options)
{
    const std::string key = BuildCacheKey(path, options);
    std::shared_ptr<Entry> entry;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto [iterator, inserted]
            = entries_.try_emplace(key, std::make_shared<Entry>());
        entry = iterator->second;
        if (inserted) {
            entry->sourceKey = BuildSourceKey(path);
        }

        for (;;) {
            if (TextureHandle texture = entry->texture.lock()) {
                RetainLocked(key, entry, texture);
                return texture;
            }
            if (!entry->isLoading) {
                entry->isLoading = true;
                break;
            }
            entry->completion.wait(lock, [&entry] { return !entry->isLoading; });
        }
    }

    try {
        TextureHandle texture = TextureLoader::Load(path, options);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entry->texture = texture;
            entry->isLoading = false;
            RetainLocked(key, entry, texture);
        }
        entry->completion.notify_all();
        return texture;
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entry->isLoading = false;
        }
        entry->completion.notify_all();
        throw;
    }
}

TextureHandle TextureCache::Find(const std::filesystem::path& path,
                                 const TextureLoadOptions& options)
{
    const std::string key = BuildCacheKey(path, options);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = entries_.find(key);
    if (iterator == entries_.end()) {
        return {};
    }

    TextureHandle texture = iterator->second->texture.lock();
    if (texture) {
        RetainLocked(key, iterator->second, texture);
    }
    return texture;
}

void TextureCache::Invalidate(const std::filesystem::path& path)
{
    const std::string sourceKey = BuildSourceKey(path);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        if (iterator->second->sourceKey == sourceKey) {
            RemoveResidentLocked(iterator->first, iterator->second);
            iterator = entries_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void TextureCache::Clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, entry] : entries_) {
        RemoveResidentLocked(key, entry);
    }
    entries_.clear();
    lru_.clear();
    residentBytes_ = 0;
}

std::size_t TextureCache::PurgeExpired()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t removed = 0;
    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        const std::shared_ptr<Entry>& entry = iterator->second;
        if (!entry->isLoading && !entry->resident && entry->texture.expired()) {
            iterator = entries_.erase(iterator);
            ++removed;
        } else {
            ++iterator;
        }
    }
    return removed;
}

void TextureCache::SetMaxResidentBytes(std::size_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    options_.maxResidentBytes = bytes;
    TrimLocked();
}

std::size_t TextureCache::maxResidentBytes() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return options_.maxResidentBytes;
}

std::size_t TextureCache::residentBytes() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return residentBytes_;
}

TextureCache& TextureCache::Default()
{
    static TextureCache cache;
    return cache;
}

std::string TextureCache::BuildSourceKey(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::absolute(path, error);
    if (error) {
        normalized = path;
        error.clear();
    }
    normalized = std::filesystem::weakly_canonical(normalized, error);
    if (error) {
        normalized = normalized.lexically_normal();
    }

    std::string key = normalized.generic_u8string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
#endif
    return key;
}

std::string TextureCache::BuildCacheKey(const std::filesystem::path& path,
                                        const TextureLoadOptions& options)
{
    return BuildSourceKey(path) + "|type="
        + std::to_string(static_cast<unsigned int>(options.outputComponentType))
        + "|mips=" + (options.loadMipmaps ? "1" : "0");
}

void TextureCache::RetainLocked(const std::string& key, const std::shared_ptr<Entry>& entry,
                                const TextureHandle& texture)
{
    if (options_.maxResidentBytes == 0) {
        return;
    }

    if (!entry->resident) {
        entry->resident = texture;
        residentBytes_ += texture->byteSize();
        lru_.push_front(key);
        entry->lruPosition = lru_.begin();
        entry->isInLru = true;
    } else if (entry->isInLru) {
        lru_.splice(lru_.begin(), lru_, entry->lruPosition);
    }
    TrimLocked();
}

void TextureCache::RemoveResidentLocked(const std::string&, const std::shared_ptr<Entry>& entry)
{
    if (!entry->resident) {
        return;
    }

    residentBytes_ -= entry->resident->byteSize();
    entry->resident.reset();
    if (entry->isInLru) {
        lru_.erase(entry->lruPosition);
        entry->isInLru = false;
    }
}

void TextureCache::TrimLocked()
{
    while (residentBytes_ > options_.maxResidentBytes && !lru_.empty()) {
        const std::string key = lru_.back();
        const auto iterator = entries_.find(key);
        if (iterator == entries_.end()) {
            lru_.pop_back();
            continue;
        }
        RemoveResidentLocked(iterator->first, iterator->second);
    }
}

} // namespace vpgloader
