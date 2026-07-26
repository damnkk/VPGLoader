#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "VulkanViewer.hpp"

#include <VPGLoader/ModelLoader.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t WindowWidth = 1280;
constexpr std::uint32_t WindowHeight = 720;
constexpr std::size_t FramesInFlight = 2;
constexpr float Pi = 3.14159265358979323846f;

void Check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::string(operation) + " failed with VkResult " +
            std::to_string(static_cast<int>(result)));
    }
}

struct Vertex {
    float position[3];
    float texCoord[2];
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct QueueFamilies {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    bool Complete() const
    {
        return graphics.has_value() && present.has_value();
    }
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct DrawItem {
    std::uint32_t meshIndex = 0;
    std::uint32_t materialIndex = 0;
    vpgloader::Matrix4 model;
};

struct alignas(16) CameraData {
    vpgloader::Matrix4 viewProjection;
};

struct alignas(16) DrawData {
    vpgloader::Matrix4 model;
    vpgloader::Float4 baseColorFactor;
    float alphaCutoff = 0.5f;
    std::uint32_t alphaMode = 0;
    float padding[2] = {};
};

static_assert(sizeof(DrawData) == 96, "Push constants must match the GLSL layout");

vpgloader::Float3 Subtract(const vpgloader::Float3& left,
                           const vpgloader::Float3& right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

float Dot(const vpgloader::Float3& left, const vpgloader::Float3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

vpgloader::Float3 Cross(const vpgloader::Float3& left,
                        const vpgloader::Float3& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

vpgloader::Float3 Normalize(const vpgloader::Float3& value)
{
    const float length = std::sqrt(Dot(value, value));
    if (length <= std::numeric_limits<float>::epsilon()) {
        return {};
    }
    return {value.x / length, value.y / length, value.z / length};
}

vpgloader::Matrix4 LookAt(const vpgloader::Float3& eye,
                          const vpgloader::Float3& target)
{
    const auto forward = Normalize(Subtract(target, eye));
    const auto side = Normalize(Cross(forward, {0.0f, 1.0f, 0.0f}));
    const auto up = Cross(side, forward);

    vpgloader::Matrix4 result;
    result.values = {
        side.x, up.x, -forward.x, 0.0f,
        side.y, up.y, -forward.y, 0.0f,
        side.z, up.z, -forward.z, 0.0f,
        -Dot(side, eye), -Dot(up, eye), Dot(forward, eye), 1.0f,
    };
    return result;
}

vpgloader::Matrix4 Perspective(float verticalFieldOfView,
                               float aspect,
                               float nearPlane,
                               float farPlane)
{
    const float inverseTan =
        1.0f / std::tan(verticalFieldOfView * 0.5f);

    vpgloader::Matrix4 result;
    result.values.fill(0.0f);
    result.values[0] = inverseTan / aspect;
    result.values[5] = -inverseTan;
    result.values[10] = farPlane / (nearPlane - farPlane);
    result.values[11] = -1.0f;
    result.values[14] =
        (farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

std::vector<char> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::ate | std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open shader: " + path.string());
    }

    const auto size = stream.tellg();
    if (size <= 0) {
        throw std::runtime_error("Shader is empty: " + path.string());
    }

    std::vector<char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(bytes.data(), size);
    if (!stream) {
        throw std::runtime_error("Cannot read shader: " + path.string());
    }
    return bytes;
}

std::uint32_t ToAlphaMode(vpgloader::AlphaMode mode)
{
    switch (mode) {
    case vpgloader::AlphaMode::Mask:
        return 1;
    case vpgloader::AlphaMode::Blend:
        return 2;
    default:
        return 0;
    }
}

} // namespace

struct VulkanViewer::Impl {
    explicit Impl(std::filesystem::path path, std::uint64_t limit)
        : modelPath(std::move(path)), frameLimit(limit)
    {
    }

    ~Impl()
    {
        Cleanup();
    }

    int Run()
    {
        LoadModel();
        InitWindow();
        InitVulkan();

        const auto start = std::chrono::steady_clock::now();
        std::uint64_t renderedFrames = 0;
        while (!glfwWindowShouldClose(window) &&
               (frameLimit == 0 || renderedFrames < frameLimit)) {
            glfwPollEvents();
            if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            const auto elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - start).count();
            DrawFrame(elapsed);
            ++renderedFrames;
        }

        Check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        return 0;
    }

    void LoadModel()
    {
        if (!std::filesystem::exists(modelPath)) {
            throw std::runtime_error(
                "Model does not exist: " + modelPath.string());
        }

        std::cout << "Loading model: " << modelPath << '\n';
        model = vpgloader::ModelLoader::Load(modelPath);
        if (!model || model->geometry.positions.empty() ||
            model->geometry.indices.empty()) {
            throw std::runtime_error(
                "The model has no indexed triangle geometry");
        }

        for (const auto& warning : model->warnings) {
            std::cerr << "Model warning: " << warning << '\n';
        }

        BuildDrawItems();
        if (drawItems.empty()) {
            throw std::runtime_error("The model contains no drawable submeshes");
        }

        if (model->asset.bounds.valid) {
            orbitCenter = {
                (model->asset.bounds.min.x + model->asset.bounds.max.x) * 0.5f,
                (model->asset.bounds.min.y + model->asset.bounds.max.y) * 0.5f,
                (model->asset.bounds.min.z + model->asset.bounds.max.z) * 0.5f,
            };
            const auto extent = Subtract(
                model->asset.bounds.max, model->asset.bounds.min);
            orbitRadius = std::max(
                0.1f, 0.5f * std::sqrt(Dot(extent, extent)));
        }

        std::cout
            << "Loaded " << model->meshes.size() << " meshes, "
            << model->geometry.positions.size() << " vertices, "
            << model->textures.size() << " texture references\n";
    }

    void BuildDrawItems()
    {
        if (model->asset.nodes.empty()) {
            for (std::uint32_t meshIndex = 0;
                 meshIndex < model->meshes.size(); ++meshIndex) {
                drawItems.push_back(
                    {meshIndex, model->meshes[meshIndex].materialIndex, {}});
            }
            return;
        }

        std::vector<bool> visited(model->asset.nodes.size(), false);
        std::function<void(std::uint32_t, const vpgloader::Matrix4&)> visit;
        visit = [&](std::uint32_t nodeIndex,
                    const vpgloader::Matrix4& parentTransform) {
            if (nodeIndex >= model->asset.nodes.size() || visited[nodeIndex]) {
                return;
            }
            visited[nodeIndex] = true;

            const auto& node = model->asset.nodes[nodeIndex];
            vpgloader::Matrix4 localTransform;
            if (node.transformIndex < model->asset.transforms.size()) {
                localTransform = model->asset.transforms[node.transformIndex];
            }
            const auto worldTransform =
                vpgloader::Multiply(parentTransform, localTransform);

            for (const auto submeshIndex : node.submeshIndices) {
                if (submeshIndex >= model->asset.submeshes.size()) {
                    continue;
                }
                const auto meshIndex =
                    model->asset.submeshes[submeshIndex].meshIndex;
                if (meshIndex >= model->meshes.size()) {
                    continue;
                }
                drawItems.push_back({
                    meshIndex,
                    model->meshes[meshIndex].materialIndex,
                    worldTransform,
                });
            }

            auto child = node.firstChild;
            while (child != vpgloader::InvalidModelIndex &&
                   child < model->asset.nodes.size()) {
                visit(child, worldTransform);
                child = model->asset.nodes[child].nextSibling;
            }
        };

        const vpgloader::Matrix4 identity;
        if (model->asset.rootNode < model->asset.nodes.size()) {
            visit(model->asset.rootNode, identity);
        }
        for (std::uint32_t nodeIndex = 0;
             nodeIndex < model->asset.nodes.size(); ++nodeIndex) {
            if (!visited[nodeIndex]) {
                visit(nodeIndex, identity);
            }
        }
    }

    void InitWindow()
    {
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("GLFW initialization failed");
        }
        glfwInitialized = true;

        if (glfwVulkanSupported() != GLFW_TRUE) {
            throw std::runtime_error(
                "GLFW cannot find a Vulkan loader or Vulkan-capable driver");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window = glfwCreateWindow(
            WindowWidth, WindowHeight, "VPGLoader Vulkan Viewer", nullptr, nullptr);
        if (!window) {
            throw std::runtime_error("GLFW window creation failed");
        }

        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(
            window,
            [](GLFWwindow* callbackWindow, int, int) {
                auto* self = static_cast<Impl*>(
                    glfwGetWindowUserPointer(callbackWindow));
                self->framebufferResized = true;
            });
    }

    void InitVulkan()
    {
        CreateInstance();
        Check(glfwCreateWindowSurface(instance, window, nullptr, &surface),
              "glfwCreateWindowSurface");
        PickPhysicalDevice();
        CreateLogicalDevice();
        CreateSwapchain();
        CreateSwapchainImageViews();
        CreateRenderPass();
        CreateDescriptorSetLayout();
        CreatePipelineLayout();
        CreateGraphicsPipeline();
        CreateCommandPool();
        CreateDepthResources();
        CreateFramebuffers();
        CreateModelBuffers();
        CreateTextureResources();
        CreateUniformBuffers();
        CreateDescriptorSets();
        CreateCommandBuffers();
        CreateSynchronization();
    }

    void CreateInstance()
    {
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "VPGLoader Vulkan Viewer";
        application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        application.pEngineName = "VPGLoader Example";
        application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        application.apiVersion = VK_API_VERSION_1_0;

        std::uint32_t extensionCount = 0;
        const char** extensions =
            glfwGetRequiredInstanceExtensions(&extensionCount);
        if (!extensions || extensionCount == 0) {
            throw std::runtime_error(
                "GLFW did not provide Vulkan instance extensions");
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &application;
        createInfo.enabledExtensionCount = extensionCount;
        createInfo.ppEnabledExtensionNames = extensions;
        Check(vkCreateInstance(&createInfo, nullptr, &instance),
              "vkCreateInstance");
    }

    QueueFamilies FindQueueFamilies(VkPhysicalDevice candidate) const
    {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> properties(count);
        vkGetPhysicalDeviceQueueFamilyProperties(
            candidate, &count, properties.data());

        QueueFamilies result;
        for (std::uint32_t index = 0; index < count; ++index) {
            if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                result.graphics = index;
            }

            VkBool32 supportsPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(
                candidate, index, surface, &supportsPresent);
            if (supportsPresent == VK_TRUE) {
                result.present = index;
            }
            if (result.Complete()) {
                break;
            }
        }
        return result;
    }

    SwapchainSupport QuerySwapchainSupport(VkPhysicalDevice candidate) const
    {
        SwapchainSupport support;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            candidate, surface, &support.capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            candidate, surface, &formatCount, nullptr);
        support.formats.resize(formatCount);
        if (formatCount != 0) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(
                candidate, surface, &formatCount, support.formats.data());
        }

        std::uint32_t modeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            candidate, surface, &modeCount, nullptr);
        support.presentModes.resize(modeCount);
        if (modeCount != 0) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(
                candidate, surface, &modeCount, support.presentModes.data());
        }
        return support;
    }

    bool SupportsRequiredDeviceExtensions(VkPhysicalDevice candidate) const
    {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(
            candidate, nullptr, &count, available.data());

        for (const auto& extension : available) {
            if (std::strcmp(extension.extensionName,
                            VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                return true;
            }
        }
        return false;
    }

    bool IsSuitable(VkPhysicalDevice candidate) const
    {
        if (!FindQueueFamilies(candidate).Complete() ||
            !SupportsRequiredDeviceExtensions(candidate)) {
            return false;
        }
        const auto swapchain = QuerySwapchainSupport(candidate);
        return !swapchain.formats.empty() &&
               !swapchain.presentModes.empty();
    }

    void PickPhysicalDevice()
    {
        std::uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
              "vkEnumeratePhysicalDevices");
        if (count == 0) {
            throw std::runtime_error("No Vulkan physical device was found");
        }

        std::vector<VkPhysicalDevice> candidates(count);
        Check(vkEnumeratePhysicalDevices(
                  instance, &count, candidates.data()),
              "vkEnumeratePhysicalDevices");

        for (const auto candidate : candidates) {
            if (IsSuitable(candidate)) {
                physicalDevice = candidate;
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "No device supports graphics, presentation, and swapchains");
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        std::cout << "Vulkan device: " << properties.deviceName << '\n';
    }

    void CreateLogicalDevice()
    {
        queueFamilies = FindQueueFamilies(physicalDevice);
        const std::set<std::uint32_t> uniqueFamilies = {
            *queueFamilies.graphics, *queueFamilies.present};
        constexpr float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        for (const auto family : uniqueFamilies) {
            VkDeviceQueueCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            info.queueFamilyIndex = family;
            info.queueCount = 1;
            info.pQueuePriorities = &priority;
            queueInfos.push_back(info);
        }

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount =
            static_cast<std::uint32_t>(queueInfos.size());
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.pEnabledFeatures = &features;
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = extensions;
        Check(vkCreateDevice(
                  physicalDevice, &createInfo, nullptr, &device),
              "vkCreateDevice");

        vkGetDeviceQueue(device, *queueFamilies.graphics, 0, &graphicsQueue);
        vkGetDeviceQueue(device, *queueFamilies.present, 0, &presentQueue);
    }

    VkSurfaceFormatKHR ChooseSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& formats) const
    {
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return formats.front();
    }

    VkPresentModeKHR ChoosePresentMode(
        const std::vector<VkPresentModeKHR>& modes) const
    {
        for (const auto mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return mode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities) const
    {
        if (capabilities.currentExtent.width !=
            std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        VkExtent2D extent = {
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
        };
        extent.width = std::clamp(
            extent.width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            extent.height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
        return extent;
    }

    void CreateSwapchain()
    {
        const auto support = QuerySwapchainSupport(physicalDevice);
        const auto format = ChooseSurfaceFormat(support.formats);
        const auto presentMode = ChoosePresentMode(support.presentModes);
        const auto extent = ChooseExtent(support.capabilities);

        std::uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0) {
            imageCount = std::min(
                imageCount, support.capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = format.format;
        createInfo.imageColorSpace = format.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        const std::uint32_t familyIndices[] = {
            *queueFamilies.graphics, *queueFamilies.present};
        if (queueFamilies.graphics != queueFamilies.present) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = familyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        Check(vkCreateSwapchainKHR(
                  device, &createInfo, nullptr, &swapchain),
              "vkCreateSwapchainKHR");

        Check(vkGetSwapchainImagesKHR(
                  device, swapchain, &imageCount, nullptr),
              "vkGetSwapchainImagesKHR");
        swapchainImages.resize(imageCount);
        Check(vkGetSwapchainImagesKHR(
                  device, swapchain, &imageCount, swapchainImages.data()),
              "vkGetSwapchainImagesKHR");
        swapchainFormat = format.format;
        swapchainExtent = extent;
        imageFences.assign(imageCount, VK_NULL_HANDLE);
    }

    VkImageView CreateImageView(VkImage image,
                                VkFormat format,
                                VkImageAspectFlags aspect) const
    {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = image;
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = format;
        createInfo.subresourceRange.aspectMask = aspect;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        Check(vkCreateImageView(device, &createInfo, nullptr, &view),
              "vkCreateImageView");
        return view;
    }

    void CreateSwapchainImageViews()
    {
        swapchainImageViews.reserve(swapchainImages.size());
        for (const auto image : swapchainImages) {
            swapchainImageViews.push_back(
                CreateImageView(
                    image, swapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT));
        }
    }

    VkFormat FindDepthFormat() const
    {
        const VkFormat candidates[] = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT,
        };
        for (const auto format : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(
                physicalDevice, format, &properties);
            if ((properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                return format;
            }
        }
        throw std::runtime_error(
            "No supported depth attachment format was found");
    }

    bool HasStencil(VkFormat format) const
    {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
               format == VK_FORMAT_D24_UNORM_S8_UINT;
    }

    void CreateRenderPass()
    {
        depthFormat = FindDepthFormat();

        VkAttachmentDescription color{};
        color.format = swapchainFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depth{};
        depth.format = depthFormat;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depthReference{};
        depthReference.attachment = 1;
        depthReference.layout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.dstAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        const VkAttachmentDescription attachments[] = {color, depth};
        VkRenderPassCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = 2;
        createInfo.pAttachments = attachments;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;
        Check(vkCreateRenderPass(
                  device, &createInfo, nullptr, &renderPass),
              "vkCreateRenderPass");
    }

    void CreateDescriptorSetLayout()
    {
        VkDescriptorSetLayoutBinding camera{};
        camera.binding = 0;
        camera.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camera.descriptorCount = 1;
        camera.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding texture{};
        texture.binding = 1;
        texture.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture.descriptorCount = 1;
        texture.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        const VkDescriptorSetLayoutBinding bindings[] = {camera, texture};
        VkDescriptorSetLayoutCreateInfo createInfo{};
        createInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        createInfo.bindingCount = 2;
        createInfo.pBindings = bindings;
        Check(vkCreateDescriptorSetLayout(
                  device, &createInfo, nullptr, &descriptorSetLayout),
              "vkCreateDescriptorSetLayout");
    }

    void CreatePipelineLayout()
    {
        VkPushConstantRange pushConstants{};
        pushConstants.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstants.offset = 0;
        pushConstants.size = sizeof(DrawData);

        VkPipelineLayoutCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        createInfo.setLayoutCount = 1;
        createInfo.pSetLayouts = &descriptorSetLayout;
        createInfo.pushConstantRangeCount = 1;
        createInfo.pPushConstantRanges = &pushConstants;
        Check(vkCreatePipelineLayout(
                  device, &createInfo, nullptr, &pipelineLayout),
              "vkCreatePipelineLayout");
    }

    VkShaderModule CreateShaderModule(const std::vector<char>& bytes) const
    {
        if ((bytes.size() % sizeof(std::uint32_t)) != 0) {
            throw std::runtime_error("SPIR-V byte count is not word aligned");
        }
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = bytes.size();
        createInfo.pCode =
            reinterpret_cast<const std::uint32_t*>(bytes.data());

        VkShaderModule module = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(device, &createInfo, nullptr, &module),
              "vkCreateShaderModule");
        return module;
    }

    void CreateGraphicsPipeline()
    {
        const auto shaderDirectory =
            std::filesystem::path(VPGLOADER_VIEWER_SHADER_DIR);
        const auto vertexBytes =
            ReadBinaryFile(shaderDirectory / "model.vert.spv");
        const auto fragmentBytes =
            ReadBinaryFile(shaderDirectory / "model.frag.spv");
        const auto vertexModule = CreateShaderModule(vertexBytes);
        const auto fragmentModule = CreateShaderModule(fragmentBytes);

        try {
            VkPipelineShaderStageCreateInfo vertexStage{};
            vertexStage.sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertexStage.module = vertexModule;
            vertexStage.pName = "main";

            VkPipelineShaderStageCreateInfo fragmentStage{};
            fragmentStage.sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragmentStage.module = fragmentModule;
            fragmentStage.pName = "main";
            const VkPipelineShaderStageCreateInfo stages[] = {
                vertexStage, fragmentStage};

            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            VkVertexInputAttributeDescription attributes[2]{};
            attributes[0].location = 0;
            attributes[0].binding = 0;
            attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
            attributes[0].offset = offsetof(Vertex, position);
            attributes[1].location = 1;
            attributes[1].binding = 0;
            attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
            attributes[1].offset = offsetof(Vertex, texCoord);

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType =
                VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 2;
            vertexInput.pVertexAttributeDescriptions = attributes;

            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType =
                VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType =
                VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterization{};
            rasterization.sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;
            rasterization.cullMode = VK_CULL_MODE_NONE;
            rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterization.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo multisample{};
            multisample.sType =
                VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType =
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

            VkPipelineColorBlendAttachmentState blendAttachment{};
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor =
                VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstColorBlendFactor =
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.dstAlphaBlendFactor =
                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT |
                VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT |
                VK_COLOR_COMPONENT_A_BIT;

            VkPipelineColorBlendStateCreateInfo blending{};
            blending.sType =
                VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            blending.attachmentCount = 1;
            blending.pAttachments = &blendAttachment;

            const VkDynamicState dynamicStates[] = {
                VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType =
                VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dynamicStates;

            VkGraphicsPipelineCreateInfo createInfo{};
            createInfo.sType =
                VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            createInfo.stageCount = 2;
            createInfo.pStages = stages;
            createInfo.pVertexInputState = &vertexInput;
            createInfo.pInputAssemblyState = &assembly;
            createInfo.pViewportState = &viewport;
            createInfo.pRasterizationState = &rasterization;
            createInfo.pMultisampleState = &multisample;
            createInfo.pDepthStencilState = &depthStencil;
            createInfo.pColorBlendState = &blending;
            createInfo.pDynamicState = &dynamic;
            createInfo.layout = pipelineLayout;
            createInfo.renderPass = renderPass;
            createInfo.subpass = 0;
            Check(vkCreateGraphicsPipelines(
                      device,
                      VK_NULL_HANDLE,
                      1,
                      &createInfo,
                      nullptr,
                      &graphicsPipeline),
                  "vkCreateGraphicsPipelines");
        } catch (...) {
            vkDestroyShaderModule(device, fragmentModule, nullptr);
            vkDestroyShaderModule(device, vertexModule, nullptr);
            throw;
        }

        vkDestroyShaderModule(device, fragmentModule, nullptr);
        vkDestroyShaderModule(device, vertexModule, nullptr);
    }

    void CreateCommandPool()
    {
        VkCommandPoolCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                           VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        createInfo.queueFamilyIndex = *queueFamilies.graphics;
        Check(vkCreateCommandPool(
                  device, &createInfo, nullptr, &commandPool),
              "vkCreateCommandPool");
    }

    std::uint32_t FindMemoryType(std::uint32_t typeFilter,
                                 VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(
            physicalDevice, &memoryProperties);
        for (std::uint32_t index = 0;
             index < memoryProperties.memoryTypeCount; ++index) {
            if ((typeFilter & (1u << index)) != 0 &&
                (memoryProperties.memoryTypes[index].propertyFlags &
                 properties) == properties) {
                return index;
            }
        }
        throw std::runtime_error("No compatible Vulkan memory type found");
    }

    Buffer CreateBuffer(VkDeviceSize size,
                        VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties) const
    {
        Buffer result;
        result.size = size;

        VkBufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        createInfo.size = size;
        createInfo.usage = usage;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(
                  device, &createInfo, nullptr, &result.buffer),
              "vkCreateBuffer");

        try {
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(
                device, result.buffer, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex =
                FindMemoryType(requirements.memoryTypeBits, properties);
            Check(vkAllocateMemory(
                      device, &allocation, nullptr, &result.memory),
                  "vkAllocateMemory");
            Check(vkBindBufferMemory(
                      device, result.buffer, result.memory, 0),
                  "vkBindBufferMemory");
        } catch (...) {
            if (result.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, result.memory, nullptr);
            }
            vkDestroyBuffer(device, result.buffer, nullptr);
            throw;
        }
        return result;
    }

    void DestroyBuffer(Buffer& buffer)
    {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, buffer.memory, nullptr);
        }
        buffer = {};
    }

    VkCommandBuffer BeginSingleTimeCommands()
    {
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &allocation, &command),
              "vkAllocateCommandBuffers");

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command, &begin),
              "vkBeginCommandBuffer");
        return command;
    }

    void EndSingleTimeCommands(VkCommandBuffer command)
    {
        Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(
                  graphicsQueue, 1, &submit, VK_NULL_HANDLE),
              "vkQueueSubmit");
        Check(vkQueueWaitIdle(graphicsQueue), "vkQueueWaitIdle");
        vkFreeCommandBuffers(device, commandPool, 1, &command);
    }

    void CopyBuffer(const Buffer& source,
                    const Buffer& destination,
                    VkDeviceSize size)
    {
        const auto command = BeginSingleTimeCommands();
        VkBufferCopy region{};
        region.size = size;
        vkCmdCopyBuffer(
            command, source.buffer, destination.buffer, 1, &region);
        EndSingleTimeCommands(command);
    }

    Image CreateImage(std::uint32_t width,
                      std::uint32_t height,
                      VkFormat format,
                      VkImageUsageFlags usage,
                      VkImageAspectFlags aspect) const
    {
        Image result;
        VkImageCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        createInfo.imageType = VK_IMAGE_TYPE_2D;
        createInfo.extent = {width, height, 1};
        createInfo.mipLevels = 1;
        createInfo.arrayLayers = 1;
        createInfo.format = format;
        createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        createInfo.usage = usage;
        createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateImage(
                  device, &createInfo, nullptr, &result.image),
              "vkCreateImage");

        try {
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(
                device, result.image, &requirements);
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = FindMemoryType(
                requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(vkAllocateMemory(
                      device, &allocation, nullptr, &result.memory),
                  "vkAllocateMemory");
            Check(vkBindImageMemory(
                      device, result.image, result.memory, 0),
                  "vkBindImageMemory");
            result.view = CreateImageView(result.image, format, aspect);
        } catch (...) {
            if (result.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, result.view, nullptr);
            }
            if (result.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, result.memory, nullptr);
            }
            vkDestroyImage(device, result.image, nullptr);
            throw;
        }
        return result;
    }

    void DestroyImage(Image& image)
    {
        if (image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, image.view, nullptr);
        }
        if (image.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image.image, nullptr);
        }
        if (image.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, image.memory, nullptr);
        }
        image = {};
    }

    void CreateDepthResources()
    {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (HasStencil(depthFormat)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        depthImages.reserve(swapchainImages.size());
        for (std::size_t index = 0;
             index < swapchainImages.size(); ++index) {
            depthImages.push_back(CreateImage(
                swapchainExtent.width,
                swapchainExtent.height,
                depthFormat,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                aspect));
        }
    }

    void CreateFramebuffers()
    {
        framebuffers.reserve(swapchainImageViews.size());
        for (std::size_t index = 0;
             index < swapchainImageViews.size(); ++index) {
            const VkImageView attachments[] = {
                swapchainImageViews[index], depthImages[index].view};
            VkFramebufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            createInfo.renderPass = renderPass;
            createInfo.attachmentCount = 2;
            createInfo.pAttachments = attachments;
            createInfo.width = swapchainExtent.width;
            createInfo.height = swapchainExtent.height;
            createInfo.layers = 1;

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            Check(vkCreateFramebuffer(
                      device, &createInfo, nullptr, &framebuffer),
                  "vkCreateFramebuffer");
            framebuffers.push_back(framebuffer);
        }
    }

    Buffer UploadBuffer(const void* data,
                        VkDeviceSize size,
                        VkBufferUsageFlags destinationUsage)
    {
        auto staging = CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        try {
            void* mapped = nullptr;
            Check(vkMapMemory(
                      device, staging.memory, 0, size, 0, &mapped),
                  "vkMapMemory");
            std::memcpy(mapped, data, static_cast<std::size_t>(size));
            vkUnmapMemory(device, staging.memory);

            auto destination = CreateBuffer(
                size,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | destinationUsage,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            try {
                CopyBuffer(staging, destination, size);
            } catch (...) {
                DestroyBuffer(destination);
                throw;
            }
            DestroyBuffer(staging);
            return destination;
        } catch (...) {
            DestroyBuffer(staging);
            throw;
        }
    }

    void CreateModelBuffers()
    {
        std::vector<Vertex> vertices(model->geometry.positions.size());
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            const auto& position = model->geometry.positions[index];
            vertices[index].position[0] = position.x;
            vertices[index].position[1] = position.y;
            vertices[index].position[2] = position.z;
            if (index < model->geometry.texCoords0.size()) {
                const auto& uv = model->geometry.texCoords0[index];
                vertices[index].texCoord[0] = uv.x;
                vertices[index].texCoord[1] = uv.y;
            }
        }

        vertexBuffer = UploadBuffer(
            vertices.data(),
            sizeof(Vertex) * vertices.size(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        indexBuffer = UploadBuffer(
            model->geometry.indices.data(),
            sizeof(std::uint32_t) * model->geometry.indices.size(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    std::vector<std::uint8_t> ConvertTextureToRgba8(
        const vpgloader::Texture& texture,
        std::uint32_t& width,
        std::uint32_t& height) const
    {
        const auto& info = texture.info();
        if (info.format.componentType !=
                vpgloader::TextureComponentType::UInt8 ||
            info.format.channels < 1 || info.format.channels > 4 ||
            info.depth != 1 || info.width == 0 || info.height == 0) {
            throw std::runtime_error(
                "Viewer supports only 2D UInt8 textures with 1-4 channels");
        }

        width = info.width;
        height = info.height;
        std::size_t sourceOffset = 0;
        std::size_t sourceSize = texture.byteSize();
        if (!info.mipLevels.empty()) {
            width = info.mipLevels.front().width;
            height = info.mipLevels.front().height;
            sourceOffset = info.mipLevels.front().byteOffset;
            sourceSize = info.mipLevels.front().byteSize;
        }

        const std::size_t pixelCount =
            static_cast<std::size_t>(width) * height;
        const std::size_t channels = info.format.channels;
        const std::size_t required = pixelCount * channels;
        if (sourceSize < required ||
            sourceOffset + required > texture.byteSize()) {
            throw std::runtime_error(
                "Texture base mip data is smaller than its dimensions");
        }

        const auto* source = texture.data() + sourceOffset;
        std::vector<std::uint8_t> rgba(pixelCount * 4);
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const auto* input = source + pixel * channels;
            auto* output = rgba.data() + pixel * 4;
            switch (channels) {
            case 1:
                output[0] = input[0];
                output[1] = input[0];
                output[2] = input[0];
                output[3] = 255;
                break;
            case 2:
                output[0] = input[0];
                output[1] = input[0];
                output[2] = input[0];
                output[3] = input[1];
                break;
            case 3:
                output[0] = input[0];
                output[1] = input[1];
                output[2] = input[2];
                output[3] = 255;
                break;
            default:
                std::memcpy(output, input, 4);
                break;
            }
        }
        return rgba;
    }

    void TransitionTexture(VkImage image,
                           VkImageLayout oldLayout,
                           VkImageLayout newLayout)
    {
        const auto command = BeginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags sourceStage = 0;
        VkPipelineStageFlags destinationStage = 0;
        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                   newLayout ==
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            vkFreeCommandBuffers(device, commandPool, 1, &command);
            throw std::runtime_error("Unsupported texture layout transition");
        }

        vkCmdPipelineBarrier(
            command,
            sourceStage,
            destinationStage,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier);
        EndSingleTimeCommands(command);
    }

    std::size_t UploadTexture(const std::vector<std::uint8_t>& rgba,
                              std::uint32_t width,
                              std::uint32_t height,
                              bool srgb)
    {
        const auto size = static_cast<VkDeviceSize>(rgba.size());
        auto staging = CreateBuffer(
            size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        try {
            void* mapped = nullptr;
            Check(vkMapMemory(
                      device, staging.memory, 0, size, 0, &mapped),
                  "vkMapMemory");
            std::memcpy(mapped, rgba.data(), rgba.size());
            vkUnmapMemory(device, staging.memory);

            const auto format = srgb
                ? VK_FORMAT_R8G8B8A8_SRGB
                : VK_FORMAT_R8G8B8A8_UNORM;
            auto textureImage = CreateImage(
                width,
                height,
                format,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT);
            try {
                TransitionTexture(
                    textureImage.image,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                const auto command = BeginSingleTimeCommands();
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {width, height, 1};
                vkCmdCopyBufferToImage(
                    command,
                    staging.buffer,
                    textureImage.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1,
                    &region);
                EndSingleTimeCommands(command);

                TransitionTexture(
                    textureImage.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            } catch (...) {
                DestroyImage(textureImage);
                throw;
            }

            DestroyBuffer(staging);
            gpuTextures.push_back(textureImage);
            return gpuTextures.size() - 1;
        } catch (...) {
            DestroyBuffer(staging);
            throw;
        }
    }

    void CreateTextureResources()
    {
        const std::vector<std::uint8_t> white = {255, 255, 255, 255};
        UploadTexture(white, 1, 1, false);

        textureGpuIndices.assign(model->textures.size(), 0);
        for (std::size_t index = 0; index < model->textures.size(); ++index) {
            const auto& source = model->textures[index];
            if (!source.texture) {
                continue;
            }
            try {
                std::uint32_t width = 0;
                std::uint32_t height = 0;
                auto rgba =
                    ConvertTextureToRgba8(*source.texture, width, height);
                textureGpuIndices[index] =
                    UploadTexture(rgba, width, height, source.isSrgb);
            } catch (const std::exception& error) {
                std::cerr
                    << "Texture warning: " << source.name << ": "
                    << error.what() << "; using white fallback\n";
            }
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.maxLod = 0.0f;
        Check(vkCreateSampler(
                  device, &samplerInfo, nullptr, &textureSampler),
              "vkCreateSampler");

        const std::size_t materialCount =
            std::max<std::size_t>(1, model->materials.size());
        materialTextureIndices.assign(materialCount, 0);
        for (std::size_t materialIndex = 0;
             materialIndex < model->materials.size(); ++materialIndex) {
            const auto textureInfoIndex =
                model->materials[materialIndex].baseColorTexture;
            if (textureInfoIndex >= model->textureInfos.size()) {
                continue;
            }
            const auto textureIndex =
                model->textureInfos[textureInfoIndex].textureIndex;
            if (textureIndex < textureGpuIndices.size()) {
                materialTextureIndices[materialIndex] =
                    textureGpuIndices[textureIndex];
            }
        }
    }

    void CreateUniformBuffers()
    {
        for (std::size_t frame = 0; frame < FramesInFlight; ++frame) {
            uniformBuffers[frame] = CreateBuffer(
                sizeof(CameraData),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            Check(vkMapMemory(
                      device,
                      uniformBuffers[frame].memory,
                      0,
                      sizeof(CameraData),
                      0,
                      &uniformMappings[frame]),
                  "vkMapMemory");
        }
    }

    void CreateDescriptorSets()
    {
        const std::size_t materialCount = materialTextureIndices.size();
        const std::uint32_t setCount = static_cast<std::uint32_t>(
            FramesInFlight * materialCount);

        const VkDescriptorPoolSize poolSizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount},
        };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        Check(vkCreateDescriptorPool(
                  device, &poolInfo, nullptr, &descriptorPool),
              "vkCreateDescriptorPool");

        std::vector<VkDescriptorSetLayout> layouts(
            setCount, descriptorSetLayout);
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = descriptorPool;
        allocation.descriptorSetCount = setCount;
        allocation.pSetLayouts = layouts.data();
        descriptorSets.resize(setCount);
        Check(vkAllocateDescriptorSets(
                  device, &allocation, descriptorSets.data()),
              "vkAllocateDescriptorSets");

        for (std::size_t frame = 0; frame < FramesInFlight; ++frame) {
            for (std::size_t material = 0;
                 material < materialCount; ++material) {
                const auto setIndex = frame * materialCount + material;
                VkDescriptorBufferInfo bufferInfo{};
                bufferInfo.buffer = uniformBuffers[frame].buffer;
                bufferInfo.range = sizeof(CameraData);

                const auto textureIndex =
                    materialTextureIndices[material];
                VkDescriptorImageInfo imageInfo{};
                imageInfo.sampler = textureSampler;
                imageInfo.imageView = gpuTextures[textureIndex].view;
                imageInfo.imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet writes[2]{};
                writes[0].sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = descriptorSets[setIndex];
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType =
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &bufferInfo;
                writes[1].sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = descriptorSets[setIndex];
                writes[1].dstBinding = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &imageInfo;
                vkUpdateDescriptorSets(
                    device, 2, writes, 0, nullptr);
            }
        }
    }

    void CreateCommandBuffers()
    {
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount =
            static_cast<std::uint32_t>(commandBuffers.size());
        Check(vkAllocateCommandBuffers(
                  device, &allocation, commandBuffers.data()),
              "vkAllocateCommandBuffers");
    }

    void CreateSynchronization()
    {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (std::size_t frame = 0; frame < FramesInFlight; ++frame) {
            Check(vkCreateSemaphore(
                      device,
                      &semaphoreInfo,
                      nullptr,
                      &imageAvailable[frame]),
                  "vkCreateSemaphore");
            Check(vkCreateFence(
                      device, &fenceInfo, nullptr, &inFlight[frame]),
                  "vkCreateFence");
        }
        CreatePresentationSemaphores();
    }

    void CreatePresentationSemaphores()
    {
        VkSemaphoreCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        renderFinished.resize(swapchainImages.size(), VK_NULL_HANDLE);
        for (auto& semaphore : renderFinished) {
            Check(vkCreateSemaphore(
                      device, &createInfo, nullptr, &semaphore),
                  "vkCreateSemaphore");
        }
    }

    void UpdateCamera(float elapsedSeconds)
    {
        const float distance = orbitRadius * 2.8f;
        const float angle = elapsedSeconds * 0.35f;
        const vpgloader::Float3 eye = {
            orbitCenter.x + std::sin(angle) * distance,
            orbitCenter.y + orbitRadius * 0.65f,
            orbitCenter.z + std::cos(angle) * distance,
        };
        const float nearPlane = std::max(0.001f, orbitRadius * 0.01f);
        const float farPlane = std::max(
            nearPlane + 1.0f, distance + orbitRadius * 4.0f);
        const auto view = LookAt(eye, orbitCenter);
        const auto projection = Perspective(
            45.0f * Pi / 180.0f,
            static_cast<float>(swapchainExtent.width) /
                static_cast<float>(swapchainExtent.height),
            nearPlane,
            farPlane);

        CameraData camera;
        camera.viewProjection =
            vpgloader::Multiply(projection, view);
        std::memcpy(
            uniformMappings[currentFrame], &camera, sizeof(camera));
    }

    const vpgloader::ModelMaterial& MaterialFor(
        std::uint32_t materialIndex) const
    {
        static const vpgloader::ModelMaterial fallback;
        if (materialIndex < model->materials.size()) {
            return model->materials[materialIndex];
        }
        return fallback;
    }

    void RecordCommandBuffer(VkCommandBuffer command,
                             std::uint32_t imageIndex)
    {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        Check(vkBeginCommandBuffer(command, &begin),
              "vkBeginCommandBuffer");

        VkClearValue clears[2]{};
        clears[0].color = {{0.025f, 0.03f, 0.04f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType =
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = framebuffers[imageIndex];
        renderPassInfo.renderArea.extent = swapchainExtent;
        renderPassInfo.clearValueCount = 2;
        renderPassInfo.pClearValues = clears;
        vkCmdBeginRenderPass(
            command, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchainExtent.width);
        viewport.height = static_cast<float>(swapchainExtent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(command, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.extent = swapchainExtent;
        vkCmdSetScissor(command, 0, 1, &scissor);

        constexpr VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(
            command, 0, 1, &vertexBuffer.buffer, &vertexOffset);
        vkCmdBindIndexBuffer(
            command, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        const auto materialCount = materialTextureIndices.size();
        for (const auto& item : drawItems) {
            const auto& mesh = model->meshes[item.meshIndex];
            const std::size_t materialIndex =
                item.materialIndex < materialCount
                ? item.materialIndex
                : 0;
            const auto descriptorIndex =
                currentFrame * materialCount + materialIndex;
            vkCmdBindDescriptorSets(
                command,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout,
                0,
                1,
                &descriptorSets[descriptorIndex],
                0,
                nullptr);

            const auto& material =
                MaterialFor(item.materialIndex);
            DrawData push;
            push.model = item.model;
            push.baseColorFactor = material.baseColorFactor;
            push.alphaCutoff = material.alphaCutoff;
            push.alphaMode = ToAlphaMode(material.alphaMode);
            vkCmdPushConstants(
                command,
                pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT |
                    VK_SHADER_STAGE_FRAGMENT_BIT,
                0,
                sizeof(push),
                &push);
            vkCmdDrawIndexed(
                command,
                mesh.indexCount,
                1,
                mesh.firstIndex,
                static_cast<std::int32_t>(mesh.firstVertex),
                0);
        }

        vkCmdEndRenderPass(command);
        Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
    }

    void DrawFrame(float elapsedSeconds)
    {
        Check(vkWaitForFences(
                  device,
                  1,
                  &inFlight[currentFrame],
                  VK_TRUE,
                  std::numeric_limits<std::uint64_t>::max()),
              "vkWaitForFences");

        std::uint32_t imageIndex = 0;
        const auto acquire = vkAcquireNextImageKHR(
            device,
            swapchain,
            std::numeric_limits<std::uint64_t>::max(),
            imageAvailable[currentFrame],
            VK_NULL_HANDLE,
            &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            RecreateSwapchain();
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            Check(acquire, "vkAcquireNextImageKHR");
        }

        if (imageFences[imageIndex] != VK_NULL_HANDLE) {
            Check(vkWaitForFences(
                      device,
                      1,
                      &imageFences[imageIndex],
                      VK_TRUE,
                      std::numeric_limits<std::uint64_t>::max()),
                  "vkWaitForFences");
        }
        imageFences[imageIndex] = inFlight[currentFrame];

        UpdateCamera(elapsedSeconds);
        Check(vkResetFences(device, 1, &inFlight[currentFrame]),
              "vkResetFences");
        Check(vkResetCommandBuffer(
                  commandBuffers[currentFrame], 0),
              "vkResetCommandBuffer");
        RecordCommandBuffer(commandBuffers[currentFrame], imageIndex);

        const VkPipelineStageFlags waitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageAvailable[currentFrame];
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffers[currentFrame];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderFinished[imageIndex];
        Check(vkQueueSubmit(
                  graphicsQueue,
                  1,
                  &submit,
                  inFlight[currentFrame]),
              "vkQueueSubmit");

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderFinished[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const auto presentation =
            vkQueuePresentKHR(presentQueue, &present);
        if (presentation == VK_ERROR_OUT_OF_DATE_KHR ||
            presentation == VK_SUBOPTIMAL_KHR ||
            framebufferResized) {
            framebufferResized = false;
            RecreateSwapchain();
        } else if (presentation != VK_SUCCESS) {
            Check(presentation, "vkQueuePresentKHR");
        }

        currentFrame = (currentFrame + 1) % FramesInFlight;
    }

    void CleanupSwapchain()
    {
        for (const auto semaphore : renderFinished) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        renderFinished.clear();
        for (const auto framebuffer : framebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        framebuffers.clear();
        for (auto& depthImage : depthImages) {
            DestroyImage(depthImage);
        }
        depthImages.clear();
        if (graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
            graphicsPipeline = VK_NULL_HANDLE;
        }
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
        for (const auto view : swapchainImageViews) {
            vkDestroyImageView(device, view, nullptr);
        }
        swapchainImageViews.clear();
        swapchainImages.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    void RecreateSwapchain()
    {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window, &width, &height);
        }

        Check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
        CleanupSwapchain();
        CreateSwapchain();
        CreatePresentationSemaphores();
        CreateSwapchainImageViews();
        CreateRenderPass();
        CreateGraphicsPipeline();
        CreateDepthResources();
        CreateFramebuffers();
    }

    void Cleanup()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);

            for (std::size_t frame = 0;
                 frame < FramesInFlight; ++frame) {
                if (imageAvailable[frame] != VK_NULL_HANDLE) {
                    vkDestroySemaphore(
                        device, imageAvailable[frame], nullptr);
                }
                if (inFlight[frame] != VK_NULL_HANDLE) {
                    vkDestroyFence(device, inFlight[frame], nullptr);
                }
            }

            if (descriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            }
            for (std::size_t frame = 0;
                 frame < FramesInFlight; ++frame) {
                if (uniformMappings[frame] != nullptr &&
                    uniformBuffers[frame].memory != VK_NULL_HANDLE) {
                    vkUnmapMemory(device, uniformBuffers[frame].memory);
                }
                DestroyBuffer(uniformBuffers[frame]);
            }
            if (textureSampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, textureSampler, nullptr);
            }
            for (auto& texture : gpuTextures) {
                DestroyImage(texture);
            }
            gpuTextures.clear();
            DestroyBuffer(indexBuffer);
            DestroyBuffer(vertexBuffer);

            CleanupSwapchain();
            if (pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            }
            if (descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(
                    device, descriptorSetLayout, nullptr);
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }

        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (window) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfwInitialized) {
            glfwTerminate();
            glfwInitialized = false;
        }
    }

    std::filesystem::path modelPath;
    std::uint64_t frameLimit = 0;
    vpgloader::ModelHandle model;
    std::vector<DrawItem> drawItems;
    vpgloader::Float3 orbitCenter;
    float orbitRadius = 1.0f;

    GLFWwindow* window = nullptr;
    bool glfwInitialized = false;
    bool framebufferResized = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    QueueFamilies queueFamilies;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<Image> depthImages;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkFramebuffer> framebuffers;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, FramesInFlight> commandBuffers{};
    Buffer vertexBuffer;
    Buffer indexBuffer;
    std::vector<Image> gpuTextures;
    std::vector<std::size_t> textureGpuIndices;
    std::vector<std::size_t> materialTextureIndices;
    VkSampler textureSampler = VK_NULL_HANDLE;
    std::array<Buffer, FramesInFlight> uniformBuffers{};
    std::array<void*, FramesInFlight> uniformMappings{};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets;

    std::array<VkSemaphore, FramesInFlight> imageAvailable{};
    std::vector<VkSemaphore> renderFinished;
    std::array<VkFence, FramesInFlight> inFlight{};
    std::vector<VkFence> imageFences;
    std::size_t currentFrame = 0;
};

VulkanViewer::VulkanViewer(std::filesystem::path modelPath,
                           std::uint64_t frameLimit)
    : impl_(std::make_unique<Impl>(
          std::move(modelPath), frameLimit))
{
}

VulkanViewer::~VulkanViewer() = default;

int VulkanViewer::Run()
{
    return impl_->Run();
}
