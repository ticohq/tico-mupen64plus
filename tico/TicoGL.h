/// @file TicoGL.h
/// @brief OpenGL for GLideN64: Mesa's EGL on NVC0 or Zink, and the overlay.
///
/// Two contexts share one namespace. The main thread's context owns the
/// window, draws the overlay and presents. The emulation thread's context is
/// where GLideN64 runs: it draws into a framebuffer of ours, and on each swap
/// that frame is copied into one of kSlots textures, which the main thread
/// draws through ImGui like any other image. Fences keep the two from using a
/// texture at the same time.
#pragma once

#include "imgui.h"

#include <cstdint>
#include <vector>

namespace TicoGL
{

constexpr uint32_t kSlots = 3;

/// Mesa's GL driver: native NVC0, or Zink on NVK. Picked before the first EGL
/// call, for the life of the process.
bool Init(uint32_t width, uint32_t height, bool zink);
void Shutdown();

void *GetProcAddress(const char *name);

// --- emulation thread -------------------------------------------------------

/// Make the GLideN64 context current on the calling thread, and release it.
bool BeginCoreThread();
void EndCoreThread();

/// The framebuffer GLideN64 renders into, at the size it renders at.
unsigned CoreFramebuffer(uint32_t width, uint32_t height);

/// A frame is finished: copy it into a slot and return the slot.
uint32_t PostCoreFrame(uint32_t width, uint32_t height);

// --- main thread ------------------------------------------------------------

/// Start a frame: the ImGui backend's new frame and a cleared screen.
void BeginFrame();

/// The texture holding the frame posted in `slot`, ready to sample, and its
/// size. Textures are bottom-up: draw them with v from 1 to 0.
ImTextureID CoreFrameTexture(uint32_t slot, uint32_t &width, uint32_t &height);

/// Draw ImGui and present. `slot` is the frame the overlay drew, if any
/// (released for reuse once the GPU has read it), or -1.
void EndFrame(ImDrawData *drawData, int slot);

/// The frame in `slot` as top-down RGBA8 (the save state pictures).
bool ReadFrameRGBA(uint32_t slot, std::vector<uint8_t> &out, uint32_t &width, uint32_t &height);

void Resize(uint32_t width, uint32_t height);
void GetSurfaceExtent(uint32_t &width, uint32_t &height);
void SetVsync(bool enabled);

ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height);
void DestroyTexture(ImTextureID texture);

} // namespace TicoGL
