/// @file TicoVulkan.cpp
/// @brief Vulkan device, swapchain and ImGui renderer. See TicoVulkan.h.

#include "TicoVulkan.h"
#include "TicoLogger.h"

#include "m64p/tico_m64p.h"
#include "../mupen64plus-video-paraLLEl/tico_vulkan.h"

#include "imgui_impl_vulkan.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);
VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion);
}

#define VK_TAG "VK"

namespace TicoVulkan
{
namespace
{

struct PerFrame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
};

struct Texture
{
    Image image;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

Context s_ctx;
VkSurfaceKHR s_surface = VK_NULL_HANDLE;
VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
VkFormat s_swapFormat = VK_FORMAT_UNDEFINED;
VkColorSpaceKHR s_swapColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
VkExtent2D s_swapExtent = {0, 0};
VkExtent2D s_wantedExtent = {1280, 720};
std::vector<VkImage> s_swapImages;
std::vector<VkImageView> s_swapViews;
std::vector<VkFramebuffer> s_swapFramebuffers;
std::vector<VkSemaphore> s_renderDone; // one per swapchain image
VkRenderPass s_swapRenderPass = VK_NULL_HANDLE;

VkCommandPool s_commandPool = VK_NULL_HANDLE;
PerFrame s_frames[kFramesInFlight];
uint32_t s_frameSlot = 0;
uint64_t s_frameCounter = 0;
uint32_t s_imageIndex = 0;
bool s_frameActive = false;
bool s_swapchainDirty = false;
bool s_vsync = true;
bool s_ready = false;
bool s_imguiReady = false;

std::deque<std::pair<uint64_t, std::function<void()>>> s_deferred;
std::vector<Texture> s_textures;

// paraLLEl-RDP submits from the emulation thread (and its own worker), the
// frontend from the main thread; one queue, so one lock. Recursive: paraLLEl
// may submit while it already holds it.
std::recursive_mutex s_queueMutex;

// The slot paraLLEl renders its next frame into (emulation thread), and the
// image it scanned out into each slot.
std::atomic<uint32_t> s_coreSlot{0};
std::mutex s_coreImageMutex;
tico_vk_image s_coreImages[kFramesInFlight] = {};
bool s_coreImageValid[kFramesInFlight] = {};
tico_vk_interface s_coreInterface = {};

bool Check(VkResult res, const char *what)
{
    if (res == VK_SUCCESS)
        return true;
    LOG_ERROR(VK_TAG, "%s failed: %d", what, (int)res);
    return false;
}

PFN_vkGetInstanceProcAddr GetInstanceProcAddr()
{
    uint32_t icdVersion = 5;
    vk_icdNegotiateLoaderICDInterfaceVersion(&icdVersion);
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(&vk_icdGetInstanceProcAddr);
}

void RunDeferred(bool all)
{
    while (!s_deferred.empty() &&
           (all || s_deferred.front().first + kFramesInFlight <= s_frameCounter))
    {
        s_deferred.front().second();
        s_deferred.pop_front();
    }
}

//------------------------------------------------------------------------------
// paraLLEl-RDP's interface
//------------------------------------------------------------------------------

void CoreSetImage(void *, const tico_vk_image *image)
{
    if (!image)
        return;
    const uint32_t slot = s_coreSlot.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(s_coreImageMutex);
    s_coreImages[slot] = *image;
    s_coreImageValid[slot] = image->create_info.image != VK_NULL_HANDLE;
}

uint32_t CoreGetSyncIndex(void *)
{
    return s_coreSlot.load(std::memory_order_relaxed);
}

uint32_t CoreGetSyncIndexMask(void *)
{
    return (1u << kFramesInFlight) - 1u;
}

// The image paraLLEl is about to render into was last read by the present of
// this slot; wait for that to finish.
void CoreWaitSyncIndex(void *)
{
    const PerFrame &f = s_frames[s_coreSlot.load(std::memory_order_relaxed)];
    if (f.fence)
        vkWaitForFences(s_ctx.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
}

void CoreLockQueue(void *) { s_queueMutex.lock(); }
void CoreUnlockQueue(void *) { s_queueMutex.unlock(); }

//------------------------------------------------------------------------------
// Bring-up
//------------------------------------------------------------------------------

bool CreateInstance()
{
    PFN_vkGetInstanceProcAddr getProc = GetInstanceProcAddr();
    volkInitializeCustom(getProc);

    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "tico-mupen64plus";
    app.pEngineName = "tico";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateInstance(&ci, nullptr, &s_ctx.instance), "vkCreateInstance"))
        return false;
    volkLoadInstance(s_ctx.instance);
    return true;
}

bool CreateSurface()
{
    VkViSurfaceCreateInfoNN ci = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    ci.window = nwindowGetDefault();
    return Check(vkCreateViSurfaceNN(s_ctx.instance, &ci, nullptr, &s_surface), "vkCreateViSurfaceNN");
}

void FinishDevice()
{
    volkLoadDevice(s_ctx.device);
    vkGetPhysicalDeviceProperties(s_ctx.gpu, &s_ctx.props);
    vkGetPhysicalDeviceMemoryProperties(s_ctx.gpu, &s_ctx.memProps);
    LOG_INFO(VK_TAG, "Device: %s (Vulkan %u.%u.%u), queue family %u", s_ctx.props.deviceName,
             VK_API_VERSION_MAJOR(s_ctx.props.apiVersion), VK_API_VERSION_MINOR(s_ctx.props.apiVersion),
             VK_API_VERSION_PATCH(s_ctx.props.apiVersion), s_ctx.queueFamily);
}

// paraLLEl-RDP picks the features and extensions it needs; the frontend only
// adds the swapchain.
bool CreateCoreDevice()
{
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    tico_vk_context context = {};
    if (!tico_m64p_vulkan_create_device(&context, s_ctx.instance, VK_NULL_HANDLE, s_surface,
                                        (void *)GetInstanceProcAddr(), extensions, 1))
    {
        LOG_ERROR(VK_TAG, "paraLLEl-RDP could not create its device");
        return false;
    }
    s_ctx.gpu = context.gpu;
    s_ctx.device = context.device;
    s_ctx.queue = context.queue;
    s_ctx.queueFamily = context.queue_family_index;
    FinishDevice();

    s_coreInterface.handle = nullptr;
    s_coreInterface.set_image = CoreSetImage;
    s_coreInterface.get_sync_index = CoreGetSyncIndex;
    s_coreInterface.get_sync_index_mask = CoreGetSyncIndexMask;
    s_coreInterface.wait_sync_index = CoreWaitSyncIndex;
    s_coreInterface.lock_queue = CoreLockQueue;
    s_coreInterface.unlock_queue = CoreUnlockQueue;
    return true;
}

// Without a game (the library, or GL rendering the overlay elsewhere): a plain
// device with a graphics queue that can present.
bool CreateOwnDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s_ctx.instance, &count, nullptr);
    if (count == 0)
    {
        LOG_ERROR(VK_TAG, "No Vulkan devices");
        return false;
    }
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(s_ctx.instance, &count, gpus.data());
    s_ctx.gpu = gpus[0];

    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s_ctx.gpu, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(s_ctx.gpu, &qcount, queues.data());
    bool found = false;
    for (uint32_t i = 0; i < qcount && !found; i++)
    {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(s_ctx.gpu, i, s_surface, &present);
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
        {
            s_ctx.queueFamily = i;
            found = true;
        }
    }
    if (!found)
    {
        LOG_ERROR(VK_TAG, "No graphics queue that can present");
        return false;
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = s_ctx.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;
    if (!Check(vkCreateDevice(s_ctx.gpu, &ci, nullptr, &s_ctx.device), "vkCreateDevice"))
        return false;
    vkGetDeviceQueue(s_ctx.device, s_ctx.queueFamily, 0, &s_ctx.queue);
    FinishDevice();
    return true;
}

