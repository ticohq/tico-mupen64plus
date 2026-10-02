/// @file TicoVulkan.cpp
/// @brief Vulkan swapchain + libretro hw_render interface for paraLLEl-RDP.

#include "TicoVulkan.h"
#include "TicoLogger.h"

#include "imgui_impl_vulkan.h"
#include "volk.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
extern "C" {
PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion);
}
#endif

namespace TicoVulkan
{
namespace
{

constexpr const char *TAG = "VK";

#define VK_LOG_INFO(fmt, ...) LOG_INFO(TAG, fmt, ##__VA_ARGS__)
#define VK_LOG_WARN(fmt, ...) LOG_WARN(TAG, fmt, ##__VA_ARGS__)
#define VK_LOG_ERROR(fmt, ...) LOG_ERROR(TAG, fmt, ##__VA_ARGS__)

struct PerFrame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence inflightFence = VK_NULL_HANDLE;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderSemaphore = VK_NULL_HANDLE;
    retro_vulkan_image image = {};
    bool imageValid = false;
    std::vector<VkCommandBuffer> coreCommandBuffers;
    VkSemaphore signalSemaphore = VK_NULL_HANDLE;
};

struct OverlayTextureResource
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

VkInstance s_instance = VK_NULL_HANDLE;
VkPhysicalDevice s_gpu = VK_NULL_HANDLE;
VkDevice s_device = VK_NULL_HANDLE;
VkQueue s_queue = VK_NULL_HANDLE;
VkQueue s_presentQueue = VK_NULL_HANDLE;
uint32_t s_queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
uint32_t s_presentQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

VkSurfaceKHR s_surface = VK_NULL_HANDLE;
VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
VkFormat s_swapFormat = VK_FORMAT_UNDEFINED;
VkExtent2D s_swapExtent = {};
VkExtent2D s_sourceExtent = {};
std::vector<VkImage> s_swapImages;
std::vector<VkImageView> s_swapImageViews;
VkRenderPass s_overlayRenderPass = VK_NULL_HANDLE;
std::vector<VkFramebuffer> s_overlayFramebuffers;

VkCommandPool s_commandPool = VK_NULL_HANDLE;
std::vector<PerFrame> s_frames;
uint32_t s_currentFrame = 0;
// Frame being recorded by BeginFrame/EndFrame. Equal to s_currentFrame unless the
// emu thread advances s_currentFrame itself (standalone).
uint32_t s_presentFrame = 0;
bool s_coreOwnsFrameIndex = false;
uint32_t s_currentImage = 0;
bool s_frameInFlight = false;
bool s_ready = false;
bool s_overlayReady = false;
VkDescriptorPool s_overlayDescriptorPool = VK_NULL_HANDLE;
ImDrawData *s_overlayDrawData = nullptr;

// Written by set_image on the core's thread, read by the present (another thread in standalone).
std::mutex s_lastImageMutex;
retro_vulkan_image s_lastImage = {};
bool s_lastImageValid = false;
uint32_t s_lastImageFrame = 0;
std::vector<OverlayTextureResource> s_overlayTextures;

const retro_hw_render_context_negotiation_interface_vulkan *s_negIface = nullptr;
retro_hw_render_interface_vulkan s_hwIface = {};
// paraLLEl-RDP drives lock_queue/unlock_queue on the SAME cooperative (libco) thread
// that also runs EndFrame. On libnx a re-entrant std::mutex self-deadlocks: mutexLock()
// has no self-owner check, so the second lock calls svcArbitrateLock() waiting on the
// owning thread — which is itself — and hangs forever. recursive_mutex makes re-entry
// safe; the owner tracking below logs whether/where recursion actually happens so we can
// confirm and, if desired, remove the underlying re-entrancy later.
std::recursive_mutex s_queueMutex;
std::atomic<uint64_t> s_queueOwner{0};
int s_queueDepth = 0;
uint64_t s_queueRecursionLogs = 0;

inline uint64_t CurrentThreadId()
{
    return (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
}

void QueueLockImpl()
{
    const uint64_t self = CurrentThreadId();
    if (s_queueOwner.load(std::memory_order_relaxed) == self)
    {
        if (s_queueRecursionLogs < 32)
        {
            VK_LOG_WARN("RECURSIVE queue lock on same thread (new depth=%d) — a plain std::mutex self-deadlocks on libnx here",
                        s_queueDepth + 1);
            s_queueRecursionLogs++;
        }
    }
    s_queueMutex.lock();
    s_queueOwner.store(self, std::memory_order_relaxed);
    s_queueDepth++;
}

void QueueUnlockImpl()
{
    if (--s_queueDepth == 0)
        s_queueOwner.store(0, std::memory_order_relaxed);
    s_queueMutex.unlock();
}

struct QueueLockGuard
{
    QueueLockGuard() { QueueLockImpl(); }
    ~QueueLockGuard() { QueueUnlockImpl(); }
    QueueLockGuard(const QueueLockGuard &) = delete;
    QueueLockGuard &operator=(const QueueLockGuard &) = delete;
};

uint64_t s_setImageCount = 0;
uint64_t s_waitSyncCount = 0;
uint64_t s_beginFrameCount = 0;
uint64_t s_setCommandBufferCount = 0;
uint64_t s_signalSemaphoreCount = 0;
uint64_t s_presentCount = 0;
uint64_t s_emptySourceFrames = 0;

bool Check(VkResult result, const char *what)
{
    if (result == VK_SUCCESS)
        return true;
    VK_LOG_ERROR("%s failed: %d", what, (int)result);
    return false;
}

PFN_vkGetInstanceProcAddr GetInstanceProcAddrFunc()
{
#if defined(__SWITCH__)
    uint32_t icdVersion = 5;
    vk_icdNegotiateLoaderICDInterfaceVersion(&icdVersion);
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(&vk_icdGetInstanceProcAddr);
#else
    return vkGetInstanceProcAddr;
#endif
}

PFN_vkVoidFunction ImGuiVulkanLoader(const char *functionName, void *)
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    return getInstProcAddr ? getInstProcAddr(s_instance, functionName) : nullptr;
}

void TransitionLayout(VkCommandBuffer cmd, VkImage image,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t &typeIndex)
{
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    vkGetPhysicalDeviceMemoryProperties(s_gpu, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            typeIndex = i;
            return true;
        }
    }
    return false;
}

