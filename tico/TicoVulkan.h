/// @file TicoVulkan.h
/// @brief Vulkan device, swapchain and ImGui renderer for the tico frontend.
///
/// Mesa's NVK, statically linked; every entry point goes through volk, which
/// paraLLEl-RDP shares. With a game, the device is the one paraLLEl-RDP asks
/// for (tico_m64p_vulkan_create_device) and paraLLEl renders each scanned-out
/// frame into one of kFramesInFlight images; the frontend presents them. The
/// main thread's frame loop is:
///
///   VkCommandBuffer cmd = TicoVulkan::BeginFrame(slot);
///   ... record offscreen work (the shader chain) into cmd ...
///   TicoVulkan::EndFrame(ImGui::GetDrawData());   // swapchain pass + present
///
/// Everything the game draws reaches the screen through ImGui: the shader
/// chain renders into an image, which the overlay draws with AddImage.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#include "imgui.h"

#include <cstdint>
#include <functional>

struct tico_vk_interface;

namespace TicoVulkan
{

/// Frames the CPU may record ahead of the GPU, and the images paraLLEl-RDP
/// renders into. Per-frame resources (staging buffers, uniform buffers,
/// descriptor sets) are allocated this many times.
constexpr uint32_t kFramesInFlight = 3;

struct Context
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties props = {};
    VkPhysicalDeviceMemoryProperties memProps = {};
};

/// Bring up instance, surface, device, swapchain and ImGui's Vulkan backend.
/// With `forCore` the device is created by paraLLEl-RDP. An ImGui context
/// must exist.
bool Init(uint32_t width, uint32_t height, bool forCore);
void Shutdown();

const Context &Ctx();

/// The interface paraLLEl-RDP renders through (set_image, sync indices,
/// the queue lock).
const tico_vk_interface *CoreInterface();

/// Emulation thread, after paraLLEl scanned out a frame: the slot it was
/// rendered for. The next frame goes to the following slot.
uint32_t AdvanceCoreFrame();

/// The image paraLLEl last put in `slot`, if any.
bool CoreImage(uint32_t slot, VkImage &image, VkImageLayout &layout);

/// Slot of the frame being recorded, 0..kFramesInFlight-1.
uint32_t FrameIndex();

/// Wait for this slot's previous submission, acquire a swapchain image and
/// begin the frame's command buffer. A game's frames are presented in the
/// slot paraLLEl rendered them for; without one the slots take turns.
/// Returns VK_NULL_HANDLE when the frame has to be skipped (the swapchain is
/// recreated on the next call).
VkCommandBuffer BeginFrame(uint32_t slot);
VkCommandBuffer BeginFrame();

/// Draw ImGui into the swapchain image, submit and present.
void EndFrame(ImDrawData *drawData);

/// Resize the swapchain, e.g. on a handheld/docked switch.
void Resize(uint32_t width, uint32_t height);
void GetSwapExtent(uint32_t &width, uint32_t &height);

/// FIFO (vsync) or uncapped presentation, for fast-forward.
void SetVsync(bool enabled);

/// Run `fn` once the GPU can no longer be using what it frees.
void DeferDestroy(std::function<void()> fn);

void WaitIdle();

/// Every queue submission takes this lock, the frontend's and paraLLEl's.
void LockQueue();
void UnlockQueue();

bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props, uint32_t &index);

/// Submit one-shot work synchronously (texture uploads outside the frame).
VkCommandBuffer BeginOneShot();
void EndOneShot(VkCommandBuffer cmd);

/// Layout transition over the whole colour image.
void TransitionImage(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

/// A sampled 2D colour image with memory and view.
struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

/// `swizzleAlphaOne` forces alpha to 1 in the view, for XRGB/RGB formats.
bool CreateImage(Image &out, uint32_t width, uint32_t height, VkFormat format,
                 VkImageUsageFlags usage, uint32_t mipLevels = 1,
                 bool swizzleAlphaOne = false);
/// Destroy immediately (caller guarantees the GPU is done) ...
void DestroyImage(Image &img);
/// ... or once in-flight frames have finished.
void DeferDestroyImage(Image &img);

/// Host-visible, coherent buffer.
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkDeviceSize size = 0;
};
bool CreateBuffer(Buffer &out, VkDeviceSize size, VkBufferUsageFlags usage);
void DestroyBuffer(Buffer &buf);
void DeferDestroyBuffer(Buffer &buf);

/// RGBA8 texture for the overlay (icons, badges, avatar). 0 on failure.
ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height);
void DestroyTexture(ImTextureID tex);

/// Expose an image the caller owns to ImGui. It must be in
/// SHADER_READ_ONLY_OPTIMAL whenever ImGui draws it.
ImTextureID RegisterImage(VkImageView view);
void UnregisterImage(ImTextureID tex);

} // namespace TicoVulkan
