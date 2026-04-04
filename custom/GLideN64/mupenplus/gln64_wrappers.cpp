/* gln64_wrappers.cpp
 * Provides gln64-prefixed entry points that plugin.c / libretro.c expect,
 * forwarding to the unprefixed GLideN64 API.
 * Also provides Switch stubs for symbols not available on this platform.
 */

#ifdef OS_WINDOWS
# include <windows.h>
#else
# include "winlnxdefs.h"
#endif

#include "PluginAPI.h"
#include "N64.h"
#include "mupenplus/GLideN64_mupenplus.h"

extern "C" {

EXPORT m64p_error CALL gln64PluginGetVersion(
    m64p_plugin_type *_PluginType,
    int *_PluginVersion,
    int *_APIVersion,
    const char **_PluginNamePtr,
    int *_Capabilities)
{
    return api().PluginGetVersion(_PluginType, _PluginVersion, _APIVersion, _PluginNamePtr, _Capabilities);
}

EXPORT void CALL gln64ChangeWindow(void)       { api().ChangeWindow(); }
EXPORT int  CALL gln64InitiateGFX(GFX_INFO g)  { return api().InitiateGFX(g); }
EXPORT void CALL gln64MoveScreen(int x, int y) { api().MoveScreen(x, y); }
EXPORT void CALL gln64ProcessDList(void)        { api().ProcessDList(); }
EXPORT void CALL gln64ProcessRDPList(void)      { api().ProcessRDPList(); }
EXPORT void CALL gln64RomClosed(void)           { api().RomClosed(); }

EXPORT int CALL gln64RomOpen(void)
{
    if (rdram_size != nullptr)
        RDRAMSize = *rdram_size - 1;
    else
        RDRAMSize = 0;
    return api().RomOpen();
}

EXPORT void CALL gln64ShowCFB(void)                                    { api().ShowCFB(); }
EXPORT void CALL gln64UpdateScreen(void)                               { api().UpdateScreen(); }
EXPORT void CALL gln64ViStatusChanged(void)                            { api().ViStatusChanged(); }
EXPORT void CALL gln64ViWidthChanged(void)                             { api().ViWidthChanged(); }
EXPORT void CALL gln64ReadScreen2(void *d, int *w, int *h, int f)      { api().ReadScreen2(d, w, h, f); }
EXPORT void CALL gln64SetRenderingCallback(void (*cb)(int))            { api().SetRenderingCallback(cb); }
EXPORT void CALL gln64FBRead(unsigned int addr)                        { api().FBRead(addr); }
EXPORT void CALL gln64FBWrite(unsigned int addr, unsigned int size)    { api().FBWrite(addr, size); }
EXPORT void CALL gln64FBGetFrameBufferInfo(void *p)                    { api().FBGetFrameBufferInfo(p); }

/* No GL context loss on Switch — safe no-ops */
EXPORT void CALL gln64DestroyGfxContext(void) {}
EXPORT void CALL gln64ReinitGfxContext(void)  {}

/* osal_keys — no keyboard on Switch */
EXPORT void         CALL osal_keys_init(void)                              {}
EXPORT void         CALL osal_keys_quit(void)                              {}
EXPORT void         CALL osal_keys_update_state(void)                      {}
EXPORT unsigned int CALL osal_is_key_pressed(unsigned int k, unsigned int m) { return 0; }

} /* extern "C" */