void DestroyOverlayTextureResource(OverlayTextureResource &texture, bool removeDescriptor)
{
    if (removeDescriptor && s_overlayReady && texture.descriptor != VK_NULL_HANDLE)
        ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
    texture.descriptor = VK_NULL_HANDLE;

    if (texture.sampler) vkDestroySampler(s_device, texture.sampler, nullptr);
    if (texture.view) vkDestroyImageView(s_device, texture.view, nullptr);
    if (texture.image) vkDestroyImage(s_device, texture.image, nullptr);
    if (texture.memory) vkFreeMemory(s_device, texture.memory, nullptr);
    texture = {};
}

void DestroyOverlayTextureResources()
{
    if (!s_device)
    {
        s_overlayTextures.clear();
        return;
    }

    vkDeviceWaitIdle(s_device);
    for (auto &texture : s_overlayTextures)
        DestroyOverlayTextureResource(texture, true);
    s_overlayTextures.clear();
}

void DestroyOverlayRenderTargets()
{
    for (VkFramebuffer framebuffer : s_overlayFramebuffers)
        if (framebuffer) vkDestroyFramebuffer(s_device, framebuffer, nullptr);
    s_overlayFramebuffers.clear();

    if (s_overlayRenderPass)
        vkDestroyRenderPass(s_device, s_overlayRenderPass, nullptr);
    s_overlayRenderPass = VK_NULL_HANDLE;
}

void ShutdownOverlayRendererInternal()
{
    DestroyOverlayTextureResources();

    if (s_overlayReady)
    {
        ImGui_ImplVulkan_Shutdown();
        s_overlayReady = false;
    }

    if (s_overlayDescriptorPool)
    {
        vkDestroyDescriptorPool(s_device, s_overlayDescriptorPool, nullptr);
        s_overlayDescriptorPool = VK_NULL_HANDLE;
    }

    s_overlayDrawData = nullptr;
}

bool CreateOverlayRenderTargets()
{
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = s_swapFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &colorAttachment;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    if (!Check(vkCreateRenderPass(s_device, &rpci, nullptr, &s_overlayRenderPass), "vkCreateRenderPass"))
        return false;

    s_overlayFramebuffers.resize(s_swapImageViews.size());
    for (size_t i = 0; i < s_swapImageViews.size(); ++i)
    {
        VkImageView attachment = s_swapImageViews[i];
        VkFramebufferCreateInfo fbci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = s_overlayRenderPass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &attachment;
        fbci.width = s_swapExtent.width;
        fbci.height = s_swapExtent.height;
        fbci.layers = 1;
        if (!Check(vkCreateFramebuffer(s_device, &fbci, nullptr, &s_overlayFramebuffers[i]), "vkCreateFramebuffer"))
            return false;
    }

    return true;
}

void RETRO_CALLCONV cb_set_image(void *, const retro_vulkan_image *image,
                                 uint32_t numSemaphores, const VkSemaphore *, uint32_t srcQueueFamily)
{
    if (!image || s_frames.empty())
        return;

    s_setImageCount++;
    if (s_setImageCount <= 8 || (s_setImageCount % 120) == 0)
    {
        VK_LOG_INFO("set_image #%llu frame=%u image=0x%llx view=0x%llx layout=%d semaphores=%u src_q=%u",
                    (unsigned long long)s_setImageCount,
                    s_currentFrame,
                    (unsigned long long)(uintptr_t)image->create_info.image,
                    (unsigned long long)(uintptr_t)image->image_view,
                    (int)image->image_layout,
                    numSemaphores,
                    srcQueueFamily);
    }

    PerFrame &f = s_frames[s_currentFrame];
    f.image = *image;
    f.imageValid = true;
    std::lock_guard<std::mutex> lastImageLock(s_lastImageMutex);
    s_lastImage = *image;
    s_lastImageValid = image->create_info.image != VK_NULL_HANDLE;
    s_lastImageFrame = s_currentFrame;
}

uint32_t RETRO_CALLCONV cb_get_sync_index(void *)
{
    return s_currentFrame;
}

uint32_t RETRO_CALLCONV cb_get_sync_index_mask(void *)
{
    return s_frames.empty() ? 1u : ((1u << s_frames.size()) - 1u);
}

