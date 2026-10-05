/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-Next - mupen64plus-next_common.h                          *
 *   Copyright (C) 2020 M4xw <m4x@m4xw.net>                                *
 *   Copyright (C) 2020 Daniel De Matteis                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef __M64P_NEXT_COMMON_H__
#define __M64P_NEXT_COMMON_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdbool.h>
#include <stdint.h>

#include "api/m64p_common.h"
#include "api/m64p_plugin.h"
#include "api/m64p_types.h"

enum rdp_plugin_type
{
   RDP_PLUGIN_NONE = 0,
   RDP_PLUGIN_GLIDEN64,
   RDP_PLUGIN_ANGRYLION,
   RDP_PLUGIN_PARALLEL,
   RDP_PLUGIN_MAX
};

enum rsp_plugin_type
{
   RSP_PLUGIN_NONE = 0,
   RSP_PLUGIN_HLE,
   RSP_PLUGIN_CXD4,
   RSP_PLUGIN_PARALLEL,
   RSP_PLUGIN_MAX
};

void plugin_connect_rsp_api(enum rsp_plugin_type type);
void plugin_connect_rdp_api(enum rdp_plugin_type type);
void plugin_connect_all();

uint32_t m64p_screen_width();
uint32_t m64p_screen_height();

extern enum rdp_plugin_type current_rdp_type;
extern enum rsp_plugin_type current_rsp_type;

/* ------------------------------------------------------------------------
 * The tico frontend (tico/m64p/tico_m64p.c). The core and the plugins call
 * these; tico_m64p.c forwards them to the frontend's callbacks.
 * ---------------------------------------------------------------------- */
enum tico_log_level
{
   TICO_LOG_DEBUG = 0,
   TICO_LOG_INFO,
   TICO_LOG_WARN,
   TICO_LOG_ERROR
};
void tico_m64p_log(enum tico_log_level level, const char *fmt, ...);

/* where the core looks for its own data (mupen64plus.ini, the 64DD IPL, the
 * GLideN64 shader and texture caches) */
const char *tico_m64p_system_dir(void);

/* on every VI, from the emulation thread, before the core reads the pads */
void tico_m64p_input_poll(void);

/* paraLLEl-RDP scanned out a frame (emulation thread) */
void tico_m64p_present_vulkan(unsigned width, unsigned height);
/* GLideN64 swapped buffers; the emulation thread's GL context is current */
void tico_m64p_present_gl(void);
/* the framebuffer GLideN64 draws its output into */
unsigned tico_m64p_gl_default_framebuffer(void);
void *tico_m64p_gl_get_proc_address(const char *name);

/* RDRAM and the cartridge as the RetroAchievements client reads them */
struct tico_m64p_memory_region
{
   void *ptr;
   uint32_t start;
   uint32_t length;
   bool read_only;
};
void tico_m64p_set_memory_regions(const struct tico_m64p_memory_region *regions, unsigned count);

// Misc Globals
extern CONTROL Controls[4];
extern struct xoshiro256pp_state l_mpk_idgen;

// Savestate globals
extern bool tico_savestate_complete;
extern int  tico_savestate_result;

// 64DD globals
extern char* tico_dd_path_img;
extern char* tico_dd_path_rom;

// Other Subsystems
extern char* tico_transferpak_rom_path;
extern char* tico_transferpak_ram_path;

// GLN64 context management
extern void gln64DestroyGfxContext(void);
extern void gln64ReinitGfxContext(void);

// Core options
// GLN64
extern uint32_t bilinearMode;
extern uint32_t EnableHybridFilter;
extern uint32_t EnableDitheringPattern;
extern uint32_t EnableDitheringQuantization;
extern uint32_t RDRAMImageDitheringMode;
extern uint32_t EnableHWLighting;
extern uint32_t CorrectTexrectCoords;
extern uint32_t EnableTexCoordBounds;
extern uint32_t EnableInaccurateTextureCoordinates;
extern uint32_t enableNativeResTexrects;
extern uint32_t enableLegacyBlending;
extern uint32_t EnableCopyColorToRDRAM;
extern uint32_t EnableCopyColorFromRDRAM;
extern uint32_t EnableCopyDepthToRDRAM;
extern uint32_t AspectRatio;
extern uint32_t MaxTxCacheSize;
extern uint32_t MaxHiResTxVramLimit;
extern uint32_t txFilterMode;
extern uint32_t txEnhancementMode;
extern uint32_t txHiresEnable;
extern uint32_t txHiresFullAlphaChannel;
extern uint32_t txFilterIgnoreBG;
extern uint32_t EnableFXAA;
extern uint32_t MultiSampling;
extern uint32_t EnableFragmentDepthWrite;
extern uint32_t EnableShadersStorage;
extern uint32_t EnableTextureCache;
extern uint32_t EnableFBEmulation;
extern uint32_t EnableFrameDuping;
extern uint32_t EnableLODEmulation;
extern uint32_t EnableFullspeed;
extern uint32_t CountPerOp;
extern uint32_t CountPerOpDenomPot;
extern uint32_t CountPerScanlineOverride;
extern uint32_t BackgroundMode;
extern uint32_t EnableEnhancedTextureStorage;
extern uint32_t EnableHiResAltCRC;
extern uint32_t EnableEnhancedHighResStorage;
extern uint32_t EnableTxCacheCompression;
extern uint32_t ForceDisableExtraMem;
extern uint32_t IgnoreTLBExceptions;
extern uint32_t EnableNativeResFactor;
extern uint32_t EnableN64DepthCompare;
extern uint32_t EnableThreadedRenderer;
extern uint32_t EnableCopyAuxToRDRAM;
extern uint32_t GLideN64IniBehaviour;

// Overscan Options
extern uint32_t EnableOverscan;
extern uint32_t OverscanTop;
extern uint32_t OverscanLeft;
extern uint32_t OverscanRight;
extern uint32_t OverscanBottom;

#ifndef CORE_NAME
#define CORE_NAME "mupen64plus"
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __M64P_NEXT_COMMON_H__ */
