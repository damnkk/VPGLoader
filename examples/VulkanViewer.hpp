#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

class VulkanViewer final {
public:
    explicit VulkanViewer(std::filesystem::path modelPath,
                          std::uint64_t frameLimit = 0);
    ~VulkanViewer();

    VulkanViewer(const VulkanViewer&) = delete;
    VulkanViewer& operator=(const VulkanViewer&) = delete;

    int Run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