void RETRO_CALLCONV cb_set_command_buffers(void *, uint32_t num_cmd, const VkCommandBuffer *cmd)
{
    if (s_frames.empty())
        return;

    s_setCommandBufferCount++;
    if (s_setCommandBufferCount <= 8 || (s_setCommandBufferCount % 120) == 0)
    {
        VK_LOG_INFO("set_command_buffers #%llu frame=%u count=%u",
                    (unsigned long long)s_setCommandBufferCount,
                    s_currentFrame,
                    num_cmd);
    }

    PerFrame &f = s_frames[s_currentFrame];
    f.coreCommandBuffers.assign(cmd, cmd + num_cmd);
}

void RETRO_CALLCONV cb_wait_sync_index(void *)
{
    if (s_frames.empty() || !s_device)
        return;
    s_waitSyncCount++;
    PerFrame &f = s_frames[s_currentFrame];
    const bool logThis = s_waitSyncCount <= 8;
    if (logThis)
        VK_LOG_INFO("wait_sync_index #%llu enter idx=%u fence=0x%llx",
                    (unsigned long long)s_waitSyncCount, s_currentFrame,
                    (unsigned long long)(uintptr_t)f.inflightFence);
    if (f.inflightFence)
        vkWaitForFences(s_device, 1, &f.inflightFence, VK_TRUE, UINT64_MAX);
    if (logThis)
        VK_LOG_INFO("wait_sync_index #%llu exit", (unsigned long long)s_waitSyncCount);
}

void RETRO_CALLCONV cb_lock_queue(void *)
{
    QueueLockImpl();
}

void RETRO_CALLCONV cb_unlock_queue(void *)
{
    QueueUnlockImpl();
}

void RETRO_CALLCONV cb_set_signal_semaphore(void *, VkSemaphore semaphore)
{
    if (s_frames.empty())
        return;
    s_signalSemaphoreCount++;
    if (s_signalSemaphoreCount <= 8 || (s_signalSemaphoreCount % 120) == 0)
    {
        VK_LOG_INFO("set_signal_semaphore #%llu frame=%u semaphore=0x%llx",
                    (unsigned long long)s_signalSemaphoreCount,
                    s_currentFrame,
                    (unsigned long long)(uintptr_t)semaphore);
    }
    s_frames[s_currentFrame].signalSemaphore = semaphore;
}

bool CreateInstanceInternal()
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    if (!getInstProcAddr)
    {
        VK_LOG_ERROR("vkGetInstanceProcAddr unavailable");
        return false;
    }

    volkInitializeCustom(getInstProcAddr);

    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "tico-mupen64plus";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "Tico";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char *extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(__SWITCH__) && defined(VK_NN_VI_SURFACE_EXTENSION_NAME)
        VK_NN_VI_SURFACE_EXTENSION_NAME,
#endif
    };

    VkInstanceCreateInfo createInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    createInfo.ppEnabledExtensionNames = extensions;

    if (!Check(vkCreateInstance(&createInfo, nullptr, &s_instance), "vkCreateInstance"))
        return false;

    volkLoadInstance(s_instance);
    VK_LOG_INFO("VkInstance created");
    return true;
}

bool CreateSurfaceInternal()
{
#if defined(__SWITCH__)
    VkViSurfaceCreateInfoNN createInfo = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    createInfo.window = nwindowGetDefault();
    if (!Check(vkCreateViSurfaceNN(s_instance, &createInfo, nullptr, &s_surface), "vkCreateViSurfaceNN"))
        return false;
    VK_LOG_INFO("VkSurfaceKHR created via VK_NN_vi_surface");
    return true;
#else
    VK_LOG_ERROR("Surface creation is only implemented for Switch");
    return false;
#endif
}

