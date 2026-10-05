/// @file vidext_tico.c
/// @brief The core's video extension, implemented by the tico frontend.
///
/// The frontend owns the window and the GL contexts (TicoGL): the emulation
/// thread already has its context current when GLideN64 starts, so there is
/// no mode to set. A swap hands the finished frame to the frontend, and GL
/// entry points come from its loader. paraLLEl-RDP uses none of this.

#include <stddef.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/m64p_types.h"
#include "api/m64p_vidext.h"
#include "api/vidext.h"

#include <mupen64plus-next_common.h>

static int s_video_running = 0;

m64p_error OverrideVideoFunctions(m64p_video_extension_functions *functions)
{
   (void)functions;
   return M64ERR_SUCCESS;
}

int VidExt_InFullscreenMode(void) { return 1; }
int VidExt_VideoRunning(void) { return s_video_running; }

EXPORT m64p_error CALL VidExt_Init(void)
{
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_Quit(void)
{
   s_video_running = 0;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size *sizes, int *count)
{
   (void)sizes;
   if (count)
      *count = 0;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size size, int *count, int *rates)
{
   (void)size;
   (void)rates;
   if (count)
      *count = 0;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetVideoMode(int width, int height, int bits, m64p_video_mode mode,
                                           m64p_video_flags flags)
{
   (void)width; (void)height; (void)bits; (void)mode; (void)flags;
   s_video_running = 1;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetVideoModeWithRate(int width, int height, int rate, int bits,
                                                   m64p_video_mode mode, m64p_video_flags flags)
{
   (void)rate;
   return VidExt_SetVideoMode(width, height, bits, mode, flags);
}

EXPORT m64p_error CALL VidExt_ResizeWindow(int width, int height)
{
   (void)width; (void)height;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_SetCaption(const char *title)
{
   (void)title;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_ToggleFullScreen(void)
{
   return M64ERR_SUCCESS;
}

EXPORT m64p_function CALL VidExt_GL_GetProcAddress(const char *name)
{
   return (m64p_function)tico_m64p_gl_get_proc_address(name);
}

EXPORT m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr attr, int value)
{
   (void)attr; (void)value;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr attr, int *value)
{
   (void)attr;
   if (value)
      *value = 0;
   return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL VidExt_GL_SwapBuffers(void)
{
   tico_m64p_present_gl();
   return M64ERR_SUCCESS;
}

EXPORT uint32_t CALL VidExt_GL_GetDefaultFramebuffer(void)
{
   return tico_m64p_gl_default_framebuffer();
}

EXPORT m64p_error CALL VidExt_VK_GetSurface(void **surface, void *instance)
{
   (void)surface; (void)instance;
   return M64ERR_UNSUPPORTED;
}

EXPORT m64p_error CALL VidExt_VK_GetInstanceExtensions(const char **extensions[], uint32_t *count)
{
   (void)extensions;
   if (count)
      *count = 0;
   return M64ERR_UNSUPPORTED;
}
