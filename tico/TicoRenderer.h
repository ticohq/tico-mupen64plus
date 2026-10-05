/// @file TicoRenderer.h
/// @brief The renderer chosen at launch, for code that works with either.
///
/// tico's module settings pick Vulkan (paraLLEl-RDP on NVK), OpenGL (GLideN64
/// on Mesa's NVC0 driver) or Zink (GLideN64 on Mesa's GL-over-Vulkan). It is
/// read once, before anything is created, and holds for the whole run: the
/// running game cannot switch it. The overlay creates its textures through
/// here and does not care which backend draws them.
#pragma once

#include "imgui.h"

#include <string>

namespace TicoRenderer
{

enum class Backend
{
    Vulkan,
    OpenGL, // GLideN64 on NVC0
    Zink,   // GLideN64 on Zink
};

/// The renderer the config names ("vk", "gl", "zink"); Vulkan otherwise.
Backend FromSetting(const std::string &value);
const char *SettingValue(Backend backend);

void Select(Backend backend);
Backend Current();
bool IsVulkan();
const char *Name();

/// RGBA8 texture for the overlay (icons, badges, avatar, state pictures).
/// ImTextureID_Invalid on failure. Main thread only.
ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height);
void DestroyTexture(ImTextureID texture);

} // namespace TicoRenderer