void DestroySwapchainResources()
{
    for (VkFramebuffer fb : s_swapFramebuffers)
        vkDestroyFramebuffer(s_ctx.device, fb, nullptr);
    for (VkImageView view : s_swapViews)
        vkDestroyImageView(s_ctx.device, view, nullptr);
    for (VkSemaphore sem : s_renderDone)
        vkDestroySemaphore(s_ctx.device, sem, nullptr);
    s_swapFramebuffers.clear();
    s_swapViews.clear();
    s_renderDone.clear();
    s_swapImages.clear();
}

bool CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_ctx.gpu, s_surface, &caps);

    if (s_swapFormat == VK_FORMAT_UNDEFINED)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(s_ctx.gpu, s_surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(s_ctx.gpu, s_surface, &count, formats.data());
        if (formats.empty())
        {
            LOG_ERROR(VK_TAG, "Surface has no formats");
            return false;
        }
        // UNORM: shaders write display-ready values, as they do in RetroArch.
        VkSurfaceFormatKHR chosen = formats[0];
        for (const VkSurfaceFormatKHR &f : formats)
        {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)
            {
                chosen = f;
                break;
            }
        }
        s_swapFormat = chosen.format;
        s_swapColorSpace = chosen.colorSpace;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        extent.width = std::clamp(s_wantedExtent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(s_wantedExtent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return false;

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!s_vsync)
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(s_ctx.gpu, s_surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(s_ctx.gpu, s_surface, &count, modes.data());
        for (VkPresentModeKHR m : modes)
        {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR)
                mode = m;
            else if (m == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)
                mode = m;
        }
    }

    uint32_t imageCount = std::max(3u, caps.minImageCount);
    if (caps.maxImageCount)
        imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainKHR old = s_swapchain;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = s_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = s_swapFormat;
    ci.imageColorSpace = s_swapColorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if (!Check(vkCreateSwapchainKHR(s_ctx.device, &ci, nullptr, &s_swapchain), "vkCreateSwapchainKHR"))
    {
        s_swapchain = old;
        return false;
    }
    DestroySwapchainResources();
    if (old)
        vkDestroySwapchainKHR(s_ctx.device, old, nullptr);
    s_swapExtent = extent;

    if (!s_swapRenderPass)
    {
        VkAttachmentDescription color = {};
        color.format = s_swapFormat;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;
        // Wait for the acquire semaphore's stage before writing.
        VkSubpassDependency dep = {};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &color;
        rp.subpassCount = 1;
        rp.pSubpasses = &subpass;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        if (!Check(vkCreateRenderPass(s_ctx.device, &rp, nullptr, &s_swapRenderPass), "vkCreateRenderPass"))
            return false;
    }

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(s_ctx.device, s_swapchain, &count, nullptr);
    s_swapImages.resize(count);
    vkGetSwapchainImagesKHR(s_ctx.device, s_swapchain, &count, s_swapImages.data());
    s_swapViews.resize(count);
    s_swapFramebuffers.resize(count);
    s_renderDone.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = s_swapImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s_swapFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(s_ctx.device, &vci, nullptr, &s_swapViews[i]);

        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = s_swapRenderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &s_swapViews[i];
        fci.width = extent.width;
        fci.height = extent.height;
        fci.layers = 1;
        vkCreateFramebuffer(s_ctx.device, &fci, nullptr, &s_swapFramebuffers[i]);

        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(s_ctx.device, &sci, nullptr, &s_renderDone[i]);
    }

    if (s_imguiReady)
        ImGui_ImplVulkan_SetMinImageCount(imageCount);

    LOG_INFO(VK_TAG, "Swapchain %ux%u, %u images, %s", extent.width, extent.height, count,
             mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : "uncapped");
    s_swapchainDirty = false;
    return true;
}

