/// @file TicoRenderer.cpp
/// @brief The renderer chosen at launch. See TicoRenderer.h.

#include "TicoRenderer.h"
#include "TicoGL.h"
#include "TicoVulkan.h"

namespace TicoRenderer
{
namespace
{
Backend s_backend = Backend::Vulkan;
}

Backend FromSetting(const std::string &value)
{
    if (value == "gl")
        return Backend::OpenGL;
    if (value == "zink")
        return Backend::Zink;
    return Backend::Vulkan;
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
    return IsVulkan() ? TicoVulkan::CreateTextureRGBA(rgba, width, height)
                      : TicoGL::CreateTextureRGBA(rgba, width, height);
}

void DestroyTexture(ImTextureID texture)
{
    if (IsVulkan())
        TicoVulkan::DestroyTexture(texture);
    else
        TicoGL::DestroyTexture(texture);
}

} // namespace TicoRenderer