bool CreateFallbackDevice()
{
    uint32_t gpuCount = 0;
    if (!Check(vkEnumeratePhysicalDevices(s_instance, &gpuCount, nullptr), "vkEnumeratePhysicalDevices") ||
        gpuCount == 0)
        return false;

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    if (!Check(vkEnumeratePhysicalDevices(s_instance, &gpuCount, gpus.data()), "vkEnumeratePhysicalDevices"))
        return false;
    s_gpu = gpus[0];

    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProps(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(s_gpu, &queueCount, queueProps.data());

    for (uint32_t i = 0; i < queueCount; ++i)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(s_gpu, i, s_surface, &present);
        if ((queueProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
        {
            s_queueFamilyIndex = i;
            s_presentQueueFamilyIndex = i;
            break;
        }
    }

    if (s_queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED)
    {
        VK_LOG_ERROR("No graphics+present queue family");
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = s_queueFamilyIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = deviceExtensions;

    if (!Check(vkCreateDevice(s_gpu, &dci, nullptr, &s_device), "vkCreateDevice"))
        return false;

    volkLoadDevice(s_device);
    vkGetDeviceQueue(s_device, s_queueFamilyIndex, 0, &s_queue);
    s_presentQueue = s_queue;
    VK_LOG_INFO("Device created via fallback path (qfi=%u)", s_queueFamilyIndex);
    return true;
}

bool CreateDeviceInternal()
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    if (s_negIface && s_negIface->create_device)
    {
        VK_LOG_INFO("Creating Vulkan device via core negotiation interface");
        retro_vulkan_context ctx = {};
        VkPhysicalDeviceFeatures requiredFeatures = {};
        const char *requiredDeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const bool ok = s_negIface->create_device(&ctx,
                                                  s_instance,
                                                  VK_NULL_HANDLE,
                                                  s_surface,
                                                  getInstProcAddr,
                                                  requiredDeviceExtensions, 1,
                                                  nullptr, 0,
                                                  &requiredFeatures);
        if (!ok)
        {
            VK_LOG_ERROR("Core create_device returned false");
            return false;
        }

        s_gpu = ctx.gpu;
        s_device = ctx.device;
        s_queue = ctx.queue;
        s_queueFamilyIndex = ctx.queue_family_index;
        s_presentQueue = ctx.presentation_queue ? ctx.presentation_queue : ctx.queue;
        s_presentQueueFamilyIndex = ctx.presentation_queue_family_index != VK_QUEUE_FAMILY_IGNORED
                                        ? ctx.presentation_queue_family_index
                                        : ctx.queue_family_index;
        volkLoadDevice(s_device);
        VK_LOG_INFO("Device created via core negotiation gpu=0x%llx device=0x%llx queue=0x%llx qfi=%u present=%u",
                    (unsigned long long)(uintptr_t)s_gpu,
                    (unsigned long long)(uintptr_t)s_device,
                    (unsigned long long)(uintptr_t)s_queue,
                    s_queueFamilyIndex,
                    s_presentQueueFamilyIndex);
        return true;
    }

    VK_LOG_WARN("No core Vulkan negotiation interface; using fallback device path");
    return CreateFallbackDevice();
}

bool CreateSwapchainInternal()
{
    VkSurfaceCapabilitiesKHR caps = {};
    if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_gpu, s_surface, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_gpu, s_surface, &formatCount, nullptr);
    if (formatCount == 0)
    {
        VK_LOG_ERROR("No surface formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_gpu, s_surface, &formatCount, formats.data());

    VkSurfaceFormatKHR format = formats[0];
    for (const auto &candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
            candidate.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            format = candidate;
            break;
        }
    }
    s_swapFormat = format.format;
    s_swapExtent = caps.currentExtent;
    if (s_swapExtent.width == UINT32_MAX)
    {
        s_swapExtent.width = std::max(caps.minImageExtent.width,
                                      std::min(caps.maxImageExtent.width, 1280u));
        s_swapExtent.height = std::max(caps.minImageExtent.height,
                                       std::min(caps.maxImageExtent.height, 720u));
    }

    uint32_t imageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount != 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = s_surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = s_swapExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;

    uint32_t queueFamilies[] = {s_queueFamilyIndex, s_presentQueueFamilyIndex};
    if (s_queueFamilyIndex != s_presentQueueFamilyIndex)
    {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = queueFamilies;
    }
    else
    {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (!Check(vkCreateSwapchainKHR(s_device, &sci, nullptr, &s_swapchain), "vkCreateSwapchainKHR"))
        return false;

    uint32_t swapImageCount = 0;
    vkGetSwapchainImagesKHR(s_device, s_swapchain, &swapImageCount, nullptr);
    s_swapImages.resize(swapImageCount);
    vkGetSwapchainImagesKHR(s_device, s_swapchain, &swapImageCount, s_swapImages.data());

    s_swapImageViews.resize(s_swapImages.size());
    for (size_t i = 0; i < s_swapImages.size(); ++i)
    {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s_swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s_swapFormat;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (!Check(vkCreateImageView(s_device, &vci, nullptr, &s_swapImageViews[i]), "vkCreateImageView"))
            return false;
    }

    VK_LOG_INFO("Swapchain: %ux%u format=%d images=%zu",
                s_swapExtent.width, s_swapExtent.height, (int)s_swapFormat, s_swapImages.size());
    return true;
}

bool CreateFrameResources()
{
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = s_queueFamilyIndex;
    if (!Check(vkCreateCommandPool(s_device, &cpci, nullptr, &s_commandPool), "vkCreateCommandPool"))
        return false;

    const uint32_t frameCount = (uint32_t)s_swapImages.size();
    s_frames.resize(frameCount);

    std::vector<VkCommandBuffer> commandBuffers(frameCount);
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = s_commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = frameCount;
    if (!Check(vkAllocateCommandBuffers(s_device, &cbai, commandBuffers.data()), "vkAllocateCommandBuffers"))
        return false;

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        PerFrame &frame = s_frames[i];
        frame.cmd = commandBuffers[i];

        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Check(vkCreateFence(s_device, &fci, nullptr, &frame.inflightFence), "vkCreateFence"))
            return false;

        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!Check(vkCreateSemaphore(s_device, &sci, nullptr, &frame.acquireSemaphore), "vkCreateSemaphore") ||
            !Check(vkCreateSemaphore(s_device, &sci, nullptr, &frame.renderSemaphore), "vkCreateSemaphore"))
            return false;
    }

    return true;
}

void PopulateHwInterface()
{
    std::memset(&s_hwIface, 0, sizeof(s_hwIface));
    s_hwIface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    s_hwIface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    s_hwIface.handle = nullptr;
    s_hwIface.instance = s_instance;
    s_hwIface.gpu = s_gpu;
    s_hwIface.device = s_device;
    s_hwIface.queue = s_queue;
    s_hwIface.queue_index = s_queueFamilyIndex;
    s_hwIface.get_instance_proc_addr = GetInstanceProcAddrFunc();
    s_hwIface.get_device_proc_addr =
        s_hwIface.get_instance_proc_addr
            ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                  s_hwIface.get_instance_proc_addr(s_instance, "vkGetDeviceProcAddr"))
            : nullptr;
    s_hwIface.set_image = &cb_set_image;
    s_hwIface.get_sync_index = &cb_get_sync_index;
    s_hwIface.get_sync_index_mask = &cb_get_sync_index_mask;
    s_hwIface.set_command_buffers = &cb_set_command_buffers;
    s_hwIface.wait_sync_index = &cb_wait_sync_index;
    s_hwIface.lock_queue = &cb_lock_queue;
    s_hwIface.unlock_queue = &cb_unlock_queue;
    s_hwIface.set_signal_semaphore = &cb_set_signal_semaphore;
}

} // namespace

