/// @file TicoVulkan.h
/// @brief Minimal Vulkan frontend for libretro hardware rendering on Switch.
#pragma once

#include "imgui.h"

#include <cstdint>

#if defined(__SWITCH__) && !defined(VK_USE_PLATFORM_VI_NN)
#define VK_USE_PLATFORM_VI_NN
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <libretro_vulkan.h>

struct ImDrawData;

namespace TicoVulkan
{

bool CreateInstance();
bool CreateDeviceAndSwapchain();
void Shutdown();

bool BeginFrame();
void EndFrame();
// Standalone: the emu thread owns the core's frame (sync) index and presents
// happen on another thread. AdvanceCoreFrame() is called by the emu thread once
// a frame is scanned out and returns that frame's index; BeginFrameAt() records
// the present for it.
uint32_t AdvanceCoreFrame();
bool BeginFrameAt(uint32_t frameIndex);
bool IsFrameInFlight();
bool IsReady();

const retro_hw_render_interface_vulkan *GetHwRenderInterface();
void SetNegotiationInterface(const retro_hw_render_context_negotiation_interface_vulkan *iface);

void GetSwapExtent(uint32_t &width, uint32_t &height);
void SetSourceExtent(uint32_t width, uint32_t height);

bool InitOverlayRenderer();
void ShutdownOverlayRenderer();
void BeginOverlayFrame();
void SetOverlayDrawData(ImDrawData *drawData);
ImTextureID CreateOverlayTextureRGBA(const unsigned char *rgba, uint32_t width, uint32_t height);
void DestroyOverlayTexture(ImTextureID texture);

} // namespace TicoVulkan