bool RecreateSwapchain()
{
    WaitIdle();
    return CreateSwapchain();
}

bool CreateFrameResources()
{
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_ctx.queueFamily;
    if (!Check(vkCreateCommandPool(s_ctx.device, &pci, nullptr, &s_commandPool), "vkCreateCommandPool"))
        return false;

    for (PerFrame &f : s_frames)
    {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s_commandPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(s_ctx.device, &ai, &f.cmd);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(s_ctx.device, &fci, nullptr, &f.fence);
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(s_ctx.device, &sci, nullptr, &f.acquired);
    }
    return true;
}

PFN_vkVoidFunction ImGuiLoader(const char *name, void *)
{
    PFN_vkVoidFunction fn = vkGetDeviceProcAddr(s_ctx.device, name);
    return fn ? fn : vkGetInstanceProcAddr(s_ctx.instance, name);
}

bool InitImGui()
{
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, ImGuiLoader))
    {
        LOG_ERROR(VK_TAG, "ImGui_ImplVulkan_LoadFunctions failed");
        return false;
    }

    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = s_ctx.instance;
    info.PhysicalDevice = s_ctx.gpu;
    info.Device = s_ctx.device;
    info.QueueFamily = s_ctx.queueFamily;
    info.Queue = s_ctx.queue;
    info.DescriptorPoolSize = 256;
    info.MinImageCount = (uint32_t)std::max<size_t>(2, s_swapImages.size());
    info.ImageCount = (uint32_t)s_swapImages.size();
    info.PipelineInfoMain.RenderPass = s_swapRenderPass;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info))
    {
        LOG_ERROR(VK_TAG, "ImGui_ImplVulkan_Init failed");
        return false;
    }
    s_imguiReady = true;
    return true;
}

} // namespace