bool CreateInstance()
{
    if (s_instance)
        return true;
    return CreateInstanceInternal() && CreateSurfaceInternal();
}

bool CreateDeviceAndSwapchain()
{
    if (s_ready)
        return true;
    VK_LOG_INFO("CreateDeviceAndSwapchain starting");
    if (!s_instance && !CreateInstance())
        return false;
    if (!CreateDeviceInternal())
        return false;
    if (!CreateSwapchainInternal())
        return false;
    if (!CreateOverlayRenderTargets())
        return false;
    if (!CreateFrameResources())
        return false;
    PopulateHwInterface();
    s_ready = true;
    VK_LOG_INFO("CreateDeviceAndSwapchain complete");
    return true;
}

void Shutdown()
{
    if (s_device)
        vkDeviceWaitIdle(s_device);

    ShutdownOverlayRendererInternal();

    if (s_negIface && s_negIface->destroy_device)
        s_negIface->destroy_device();
    s_negIface = nullptr;

    for (auto &frame : s_frames)
    {
        if (frame.inflightFence) vkDestroyFence(s_device, frame.inflightFence, nullptr);
        if (frame.acquireSemaphore) vkDestroySemaphore(s_device, frame.acquireSemaphore, nullptr);
        if (frame.renderSemaphore) vkDestroySemaphore(s_device, frame.renderSemaphore, nullptr);
    }
    s_frames.clear();

    if (s_commandPool) vkDestroyCommandPool(s_device, s_commandPool, nullptr);
    s_commandPool = VK_NULL_HANDLE;

    DestroyOverlayRenderTargets();

    for (VkImageView view : s_swapImageViews)
        if (view) vkDestroyImageView(s_device, view, nullptr);
    s_swapImageViews.clear();
    s_swapImages.clear();

    if (s_swapchain) vkDestroySwapchainKHR(s_device, s_swapchain, nullptr);
    s_swapchain = VK_NULL_HANDLE;

    if (s_device) vkDestroyDevice(s_device, nullptr);
    s_device = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    s_presentQueue = VK_NULL_HANDLE;

    if (s_surface) vkDestroySurfaceKHR(s_instance, s_surface, nullptr);
    s_surface = VK_NULL_HANDLE;

    if (s_instance) vkDestroyInstance(s_instance, nullptr);
    s_instance = VK_NULL_HANDLE;
    s_gpu = VK_NULL_HANDLE;

    s_ready = false;
    s_frameInFlight = false;
    s_lastImageValid = false;
}

bool BeginFrame()
{
    return BeginFrameAt(s_currentFrame);
}

uint32_t AdvanceCoreFrame()
{
    s_coreOwnsFrameIndex = true;
    if (s_frames.empty())
        return 0;
    const uint32_t done = s_currentFrame;
    s_currentFrame = (s_currentFrame + 1) % (uint32_t)s_frames.size();
    return done;
}

bool BeginFrameAt(uint32_t frameIndex)
{
    if (!s_ready || s_frames.empty())
    {
        static bool loggedNotReady = false;
        if (!loggedNotReady)
        {
            VK_LOG_WARN("BeginFrame skipped: ready=%d frame_count=%zu",
                        s_ready ? 1 : 0,
                        s_frames.size());
            loggedNotReady = true;
        }
        return false;
    }

    s_presentFrame = frameIndex % (uint32_t)s_frames.size();
    PerFrame &frame = s_frames[s_presentFrame];
    s_beginFrameCount++;
    const bool logThis = s_beginFrameCount <= 8;
    if (logThis)
        VK_LOG_INFO("BeginFrame #%llu fence-wait enter idx=%u",
                    (unsigned long long)s_beginFrameCount, s_presentFrame);
    vkWaitForFences(s_device, 1, &frame.inflightFence, VK_TRUE, UINT64_MAX);
    if (logThis)
        VK_LOG_INFO("BeginFrame #%llu fence-wait exit", (unsigned long long)s_beginFrameCount);
    // NOTE: do NOT reset the fence here. paraLLEl-RDP (libco coroutine on this same
    // cooperative thread) blocks on this fence via cb_wait_sync_index() during
    // retro_run(), which runs BEFORE EndFrame. If we reset it now, the fence sits
    // unsignaled for the whole frame, so once paraLLEl-RDP's sync ring wraps (~frame 8
    // with 3 sync frames) wait_sync_index waits on a fence nothing will signal until
    // EndFrame — a self-hang on a single thread. The reset happens just before
    // vkQueueSubmit in EndFrame instead, so the fence keeps reflecting the previous
    // submission's completion until then.

    VkResult acquire = vkAcquireNextImageKHR(s_device, s_swapchain, UINT64_MAX,
                                             frame.acquireSemaphore, VK_NULL_HANDLE,
                                             &s_currentImage);
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
    {
        VK_LOG_WARN("vkAcquireNextImageKHR failed: %d", (int)acquire);
        return false;
    }

    // In standalone the core filled this frame (image, semaphores) before the
    // present started; it is cleared at the end of EndFrame instead.
    if (!s_coreOwnsFrameIndex)
    {
        frame.imageValid = false;
        frame.coreCommandBuffers.clear();
        frame.signalSemaphore = VK_NULL_HANDLE;
    }

    vkResetCommandBuffer(frame.cmd, 0);
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(vkBeginCommandBuffer(frame.cmd, &beginInfo), "vkBeginCommandBuffer"))
        return false;

    s_frameInFlight = true;
    return true;
}

