/// @file TicoRenderer.cpp
/// @brief The renderer chosen at launch. See TicoRenderer.h.

#include "TicoRenderer.h"
#include "TicoGL.h"
#ifdef TICO_HAVE_VULKAN
#include "TicoVulkan.h"
#endif

namespace TicoRenderer
{
namespace
{
#ifdef TICO_HAVE_VULKAN
Backend s_backend = Backend::Vulkan;
#else
Backend s_backend = Backend::OpenGL;
#endif
}

Backend FromSetting(const std::string &value)
{
#ifdef TICO_HAVE_VULKAN
    if (value == "gl")
        return Backend::OpenGL;
    if (value == "zink")
        return Backend::Zink;
    return Backend::Vulkan;
#else
    // Built for a Mesa without Vulkan (no NVK, so no Zink either).
    (void)value;
    return Backend::OpenGL;
#endif
}

const char *SettingValue(Backend backend)
{
    switch (backend)
    {
    case Backend::OpenGL: return "gl";
    case Backend::Zink: return "zink";
    default: return "vk";
    }
}

void Select(Backend backend) { s_backend = backend; }
Backend Current() { return s_backend; }
bool IsVulkan() { return s_backend == Backend::Vulkan; }

const char *Name()
{
    switch (s_backend)
    {
    case Backend::OpenGL: return "OpenGL (NVC0)";
    case Backend::Zink: return "Zink";
    default: return "Vulkan (NVK)";
    }
}

ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height)
{
#ifdef TICO_HAVE_VULKAN
    if (IsVulkan())
        return TicoVulkan::CreateTextureRGBA(rgba, width, height);
#endif
    return TicoGL::CreateTextureRGBA(rgba, width, height);
}

void DestroyTexture(ImTextureID texture)
{
#ifdef TICO_HAVE_VULKAN
    if (IsVulkan())
    {
        TicoVulkan::DestroyTexture(texture);
        return;
    }
#endif
    TicoGL::DestroyTexture(texture);
}

} // namespace TicoRenderer