//==============================================================================
// Lifecycle
//==============================================================================

bool Init(uint32_t width, uint32_t height, bool forCore)
{
    s_wantedExtent = {width, height};
    if (!CreateInstance() || !CreateSurface() || !(forCore ? CreateCoreDevice() : CreateOwnDevice()) ||
        !CreateSwapchain() || !CreateFrameResources() || !InitImGui())
    {
        Shutdown();
        return false;
    }
    s_ready = true;
    return true;
}

void Shutdown()
{
    if (s_ctx.device)
    {
        WaitIdle();
        RunDeferred(true);

        for (Texture &t : s_textures)
            DestroyImage(t.image);
        s_textures.clear();

        if (s_imguiReady)
            ImGui_ImplVulkan_Shutdown();
        s_imguiReady = false;

        for (PerFrame &f : s_frames)
        {
            if (f.fence)
                vkDestroyFence(s_ctx.device, f.fence, nullptr);
            if (f.acquired)
                vkDestroySemaphore(s_ctx.device, f.acquired, nullptr);
            f = {};
        }
        if (s_commandPool)
            vkDestroyCommandPool(s_ctx.device, s_commandPool, nullptr);
        s_commandPool = VK_NULL_HANDLE;

        DestroySwapchainResources();
        if (s_swapchain)
            vkDestroySwapchainKHR(s_ctx.device, s_swapchain, nullptr);
        s_swapchain = VK_NULL_HANDLE;
        if (s_swapRenderPass)
            vkDestroyRenderPass(s_ctx.device, s_swapRenderPass, nullptr);
        s_swapRenderPass = VK_NULL_HANDLE;

        vkDestroyDevice(s_ctx.device, nullptr);
    }
    if (s_surface)
        vkDestroySurfaceKHR(s_ctx.instance, s_surface, nullptr);
    s_surface = VK_NULL_HANDLE;
    if (s_ctx.instance)
        vkDestroyInstance(s_ctx.instance, nullptr);
    s_ctx = {};
    s_swapFormat = VK_FORMAT_UNDEFINED;
    s_ready = false;
}

const Context &Ctx() { return s_ctx; }
uint32_t FrameIndex() { return s_frameSlot; }

const tico_vk_interface *CoreInterface()
{
    return s_coreInterface.set_image ? &s_coreInterface : nullptr;
}

uint32_t AdvanceCoreFrame()
{
    const uint32_t done = s_coreSlot.load(std::memory_order_relaxed);
    s_coreSlot.store((done + 1) % kFramesInFlight, std::memory_order_relaxed);
    return done;
}

bool CoreImage(uint32_t slot, VkImage &image, VkImageLayout &layout)
{
    std::lock_guard<std::mutex> lock(s_coreImageMutex);
    if (slot >= kFramesInFlight || !s_coreImageValid[slot])
        return false;
    image = s_coreImages[slot].create_info.image;
    layout = s_coreImages[slot].image_layout;
    return true;
}

void WaitIdle()
{
    if (!s_ctx.device)
        return;
    std::lock_guard<std::recursive_mutex> lock(s_queueMutex);
    vkDeviceWaitIdle(s_ctx.device);
}

void LockQueue() { s_queueMutex.lock(); }
void UnlockQueue() { s_queueMutex.unlock(); }

//==============================================================================
// Frame loop
//==============================================================================

VkCommandBuffer BeginFrame()
{
    return BeginFrame((s_frameSlot + 1) % kFramesInFlight);
}