void EndFrame()
{
    if (!s_frameInFlight || s_frames.empty())
        return;

    PerFrame &frame = s_frames[s_presentFrame];
    VkImage swapImage = s_swapImages[s_currentImage];

    TransitionLayout(frame.cmd, swapImage,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    const retro_vulkan_image *sourceImage = nullptr;
    bool reusingLastImage = false;
    retro_vulkan_image lastImage = {};
    uint32_t lastImageFrame = 0;
    if (frame.imageValid && frame.image.create_info.image != VK_NULL_HANDLE)
        sourceImage = &frame.image;
    else
    {
        std::lock_guard<std::mutex> lastImageLock(s_lastImageMutex);
        if (s_lastImageValid && s_lastImage.create_info.image != VK_NULL_HANDLE)
        {
            lastImage = s_lastImage;
            lastImageFrame = s_lastImageFrame;
            sourceImage = &lastImage;
            reusingLastImage = true;
        }
    }

    if (sourceImage)
    {
        if (reusingLastImage && lastImageFrame < s_frames.size() &&
            lastImageFrame != s_presentFrame && s_frames[lastImageFrame].inflightFence)
        {
            VkFence fence = s_frames[lastImageFrame].inflightFence;
            vkWaitForFences(s_device, 1, &fence, VK_TRUE, UINT64_MAX);
        }

        VkImage coreImage = sourceImage->create_info.image;
        VkImageLayout coreLayout = sourceImage->image_layout;
        TransitionLayout(frame.cmd, coreImage,
                         coreLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        uint32_t srcW = s_sourceExtent.width ? s_sourceExtent.width : s_swapExtent.width;
        uint32_t srcH = s_sourceExtent.height ? s_sourceExtent.height : s_swapExtent.height;

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {(int32_t)srcW, (int32_t)srcH, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {(int32_t)s_swapExtent.width, (int32_t)s_swapExtent.height, 1};
        vkCmdBlitImage(frame.cmd,
                       coreImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);

        TransitionLayout(frame.cmd, coreImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, coreLayout,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    else
    {
        s_emptySourceFrames++;
        if (s_emptySourceFrames <= 8 || (s_emptySourceFrames % 60) == 0)
        {
            VK_LOG_WARN("No Vulkan source image on present #%llu frame=%u set_image_count=%llu last_valid=%d",
                        (unsigned long long)s_presentCount,
                        s_presentFrame,
                        (unsigned long long)s_setImageCount,
                        s_lastImageValid ? 1 : 0);
        }
        VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(frame.cmd, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }

    const int overlayVertexCount = s_overlayDrawData ? s_overlayDrawData->TotalVtxCount : 0;

    if (s_overlayReady && s_overlayDrawData && s_overlayDrawData->TotalVtxCount > 0 &&
        s_currentImage < s_overlayFramebuffers.size())
    {
        TransitionLayout(frame.cmd, swapImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = s_overlayRenderPass;
        rpbi.framebuffer = s_overlayFramebuffers[s_currentImage];
        rpbi.renderArea.extent = s_swapExtent;
        vkCmdBeginRenderPass(frame.cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(s_overlayDrawData, frame.cmd);
        vkCmdEndRenderPass(frame.cmd);

        TransitionLayout(frame.cmd, swapImage,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    else
    {
        TransitionLayout(frame.cmd, swapImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    s_overlayDrawData = nullptr;

    vkEndCommandBuffer(frame.cmd);

    std::vector<VkCommandBuffer> submitCmds;
    submitCmds.reserve(frame.coreCommandBuffers.size() + 1);
    submitCmds.insert(submitCmds.end(), frame.coreCommandBuffers.begin(), frame.coreCommandBuffers.end());
    submitCmds.push_back(frame.cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    std::vector<VkSemaphore> signalSems = {frame.renderSemaphore};
    if (frame.signalSemaphore != VK_NULL_HANDLE)
        signalSems.push_back(frame.signalSemaphore);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.acquireSemaphore;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = (uint32_t)submitCmds.size();
    submit.pCommandBuffers = submitCmds.data();
    submit.signalSemaphoreCount = (uint32_t)signalSems.size();
    submit.pSignalSemaphores = signalSems.data();

    {
        QueueLockGuard guard;
        // Reset immediately before reuse: the fence stayed signaled (reflecting
        // the previous submission) throughout retro_run so paraLLEl-RDP's
        // cb_wait_sync_index() never blocks forever. See BeginFrame note.
        vkResetFences(s_device, 1, &frame.inflightFence);
        VkResult result = vkQueueSubmit(s_queue, 1, &submit, frame.inflightFence);
        if (result != VK_SUCCESS)
            VK_LOG_ERROR("vkQueueSubmit failed: %d", (int)result);
    }

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.renderSemaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &s_swapchain;
    present.pImageIndices = &s_currentImage;

    {
        QueueLockGuard guard;
        VkResult result = vkQueuePresentKHR(s_presentQueue, &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            VK_LOG_WARN("vkQueuePresentKHR failed: %d", (int)result);
        else if (s_presentCount < 5 || (s_presentCount % 120) == 0)
            VK_LOG_INFO("Presented frame #%llu swap_image=%u core_cmds=%zu source_valid=%d overlay_vtx=%d",
                        (unsigned long long)s_presentCount,
                        s_currentImage,
                        frame.coreCommandBuffers.size(),
                        sourceImage ? 1 : 0,
                        overlayVertexCount);
    }

    s_presentCount++;
    s_frameInFlight = false;
    if (s_coreOwnsFrameIndex)
    {
        frame.imageValid = false;
        frame.coreCommandBuffers.clear();
        frame.signalSemaphore = VK_NULL_HANDLE;
    }
    else
        s_currentFrame = (s_currentFrame + 1) % (uint32_t)s_frames.size();
}

bool IsFrameInFlight() { return s_frameInFlight; }
bool IsReady() { return s_ready; }

bool InitOverlayRenderer()
{
    if (s_overlayReady)
        return true;
    if (!s_ready || !s_device || !s_overlayRenderPass)
    {
        VK_LOG_WARN("Overlay renderer init skipped: Vulkan not ready");
        return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 64;
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 64;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    if (!Check(vkCreateDescriptorPool(s_device, &dpci, nullptr, &s_overlayDescriptorPool),
               "vkCreateDescriptorPool"))
        return false;

#ifdef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    if (!ImGui_ImplVulkan_LoadFunctions(ImGuiVulkanLoader, nullptr))
    {
        VK_LOG_ERROR("ImGui Vulkan function loading failed");
        ShutdownOverlayRendererInternal();
        return false;
    }
#endif

    ImGui_ImplVulkan_InitInfo info = {};
    info.Instance = s_instance;
    info.PhysicalDevice = s_gpu;
    info.Device = s_device;
    info.QueueFamily = s_queueFamilyIndex;
    info.Queue = s_queue;
    info.DescriptorPool = s_overlayDescriptorPool;
    info.RenderPass = s_overlayRenderPass;
    info.MinImageCount = 2;
    info.ImageCount = (uint32_t)s_swapImages.size();
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&info))
    {
        VK_LOG_ERROR("ImGui_ImplVulkan_Init failed");
        ShutdownOverlayRendererInternal();
        return false;
    }
    s_overlayReady = true;

    if (!ImGui_ImplVulkan_CreateFontsTexture())
    {
        VK_LOG_ERROR("ImGui_ImplVulkan_CreateFontsTexture failed");
        ShutdownOverlayRendererInternal();
        return false;
    }

    VK_LOG_INFO("Overlay renderer initialized");
    return true;
}

void ShutdownOverlayRenderer()
{
    if (s_device)
        vkDeviceWaitIdle(s_device);
    ShutdownOverlayRendererInternal();
}

void BeginOverlayFrame()
{
    if (s_overlayReady)
        ImGui_ImplVulkan_NewFrame();
}

void SetOverlayDrawData(ImDrawData *drawData)
{
    s_overlayDrawData = drawData;
}

ImTextureID CreateOverlayTextureRGBA(const unsigned char *rgba, uint32_t width, uint32_t height)
{
    if (!s_overlayReady || !s_device || !s_commandPool || !rgba || width == 0 || height == 0)
        return 0;

    const VkDeviceSize uploadSize = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    OverlayTextureResource texture = {};
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    const char *failure = nullptr;
    bool success = false;
#define TICO_OVERLAY_TEXTURE_FAIL(message) \
    {                                      \
        failure = (message);               \
        break;                             \
    }

    do
    {
    VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = uploadSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(s_device, &bufferInfo, nullptr, &stagingBuffer), "vkCreateBuffer"))
        TICO_OVERLAY_TEXTURE_FAIL("staging buffer");

    VkMemoryRequirements bufferReq = {};
    vkGetBufferMemoryRequirements(s_device, stagingBuffer, &bufferReq);
    uint32_t bufferMemoryType = 0;
    if (!FindMemoryType(bufferReq.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        bufferMemoryType) &&
        !FindMemoryType(bufferReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, bufferMemoryType))
        TICO_OVERLAY_TEXTURE_FAIL("staging memory type");

    VkMemoryAllocateInfo bufferAlloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bufferAlloc.allocationSize = bufferReq.size;
    bufferAlloc.memoryTypeIndex = bufferMemoryType;
    if (!Check(vkAllocateMemory(s_device, &bufferAlloc, nullptr, &stagingMemory), "vkAllocateMemory"))
        TICO_OVERLAY_TEXTURE_FAIL("staging memory");
    if (!Check(vkBindBufferMemory(s_device, stagingBuffer, stagingMemory, 0), "vkBindBufferMemory"))
        TICO_OVERLAY_TEXTURE_FAIL("staging memory bind");

    void *mapped = nullptr;
    if (!Check(vkMapMemory(s_device, stagingMemory, 0, uploadSize, 0, &mapped), "vkMapMemory"))
        TICO_OVERLAY_TEXTURE_FAIL("staging memory map");
    std::memcpy(mapped, rgba, (size_t)uploadSize);
    VkMappedMemoryRange flushRange = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    flushRange.memory = stagingMemory;
    flushRange.size = VK_WHOLE_SIZE;
    if (!Check(vkFlushMappedMemoryRanges(s_device, 1, &flushRange), "vkFlushMappedMemoryRanges"))
    {
        vkUnmapMemory(s_device, stagingMemory);
        TICO_OVERLAY_TEXTURE_FAIL("staging memory flush");
    }
    vkUnmapMemory(s_device, stagingMemory);

    VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(s_device, &imageInfo, nullptr, &texture.image), "vkCreateImage"))
        TICO_OVERLAY_TEXTURE_FAIL("texture image");

    VkMemoryRequirements imageReq = {};
    vkGetImageMemoryRequirements(s_device, texture.image, &imageReq);
    uint32_t imageMemoryType = 0;
    if (!FindMemoryType(imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemoryType))
        TICO_OVERLAY_TEXTURE_FAIL("image memory type");

    VkMemoryAllocateInfo imageAlloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = imageMemoryType;
    if (!Check(vkAllocateMemory(s_device, &imageAlloc, nullptr, &texture.memory), "vkAllocateMemory"))
        TICO_OVERLAY_TEXTURE_FAIL("image memory");
    if (!Check(vkBindImageMemory(s_device, texture.image, texture.memory, 0), "vkBindImageMemory"))
        TICO_OVERLAY_TEXTURE_FAIL("image memory bind");

    VkImageViewCreateInfo viewInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (!Check(vkCreateImageView(s_device, &viewInfo, nullptr, &texture.view), "vkCreateImageView"))
        TICO_OVERLAY_TEXTURE_FAIL("image view");

    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Check(vkCreateSampler(s_device, &samplerInfo, nullptr, &texture.sampler), "vkCreateSampler"))
        TICO_OVERLAY_TEXTURE_FAIL("sampler");

    VkCommandBufferAllocateInfo commandAlloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAlloc.commandPool = s_commandPool;
    commandAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAlloc.commandBufferCount = 1;
    if (!Check(vkAllocateCommandBuffers(s_device, &commandAlloc, &commandBuffer), "vkAllocateCommandBuffers"))
        TICO_OVERLAY_TEXTURE_FAIL("upload command buffer");

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer"))
        TICO_OVERLAY_TEXTURE_FAIL("upload command begin");
    TransitionLayout(commandBuffer, texture.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    TransitionLayout(commandBuffer, texture.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!Check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer"))
        TICO_OVERLAY_TEXTURE_FAIL("upload command end");

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer;
    {
        QueueLockGuard guard;
        if (!Check(vkQueueSubmit(s_queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit"))
            TICO_OVERLAY_TEXTURE_FAIL("upload queue submit");
        if (!Check(vkQueueWaitIdle(s_queue), "vkQueueWaitIdle"))
            TICO_OVERLAY_TEXTURE_FAIL("upload queue wait");
    }

    vkFreeCommandBuffers(s_device, s_commandPool, 1, &commandBuffer);
    commandBuffer = VK_NULL_HANDLE;
    vkDestroyBuffer(s_device, stagingBuffer, nullptr);
    stagingBuffer = VK_NULL_HANDLE;
    vkFreeMemory(s_device, stagingMemory, nullptr);
    stagingMemory = VK_NULL_HANDLE;

    texture.descriptor = ImGui_ImplVulkan_AddTexture(texture.sampler, texture.view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!texture.descriptor)
        TICO_OVERLAY_TEXTURE_FAIL("imgui descriptor");
    s_overlayTextures.push_back(texture);
    success = true;
    } while (false);
#undef TICO_OVERLAY_TEXTURE_FAIL

    if (success)
        return (ImTextureID)texture.descriptor;

    VK_LOG_ERROR("Overlay texture creation failed: %s", failure ? failure : "unknown error");

    if (commandBuffer) vkFreeCommandBuffers(s_device, s_commandPool, 1, &commandBuffer);
    if (stagingBuffer) vkDestroyBuffer(s_device, stagingBuffer, nullptr);
    if (stagingMemory) vkFreeMemory(s_device, stagingMemory, nullptr);
    DestroyOverlayTextureResource(texture, false);
    return 0;
}

void DestroyOverlayTexture(ImTextureID textureId)
{
    if (!textureId || !s_device)
        return;

    VkDescriptorSet descriptor = (VkDescriptorSet)textureId;
    auto it = std::find_if(s_overlayTextures.begin(), s_overlayTextures.end(),
                           [descriptor](const OverlayTextureResource &texture) {
                               return texture.descriptor == descriptor;
                           });
    if (it == s_overlayTextures.end())
        return;

    vkDeviceWaitIdle(s_device);
    DestroyOverlayTextureResource(*it, true);
    s_overlayTextures.erase(it);
}

const retro_hw_render_interface_vulkan *GetHwRenderInterface()
{
    return s_ready ? &s_hwIface : nullptr;
}

void SetNegotiationInterface(const retro_hw_render_context_negotiation_interface_vulkan *iface)
{
    s_negIface = iface;
}

void GetSwapExtent(uint32_t &width, uint32_t &height)
{
    width = s_swapExtent.width;
    height = s_swapExtent.height;
}

void SetSourceExtent(uint32_t width, uint32_t height)
{
    if (s_sourceExtent.width != width || s_sourceExtent.height != height)
        VK_LOG_INFO("Source extent %ux%u", width, height);
    s_sourceExtent.width = width;
    s_sourceExtent.height = height;
}

} // namespace TicoVulkan
