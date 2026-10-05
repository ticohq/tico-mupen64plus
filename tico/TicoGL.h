/// @file TicoGL.h
/// @brief OpenGL for GLideN64: Mesa's EGL on NVC0 or Zink, and the overlay.
///
/// One context, as the libretro build had, taken in turn by the emulation
/// thread (GLideN64, with no surface) and the main thread (the overlay, with
/// the window): nouveau before Mesa 22.3 cannot run two contexts at once.
/// GLideN64 draws into a framebuffer of ours; on each swap that frame is
/// copied into one of kSlots textures, which the main thread draws through
/// ImGui like any other image. The emulation thread holds the context lock
/// while it emulates and gives it up for each frame it posts; the main thread
/// holds it from BeginFrame to EndFrame and puts back the GL state it changed,
/// since GLideN64 caches its own.
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

/// The context lock: taking it makes the context current on this thread. It
/// nests; the TicoGL calls below take it themselves.
void Lock();
void Unlock();

/// Main thread, while it waits for the emulator: give the lock up entirely,
/// then take it back to the depth Suspend returned.
int Suspend();
void Resume(int depth);

// --- emulation thread -------------------------------------------------------

/// Make the GLideN64 context current on the calling thread, and release it.
/// The thread holds the context lock in between.
bool BeginCoreThread();
void EndCoreThread();

/// The framebuffer GLideN64 renders into, at the size it renders at.
unsigned CoreFramebuffer(uint32_t width, uint32_t height);

/// A frame is finished: copy it into a slot and return the slot. Unlock
/// before handing the slot over (the main thread draws it), Lock after.
uint32_t PostCoreFrame(uint32_t width, uint32_t height);

// --- main thread ------------------------------------------------------------

/// Start a frame: the ImGui backend's new frame and a cleared screen. Holds
/// the context lock until EndFrame.
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