VkCommandBuffer BeginFrame(uint32_t slot)
{
    if (!s_ready)
        return VK_NULL_HANDLE;
    if (s_swapchainDirty && !RecreateSwapchain())
        return VK_NULL_HANDLE;

    s_frameSlot = slot % kFramesInFlight;
    PerFrame &f = s_frames[s_frameSlot];
    vkWaitForFences(s_ctx.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    RunDeferred(false);

    VkResult res = vkAcquireNextImageKHR(s_ctx.device, s_swapchain, UINT64_MAX,
                                         f.acquired, VK_NULL_HANDLE, &s_imageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR)
    {
        s_swapchainDirty = true;
        return VK_NULL_HANDLE;
    }
    if (res == VK_SUBOPTIMAL_KHR)
        s_swapchainDirty = true;
    else if (!Check(res, "vkAcquireNextImageKHR"))
        return VK_NULL_HANDLE;

    vkResetFences(s_ctx.device, 1, &f.fence);
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    s_frameActive = true;
    return f.cmd;
}

void EndFrame(ImDrawData *drawData)
{
    if (!s_frameActive)
        return;
    PerFrame &f = s_frames[s_frameSlot];

    VkClearValue clear = {};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s_swapRenderPass;
    rp.framebuffer = s_swapFramebuffers[s_imageIndex];
    rp.renderArea.extent = s_swapExtent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (drawData && s_imguiReady)
    {
        // uploading a changed font atlas submits to the queue
        std::lock_guard<std::recursive_mutex> lock(s_queueMutex);
        ImGui_ImplVulkan_RenderDrawData(drawData, f.cmd);
    }
    vkCmdEndRenderPass(f.cmd);
    vkEndCommandBuffer(f.cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s_renderDone[s_imageIndex];

    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s_renderDone[s_imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s_swapchain;
    pi.pImageIndices = &s_imageIndex;

    VkResult res;
    {
        std::lock_guard<std::recursive_mutex> lock(s_queueMutex);
        Check(vkQueueSubmit(s_ctx.queue, 1, &si, f.fence), "vkQueueSubmit");
        res = vkQueuePresentKHR(s_ctx.queue, &pi);
    }
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        s_swapchainDirty = true;
    else
        Check(res, "vkQueuePresentKHR");

    s_frameActive = false;
    s_frameCounter++;
}

void Resize(uint32_t width, uint32_t height)
{
    if (width == s_wantedExtent.width && height == s_wantedExtent.height &&
        width == s_swapExtent.width && height == s_swapExtent.height)
        return;
    s_wantedExtent = {width, height};
    s_swapchainDirty = true;
}

void GetSwapExtent(uint32_t &width, uint32_t &height)
{
    width = s_swapExtent.width;
    height = s_swapExtent.height;
}

void SetVsync(bool enabled)
{
    if (enabled == s_vsync)
        return;
    s_vsync = enabled;
    s_swapchainDirty = true;
}

void DeferDestroy(std::function<void()> fn)
{
    if (!s_ready)
    {
        fn();
        return;
    }
    s_deferred.emplace_back(s_frameCounter, std::move(fn));
}

//==============================================================================
// Resources
//==============================================================================

bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t &index)
{
    for (uint32_t i = 0; i < s_ctx.memProps.memoryTypeCount; i++)
    {
        if ((typeBits & (1u << i)) &&
            (s_ctx.memProps.memoryTypes[i].propertyFlags & props) == props)
        {
            index = i;
            return true;
        }
    }
    return false;
}

VkCommandBuffer BeginOneShot()
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s_commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(s_ctx.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void EndOneShot(VkCommandBuffer cmd)
{
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence done = VK_NULL_HANDLE;
    vkCreateFence(s_ctx.device, &fci, nullptr, &done);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    {
        std::lock_guard<std::recursive_mutex> lock(s_queueMutex);
        vkQueueSubmit(s_ctx.queue, 1, &si, done);
    }
    // a fence rather than vkQueueWaitIdle: paraLLEl keeps the queue busy
    vkWaitForFences(s_ctx.device, 1, &done, VK_TRUE, UINT64_MAX);
    vkDestroyFence(s_ctx.device, done, nullptr);
    vkFreeCommandBuffers(s_ctx.device, s_commandPool, 1, &cmd);
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool CreateImage(Image &out, uint32_t width, uint32_t height, VkFormat format,
                 VkImageUsageFlags usage, uint32_t mipLevels, bool swizzleAlphaOne)
{
    out = {};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vkCreateImage(s_ctx.device, &ci, nullptr, &out.image), "vkCreateImage"))
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(s_ctx.device, out.image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ai.memoryTypeIndex) ||
        !Check(vkAllocateMemory(s_ctx.device, &ai, nullptr, &out.memory), "vkAllocateMemory(image)"))
    {
        DestroyImage(out);
        return false;
    }
    vkBindImageMemory(s_ctx.device, out.image, out.memory, 0);

    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    if (swizzleAlphaOne)
        vci.components.a = VK_COMPONENT_SWIZZLE_ONE;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    if (!Check(vkCreateImageView(s_ctx.device, &vci, nullptr, &out.view), "vkCreateImageView"))
    {
        DestroyImage(out);
        return false;
    }

    out.format = format;
    out.width = width;
    out.height = height;
    out.mipLevels = mipLevels;
    out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void DestroyImage(Image &img)
{
    if (img.view)
        vkDestroyImageView(s_ctx.device, img.view, nullptr);
    if (img.image)
        vkDestroyImage(s_ctx.device, img.image, nullptr);
    if (img.memory)
        vkFreeMemory(s_ctx.device, img.memory, nullptr);
    img = {};
}

void DeferDestroyImage(Image &img)
{
    if (!img.image)
        return;
    Image copy = img;
    img = {};
    DeferDestroy([copy]() mutable { DestroyImage(copy); });
}

bool CreateBuffer(Buffer &out, VkDeviceSize size, VkBufferUsageFlags usage)
{
    out = {};
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(s_ctx.device, &ci, nullptr, &out.buffer), "vkCreateBuffer"))
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(s_ctx.device, out.buffer, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    if (!FindMemoryType(req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        ai.memoryTypeIndex) ||
        !Check(vkAllocateMemory(s_ctx.device, &ai, nullptr, &out.memory), "vkAllocateMemory(buffer)"))
    {
        DestroyBuffer(out);
        return false;
    }
    vkBindBufferMemory(s_ctx.device, out.buffer, out.memory, 0);
    vkMapMemory(s_ctx.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    out.size = size;
    return true;
}

void DestroyBuffer(Buffer &buf)
{
    if (buf.buffer)
        vkDestroyBuffer(s_ctx.device, buf.buffer, nullptr);
    if (buf.memory)
        vkFreeMemory(s_ctx.device, buf.memory, nullptr);
    buf = {};
}

void DeferDestroyBuffer(Buffer &buf)
{
    if (!buf.buffer)
        return;
    Buffer copy = buf;
    buf = {};
    DeferDestroy([copy]() mutable { DestroyBuffer(copy); });
}

ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height)
{
    if (!s_imguiReady || !rgba || width <= 0 || height <= 0)
        return ImTextureID_Invalid;

    Texture tex;
    if (!CreateImage(tex.image, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return ImTextureID_Invalid;

    Buffer staging;
    const VkDeviceSize size = (VkDeviceSize)width * height * 4;
    if (!CreateBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
    {
        DestroyImage(tex.image);
        return ImTextureID_Invalid;
    }
    memcpy(staging.mapped, rgba, (size_t)size);

    VkCommandBuffer cmd = BeginOneShot();
    TransitionImage(cmd, tex.image.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImage(cmd, tex.image.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    EndOneShot(cmd);
    DestroyBuffer(staging);

    tex.image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    tex.descriptor = ImGui_ImplVulkan_AddTexture(tex.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s_textures.push_back(tex);
    return (ImTextureID)tex.descriptor;
}

void DestroyTexture(ImTextureID id)
{
    if (id == ImTextureID_Invalid)
        return;
    auto it = std::find_if(s_textures.begin(), s_textures.end(), [id](const Texture &t) {
        return (ImTextureID)t.descriptor == id;
    });
    if (it == s_textures.end())
        return;
    Texture tex = *it;
    s_textures.erase(it);
    DeferDestroy([tex]() mutable {
        if (s_imguiReady)
            ImGui_ImplVulkan_RemoveTexture(tex.descriptor);
        DestroyImage(tex.image);
    });
}

ImTextureID RegisterImage(VkImageView view)
{
    if (!s_imguiReady || !view)
        return ImTextureID_Invalid;
    return (ImTextureID)ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void UnregisterImage(ImTextureID id)
{
    if (id == ImTextureID_Invalid)
        return;
    VkDescriptorSet set = (VkDescriptorSet)id;
    DeferDestroy([set]() {
        if (s_imguiReady)
            ImGui_ImplVulkan_RemoveTexture(set);
    });
}

} // namespace TicoVulkan
