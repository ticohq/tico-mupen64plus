/// @file tico_m64p.c
/// @brief mupen64plus driven directly by the tico frontend. See tico_m64p.h.

#include "tico_m64p.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_LIBNX
#include <switch.h>
#endif

#include "api/callbacks.h"
#include "api/m64p_config.h"
#include "api/m64p_frontend.h"
#include "api/m64p_types.h"
#include "device/device.h"
#include "device/r4300/r4300_core.h"
#include "main/cheat.h"
#include "main/main.h"
#include "main/mupen64plus.ini.h"
#include "main/rom.h"
#include "main/savestates.h"
#include "main/util.h"
#include "main/version.h"
#include "plugin/plugin.h"

#include <mupen64plus-next_common.h>
#include <tico_m64p_memory.h>

#ifdef HAVE_PARALLEL_RDP
#include "../../mupen64plus-video-paraLLEl/parallel.h"
#endif

/* ------------------------------------------------------------------------
 * Globals the core and plugins read (declared in mupen64plus-next_common.h)
 * ---------------------------------------------------------------------- */

save_memory_data saved_memory;

bool tico_savestate_complete = true;
int tico_savestate_result = 0;

char *tico_dd_path_img = NULL;
char *tico_dd_path_rom = NULL;
char *tico_transferpak_rom_path = NULL;
char *tico_transferpak_ram_path = NULL;

uint32_t bilinearMode = 0;
uint32_t EnableHybridFilter = 0;
uint32_t EnableDitheringPattern = 0;
uint32_t RDRAMImageDitheringMode = 0;
uint32_t EnableDitheringQuantization = 0;
uint32_t EnableHWLighting = 0;
uint32_t CorrectTexrectCoords = 0;
uint32_t EnableTexCoordBounds = 0;
uint32_t EnableInaccurateTextureCoordinates = 0;
uint32_t enableNativeResTexrects = 0;
uint32_t enableLegacyBlending = 0;
uint32_t EnableCopyColorToRDRAM = 0;
uint32_t EnableCopyColorFromRDRAM = 0;
uint32_t EnableCopyDepthToRDRAM = 0;
uint32_t AspectRatio = 1;
uint32_t MaxTxCacheSize = 0;
uint32_t MaxHiResTxVramLimit = 0;
uint32_t txFilterMode = 0;
uint32_t txEnhancementMode = 0;
uint32_t txHiresEnable = 0;
uint32_t txHiresFullAlphaChannel = 0;
uint32_t txFilterIgnoreBG = 0;
uint32_t EnableFXAA = 0;
uint32_t MultiSampling = 0;
uint32_t EnableFragmentDepthWrite = 0;
uint32_t EnableShadersStorage = 0;
uint32_t EnableTextureCache = 0;
uint32_t EnableFBEmulation = 0;
uint32_t EnableFrameDuping = 0;
uint32_t EnableLODEmulation = 0;
uint32_t BackgroundMode = 0; /* 0 is bgOnePiece */
uint32_t EnableEnhancedTextureStorage = 0;
uint32_t EnableHiResAltCRC = 0;
uint32_t EnableEnhancedHighResStorage = 0;
uint32_t EnableTxCacheCompression = 0;
uint32_t EnableNativeResFactor = 0;
uint32_t EnableN64DepthCompare = 0;
/* GLideN64's threaded GL wrapper is never used: its GL runs on the
 * emulation thread, whose context the frontend makes current there. */
uint32_t EnableThreadedRenderer = 0;
uint32_t EnableCopyAuxToRDRAM = 0;
uint32_t GLideN64IniBehaviour = 0;

uint32_t EnableOverscan = 0;
uint32_t OverscanTop = 0;
uint32_t OverscanLeft = 0;
uint32_t OverscanRight = 0;
uint32_t OverscanBottom = 0;

uint32_t EnableFullspeed = 0;
uint32_t CountPerOp = 0;
uint32_t CountPerOpDenomPot = 0;
uint32_t CountPerScanlineOverride = 0;
uint32_t ForceDisableExtraMem = 0;
uint32_t IgnoreTLBExceptions = 0;

extern unsigned int r4300_emumode;
extern struct cheat_ctx g_cheat_ctx;
extern m64p_rom_header ROM_HEADER;
#ifdef HAVE_PARALLEL_RSP
extern bool parallel_rsp_hle_audio;
#endif

/* input_tico.c */
void tico_m64p_input_set_paks(const int paks[4]);

/* ------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------- */

static tico_m64p_callbacks s_cb;
static char s_system_dir[1024] = "./";
static enum tico_m64p_renderer s_renderer = TICO_M64P_RENDERER_PARALLEL;
static bool s_core_started = false;
static bool s_rom_open = false;
static bool s_plugins_connected = false;

static uint32_t s_screen_width = 640;
static uint32_t s_screen_height = 480;
static float s_screen_aspect = 4.0f / 3.0f;

static pthread_t s_emu_thread;
static bool s_emu_thread_started = false;
static volatile bool s_emu_running = false;
#ifdef HAVE_LIBNX
static Handle s_emu_thread_handle = 0;
#endif

static struct tico_m64p_memory_region s_regions[32];
static unsigned s_region_count = 0;

uint32_t m64p_screen_width(void) { return s_screen_width; }
uint32_t m64p_screen_height(void) { return s_screen_height; }

/* ------------------------------------------------------------------------
 * Hooks the core and plugins call
 * ---------------------------------------------------------------------- */

void tico_m64p_log(enum tico_log_level level, const char *fmt, ...)
{
   char buffer[1024];
   va_list args;
   va_start(args, fmt);
   vsnprintf(buffer, sizeof(buffer), fmt, args);
   va_end(args);
   if (s_cb.log)
      s_cb.log((int)level, buffer);
}

const char *tico_m64p_system_dir(void)
{
   return s_system_dir;
}

void tico_m64p_input_poll(void)
{
   if (s_cb.input_poll)
      s_cb.input_poll();
}

void tico_m64p_present_vulkan(unsigned width, unsigned height)
{
   if (s_cb.present_vulkan)
      s_cb.present_vulkan(width, height);
}

void tico_m64p_present_gl(void)
{
   if (s_cb.present_gl)
      s_cb.present_gl(s_screen_width, s_screen_height);
}

unsigned tico_m64p_gl_default_framebuffer(void)
{
   return s_cb.gl_default_framebuffer ? s_cb.gl_default_framebuffer(s_screen_width, s_screen_height) : 0;
}

void *tico_m64p_gl_get_proc_address(const char *name)
{
   return s_cb.gl_get_proc_address ? s_cb.gl_get_proc_address(name) : NULL;
}

void tico_m64p_set_memory_regions(const struct tico_m64p_memory_region *regions, unsigned count)
{
   if (count > sizeof(s_regions) / sizeof(s_regions[0]))
      count = sizeof(s_regions) / sizeof(s_regions[0]);
   memcpy(s_regions, regions, count * sizeof(*regions));
   s_region_count = count;
}

/* used by audio_tico.c and input_tico.c */
void tico_m64p_audio_rate(unsigned rate)
{
   if (s_cb.audio_rate)
      s_cb.audio_rate(rate);
}

void tico_m64p_audio_frames(const int16_t *frames, size_t count)
{
   if (s_cb.audio_samples)
      s_cb.audio_samples(frames, count);
}

void tico_m64p_rumble(unsigned port, bool on)
{
   if (s_cb.rumble)
      s_cb.rumble(port, on);
}

/* ------------------------------------------------------------------------
 * Options: the same keys and values the core has always read
 * ---------------------------------------------------------------------- */

static const char *opt(const char *key)
{
   return s_cb.get_option ? s_cb.get_option(key) : NULL;
}

static bool opt_is(const char *key, const char *value)
{
   const char *v = opt(key);
   return v && !strcmp(v, value);
}

/* A "False"/"True" option: true unless it reads `off_value`. */
static uint32_t opt_flag(const char *key, const char *off_value, uint32_t fallback)
{
   const char *v = opt(key);
   return v ? (strcmp(v, off_value) ? 1 : 0) : fallback;
}

static uint32_t opt_int(const char *key, uint32_t fallback)
{
   const char *v = opt(key);
   return v ? (uint32_t)atoi(v) : fallback;
}

static int pak_from_option(const char *key)
{
   const char *v = opt(key);
   if (!v)
      return PLUGIN_NONE;
   if (!strcmp(v, "rumble"))
      return PLUGIN_RAW;
   if (!strcmp(v, "memory"))
      return PLUGIN_MEMPAK;
   if (!strcmp(v, "transfer"))
      return PLUGIN_TRANSFER_PAK;
   return PLUGIN_NONE;
}

static void apply_controller_options(void)
{
   const int paks[4] = {
      pak_from_option(CORE_NAME "-pak1"), pak_from_option(CORE_NAME "-pak2"),
      pak_from_option(CORE_NAME "-pak3"), pak_from_option(CORE_NAME "-pak4"),
   };
   tico_m64p_input_set_paks(paks);
}

/* Read once, before the ROM opens: everything the plugins only look at when
 * they start. */
static void apply_startup_options(void)
{
   const char *v;
   const char *screen_size_key = CORE_NAME "-43screensize";

   if (s_renderer == TICO_M64P_RENDERER_PARALLEL)
   {
#if defined(HAVE_PARALLEL_RDP) && defined(HAVE_PARALLEL_RSP)
      plugin_connect_rdp_api(RDP_PLUGIN_PARALLEL);
      plugin_connect_rsp_api(RSP_PLUGIN_PARALLEL);
      parallel_rsp_hle_audio = !opt_is(CORE_NAME "-parallel-rsp-hle-audio", "False");
#else
      tico_m64p_log(TICO_LOG_ERROR, "paraLLEl is not built in, using GLideN64\n");
      s_renderer = TICO_M64P_RENDERER_GLIDEN64;
#endif
   }
   if (s_renderer == TICO_M64P_RENDERER_GLIDEN64)
   {
      plugin_connect_rdp_api(RDP_PLUGIN_GLIDEN64);
      plugin_connect_rsp_api(RSP_PLUGIN_HLE);
   }

   bilinearMode = opt_is(CORE_NAME "-BilinearMode", "3point") ? 0 : 1;
   EnableHybridFilter = opt_flag(CORE_NAME "-HybridFilter", "False", 1);
   EnableDitheringPattern = opt_flag(CORE_NAME "-DitheringPattern", "False", 0);
   EnableDitheringQuantization = opt_flag(CORE_NAME "-DitheringQuantization", "False", 1);
   v = opt(CORE_NAME "-RDRAMImageDitheringMode");
   if (v)
      RDRAMImageDitheringMode = !strcmp(v, "BlueNoise") ? 2 : (!strcmp(v, "MagicSquare") || !strcmp(v, "Bayer")) ? 1 : 0;
   EnableFXAA = opt_int(CORE_NAME "-FXAA", 0);
   MultiSampling = opt_int(CORE_NAME "-MultiSampling", 0);
   EnableFrameDuping = opt_flag(CORE_NAME "-FrameDuping", "False", 1);
   EnableFullspeed = opt_is(CORE_NAME "-Framerate", "Fullspeed") ? 1 : 0;
   v = opt(CORE_NAME "-virefresh");
   CountPerScanlineOverride = (!v || !strcmp(v, "Auto")) ? 0 : (uint32_t)atoi(v);
   EnableLODEmulation = opt_flag(CORE_NAME "-EnableLODEmulation", "False", 1);
   EnableFBEmulation = opt_flag(CORE_NAME "-EnableFBEmulation", "False", 1);
   v = opt(CORE_NAME "-EnableN64DepthCompare");
   EnableN64DepthCompare = !v ? 0 : !strcmp(v, "Compatible") ? 2 : !strcmp(v, "True") ? 1 : 0;
   v = opt(CORE_NAME "-EnableCopyColorToRDRAM");
   EnableCopyColorToRDRAM = !v ? 2 : !strcmp(v, "TripleBuffer") ? 3 : !strcmp(v, "Async") ? 2 : !strcmp(v, "Sync") ? 1 : 0;
   EnableCopyColorFromRDRAM = opt_flag(CORE_NAME "-EnableCopyColorFromRDRAM", "False", 0);
   v = opt(CORE_NAME "-EnableCopyDepthToRDRAM");
   EnableCopyDepthToRDRAM = !v ? 2 : !strcmp(v, "Software") ? 2 : !strcmp(v, "FromMem") ? 1 : 0;
   EnableHWLighting = opt_flag(CORE_NAME "-EnableHWLighting", "False", 0);
   v = opt(CORE_NAME "-CorrectTexrectCoords");
   CorrectTexrectCoords = !v ? 0 : !strcmp(v, "Force") ? 2 : !strcmp(v, "Auto") ? 1 : 0;
   EnableTexCoordBounds = opt_flag(CORE_NAME "-EnableTexCoordBounds", "False", 0);
   EnableInaccurateTextureCoordinates = opt_flag(CORE_NAME "-EnableInaccurateTextureCoordinates", "False", 0);
   BackgroundMode = opt_is(CORE_NAME "-BackgroundMode", "Stripped") ? 1 : 0;
   v = opt(CORE_NAME "-EnableNativeResTexrects");
   enableNativeResTexrects = !v ? 0 : !strcmp(v, "Optimized") ? 1 : !strcmp(v, "Unoptimized") ? 2 : 0;

   v = opt(CORE_NAME "-txFilterMode");
   txFilterMode = 0;
   if (v)
   {
      static const char *const modes[] = {"Smooth filtering 1", "Smooth filtering 2", "Smooth filtering 3",
                                          "Smooth filtering 4", "Sharp filtering 1", "Sharp filtering 2"};
      for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
         if (!strcmp(v, modes[i]))
            txFilterMode = i + 1;
   }
   v = opt(CORE_NAME "-txEnhancementMode");
   txEnhancementMode = 0;
   if (v)
   {
      static const char *const modes[] = {"As Is", "X2", "X2SAI", "HQ2X", "HQ2XS", "LQ2X", "LQ2XS",
                                          "HQ4X", "2xBRZ", "3xBRZ", "4xBRZ", "5xBRZ", "6xBRZ"};
      for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
         if (!strcmp(v, modes[i]))
            txEnhancementMode = i + 1;
   }
   /* "Filter background textures": the option reads True to filter them */
   txFilterIgnoreBG = opt_is(CORE_NAME "-txFilterIgnoreBG", "False") ? 1 : 0;
   txHiresEnable = opt_flag(CORE_NAME "-txHiresEnable", "False", 0);
   EnableTxCacheCompression = opt_flag(CORE_NAME "-txCacheCompression", "False", 1);
   txHiresFullAlphaChannel = opt_flag(CORE_NAME "-txHiresFullAlphaChannel", "False", 0);
   MaxHiResTxVramLimit = opt_int(CORE_NAME "-MaxHiResTxVramLimit", 0);
   MaxTxCacheSize = opt_int(CORE_NAME "-MaxTxCacheSize", 4000);
   enableLegacyBlending = opt_flag(CORE_NAME "-EnableLegacyBlending", "False", 0);
   EnableFragmentDepthWrite = opt_flag(CORE_NAME "-EnableFragmentDepthWrite", "False", 1);
   EnableShadersStorage = opt_flag(CORE_NAME "-EnableShadersStorage", "False", 1);
   EnableTextureCache = opt_flag(CORE_NAME "-EnableTextureCache", "False", 1);
   EnableEnhancedTextureStorage = opt_flag(CORE_NAME "-EnableEnhancedTextureStorage", "False", 0);
   EnableEnhancedHighResStorage = opt_flag(CORE_NAME "-EnableEnhancedHighResStorage", "False", 0);
   EnableHiResAltCRC = opt_flag(CORE_NAME "-EnableHiResAltCRC", "False", 0);
   EnableCopyAuxToRDRAM = opt_flag(CORE_NAME "-EnableCopyAuxToRDRAM", "False", 0);
   v = opt(CORE_NAME "-GLideN64IniBehaviour");
   GLideN64IniBehaviour = !v ? 0 : !strcmp(v, "early") ? 1 : !strcmp(v, "disabled") ? (uint32_t)-1 : 0;

   v = opt(CORE_NAME "-cpucore");
   r4300_emumode = EMUMODE_DYNAREC;
   if (v && !strcmp(v, "pure_interpreter"))
      r4300_emumode = EMUMODE_PURE_INTERPRETER;
   else if (v && !strcmp(v, "cached_interpreter"))
      r4300_emumode = EMUMODE_INTERPRETER;

   v = opt(CORE_NAME "-aspect");
   if (v && !strcmp(v, "16:9 adjusted"))
   {
      AspectRatio = 3; /* Aspect::aAdjust */
      screen_size_key = CORE_NAME "-169screensize";
   }
   else if (v && !strcmp(v, "16:9"))
   {
      AspectRatio = 0; /* Aspect::aStretch */
      screen_size_key = CORE_NAME "-169screensize";
   }
   else
   {
      AspectRatio = 1; /* Aspect::a43 */
      s_screen_aspect = 4.0f / 3.0f;
   }

   EnableNativeResFactor = opt_int(CORE_NAME "-EnableNativeResFactor", 0);
   v = opt(screen_size_key);
   if (v)
   {
      unsigned w = 0, h = 0;
      if (sscanf(v, "%ux%u", &w, &h) == 2 && w && h)
      {
         s_screen_width = w;
         s_screen_height = h;
      }
   }
   if (AspectRatio != 1)
      s_screen_aspect = (float)s_screen_width / (float)s_screen_height;
   /* GLideN64 would blit a bigger image onto these small framebuffers */
   if ((s_screen_width == 320 && s_screen_height == 240) || (s_screen_width == 640 && s_screen_height == 360))
      EnableNativeResFactor = !EnableNativeResFactor ? 1 : EnableNativeResFactor;

   CountPerOp = opt_int(CORE_NAME "-CountPerOp", 0);
   CountPerOpDenomPot = opt_int(CORE_NAME "-CountPerOpDenomPot", 0);
   if (EnableFullspeed)
   {
      CountPerOp = 1;
      if (s_renderer == TICO_M64P_RENDERER_GLIDEN64 && !EnableFBEmulation)
         EnableFrameDuping = 1;
   }

   EnableOverscan = opt_is(CORE_NAME "-EnableOverscan", "Enabled") ? 1 : 0;
   OverscanTop = opt_int(CORE_NAME "-OverscanTop", 0);
   OverscanLeft = opt_int(CORE_NAME "-OverscanLeft", 0);
   OverscanRight = opt_int(CORE_NAME "-OverscanRight", 0);
   OverscanBottom = opt_int(CORE_NAME "-OverscanBottom", 0);
   ForceDisableExtraMem = opt_flag(CORE_NAME "-ForceDisableExtraMem", "False", 0);
   v = opt(CORE_NAME "-IgnoreTLBExceptions");
   IgnoreTLBExceptions = !v ? 0 : !strcmp(v, "OnlyNotEnabled") ? 1 : !strcmp(v, "AlwaysIgnoreTLB") ? 2 : 0;
}

#ifdef HAVE_PARALLEL_RDP
static bool parallel_bool(const char *key, bool fallback)
{
   const char *v = opt(key);
   return v ? !strcmp(v, "True") : fallback;
}

static void apply_parallel_options(void)
{
   const char *v;
   parallel_set_synchronous_rdp(parallel_bool(CORE_NAME "-parallel-rdp-synchronous", true));
   v = opt(CORE_NAME "-parallel-rdp-overscan");
   parallel_set_overscan_crop(v ? (unsigned)strtol(v, NULL, 0) : 0);
   parallel_set_divot_filter(parallel_bool(CORE_NAME "-parallel-rdp-divot-filter", true));
   parallel_set_gamma_dither(parallel_bool(CORE_NAME "-parallel-rdp-gamma-dither", true));
   parallel_set_vi_aa(parallel_bool(CORE_NAME "-parallel-rdp-vi-aa", true));
   parallel_set_vi_scale(parallel_bool(CORE_NAME "-parallel-rdp-vi-bilinear", true));
   parallel_set_dither_filter(parallel_bool(CORE_NAME "-parallel-rdp-dither-filter", true));
   v = opt(CORE_NAME "-parallel-rdp-upscaling");
   parallel_set_upscaling(v ? (unsigned)strtol(v, NULL, 0) : 1,
                          parallel_bool(CORE_NAME "-parallel-rdp-super-sampled-read-back", false));
   parallel_set_super_sampled_read_back_dither(
      parallel_bool(CORE_NAME "-parallel-rdp-super-sampled-read-back-dither", true));
   v = opt(CORE_NAME "-parallel-rdp-downscaling");
   parallel_set_downscaling_steps(!v ? 0 : !strcmp(v, "1/2") ? 1 : !strcmp(v, "1/4") ? 2 : !strcmp(v, "1/8") ? 3 : 0);
   parallel_set_native_texture_lod(parallel_bool(CORE_NAME "-parallel-rdp-native-texture-lod", false));
   parallel_set_native_tex_rect(parallel_bool(CORE_NAME "-parallel-rdp-native-tex-rect", true));
   parallel_set_interlacing(opt_is(CORE_NAME "-parallel-rdp-deinterlace-method", "Weave"));
}
#endif

void tico_m64p_apply_options(void)
{
#ifdef HAVE_PARALLEL_RDP
   if (s_renderer == TICO_M64P_RENDERER_PARALLEL)
      apply_parallel_options();
#endif
   apply_controller_options();
}

/* ------------------------------------------------------------------------
 * Core lifetime
 * ---------------------------------------------------------------------- */

static void debug_callback(void *context, int level, const char *message)
{
   (void)context;
   int tico_level = level == M64MSG_ERROR ? TICO_LOG_ERROR
                  : level == M64MSG_WARNING ? TICO_LOG_WARN
                  : level == M64MSG_VERBOSE ? TICO_LOG_DEBUG : TICO_LOG_INFO;
   tico_m64p_log((enum tico_log_level)tico_level, "%s\n", message);
}

static void state_callback(void *context, m64p_core_param param, int value)
{
   (void)context;
   if (param == M64CORE_STATE_LOADCOMPLETE || param == M64CORE_STATE_SAVECOMPLETE)
   {
      tico_savestate_result = value;
      __atomic_store_n(&tico_savestate_complete, true, __ATOMIC_RELEASE);
   }
}

static void format_saved_memory(void)
{
   format_sram(saved_memory.sram);
   format_eeprom(saved_memory.eeprom, EEPROM_MAX_SIZE);
   format_flashram(saved_memory.flashram);
   for (int i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
   {
      uint32_t serial[6];
      for (int k = 0; k < 6; ++k)
         serial[k] = xoshiro256pp_next(&l_mpk_idgen);
      format_mempak(saved_memory.mempack + i * MEMPAK_SIZE, serial, DEFAULT_MEMPAK_DEVICEID,
                    DEFAULT_MEMPAK_BANKS, DEFAULT_MEMPAK_VERSION);
   }
}

static void make_dirs(const char *path)
{
   char buffer[1024];
   snprintf(buffer, sizeof(buffer), "%s", path);
   const char *root = strstr(buffer, ":/");
   for (char *p = buffer + (root ? (root - buffer) + 2 : 1); *p; p++)
   {
      if (*p != '/')
         continue;
      *p = 0;
      mkdir(buffer, 0777);
      *p = '/';
   }
   mkdir(buffer, 0777);
}

bool tico_m64p_init(const tico_m64p_callbacks *callbacks, const char *system_dir)
{
   if (s_core_started)
      return true;
   if (callbacks)
      s_cb = *callbacks;

   size_t n = strlen(system_dir ? system_dir : "");
   snprintf(s_system_dir, sizeof(s_system_dir), "%s%s", n ? system_dir : "./",
            n && system_dir[n - 1] != '/' ? "/" : "");

   /* mupen64plus.ini and the plugins' caches live in <system>/Mupen64plus/ */
   char data_dir[1100];
   snprintf(data_dir, sizeof(data_dir), "%sMupen64plus/", s_system_dir);
   make_dirs(data_dir);
   char ini_path[1200];
   snprintf(ini_path, sizeof(ini_path), "%smupen64plus.ini", data_dir);
   FILE *ini = fopen(ini_path, "w");
   if (ini)
   {
      fputs(inifile, ini);
      fclose(ini);
   }

   m64p_error ret = CoreStartup(FRONTEND_API_VERSION, data_dir, data_dir, NULL, debug_callback, NULL, state_callback);
   if (ret != M64ERR_SUCCESS)
   {
      tico_m64p_log(TICO_LOG_ERROR, "CoreStartup failed (%d)\n", (int)ret);
      return false;
   }
   s_core_started = true;
   return true;
}

void tico_m64p_shutdown(void)
{
   tico_m64p_stop();
   if (s_core_started)
      CoreShutdown();
   s_core_started = false;
}

void tico_m64p_set_renderer(enum tico_m64p_renderer renderer)
{
   s_renderer = renderer;
}

enum tico_m64p_renderer tico_m64p_renderer(void)
{
   return s_renderer;
}

bool tico_m64p_vulkan_create_device(struct tico_vk_context *context, void *instance, void *gpu, void *surface,
                                    void *get_instance_proc_addr, const char **device_extensions,
                                    unsigned num_device_extensions)
{
#ifdef HAVE_PARALLEL_RDP
   VkPhysicalDeviceFeatures features;
   memset(&features, 0, sizeof(features));
   return parallel_create_device(context, (VkInstance)instance, (VkPhysicalDevice)gpu, (VkSurfaceKHR)surface,
                                 (PFN_vkGetInstanceProcAddr)get_instance_proc_addr, device_extensions,
                                 num_device_extensions, NULL, 0, &features);
#else
   (void)context; (void)instance; (void)gpu; (void)surface; (void)get_instance_proc_addr;
   (void)device_extensions; (void)num_device_extensions;
   return false;
#endif
}

void tico_m64p_vulkan_set_interface(const struct tico_vk_interface *iface)
{
#ifdef HAVE_PARALLEL_RDP
   parallel_set_vulkan_interface(iface);
#else
   (void)iface;
#endif
}

bool tico_m64p_load_rom(const void *data, size_t size)
{
   if (!s_core_started || s_rom_open)
      return false;

   tico_savestate_complete = true;
   format_saved_memory();
   apply_startup_options();
   tico_m64p_apply_options();

   if (CoreDoCommand(M64CMD_ROM_OPEN, (int)size, (void *)data) != M64ERR_SUCCESS)
   {
      tico_m64p_log(TICO_LOG_ERROR, "M64CMD_ROM_OPEN failed\n");
      return false;
   }
   s_rom_open = true;
   if (CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(ROM_HEADER), &ROM_HEADER) != M64ERR_SUCCESS)
      tico_m64p_log(TICO_LOG_WARN, "M64CMD_ROM_GET_HEADER failed\n");

   if (!s_plugins_connected)
   {
      plugin_connect_all();
      s_plugins_connected = true;
   }
   return true;
}

const char *tico_m64p_rom_name(void)
{
   return ROM_PARAMS.headername;
}

bool tico_m64p_rom_is_pal(void)
{
   return ROM_PARAMS.systemtype == SYSTEM_PAL;
}

static void pin_to_core(int core)
{
#ifdef HAVE_LIBNX
   Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
   if (R_FAILED(rc))
      tico_m64p_log(TICO_LOG_WARN, "could not pin the emulation thread to core %d (0x%x)\n", core, rc);
#else
   (void)core;
#endif
}

static void *emu_thread(void *arg)
{
   (void)arg;
   /* core 1: the main thread (overlay, present) is on core 2 */
   pin_to_core(1);
#ifdef HAVE_LIBNX
   s_emu_thread_handle = threadGetCurHandle();
#endif
   if (s_cb.emu_thread_begin)
      s_cb.emu_thread_begin();

   s_emu_running = true;
   CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
   s_emu_running = false;

   if (s_cb.emu_thread_end)
      s_cb.emu_thread_end();
#ifdef HAVE_LIBNX
   s_emu_thread_handle = 0;
#endif
   return NULL;
}

bool tico_m64p_start(void)
{
   if (!s_rom_open || s_emu_thread_started)
      return false;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   /* the dynarec, the RDP enqueue and save states run on it */
   pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
   s_emu_thread_started = pthread_create(&s_emu_thread, &attr, emu_thread, NULL) == 0;
   pthread_attr_destroy(&attr);
   if (!s_emu_thread_started)
      tico_m64p_log(TICO_LOG_ERROR, "could not start the emulation thread\n");
   return s_emu_thread_started;
}

void tico_m64p_stop(void)
{
   if (s_emu_thread_started)
   {
      CoreDoCommand(M64CMD_STOP, 0, NULL);
      pthread_join(s_emu_thread, NULL);
      s_emu_thread_started = false;
   }
   if (s_rom_open)
   {
      CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
      s_rom_open = false;
   }
}

bool tico_m64p_running(void)
{
   return s_emu_running;
}

void tico_m64p_reset(bool hard)
{
   CoreDoCommand(M64CMD_RESET, hard ? 1 : 0, NULL);
}

/* ------------------------------------------------------------------------
 * Save states
 * ---------------------------------------------------------------------- */

size_t tico_m64p_state_size(void)
{
   return 16788288 + 1024 + 4 + 4096;
}

static bool run_state_job(savestates_job job, const void *data)
{
   if (!s_emu_running)
      return false;
   __atomic_store_n(&tico_savestate_complete, false, __ATOMIC_RELEASE);
   tico_savestate_result = 0;
   savestates_set_job(job, savestates_type_m64p, (const char *)data);
   /* the emulation thread takes it at its next interrupt */
   while (!__atomic_load_n(&tico_savestate_complete, __ATOMIC_ACQUIRE))
   {
      if (!s_emu_running)
         return false;
      usleep(1000);
   }
   return tico_savestate_result != 0;
}

bool tico_m64p_save_state(void *data, size_t size)
{
   if (size < tico_m64p_state_size())
      return false;
   return run_state_job(savestates_job_save, data);
}

bool tico_m64p_load_state(const void *data, size_t size)
{
   (void)size;
   return run_state_job(savestates_job_load, data);
}

/* ------------------------------------------------------------------------
 * Cheats
 * ---------------------------------------------------------------------- */

void tico_m64p_cheats_clear(void)
{
   cheat_delete_all(&g_cheat_ctx);
}

/* "XXXXXXXX YYYY" pairs, separated by spaces, '+', newlines or ';' */
bool tico_m64p_cheat_add(const char *name, const char *codes)
{
   m64p_cheat_code parsed[256];
   unsigned count = 0;
   uint32_t numbers[512];
   unsigned numbers_count = 0;
   const char *p = codes;

   while (*p && numbers_count < 512)
   {
      while (*p && !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
         p++;
      if (!*p)
         break;
      char *end = NULL;
      numbers[numbers_count++] = (uint32_t)strtoul(p, &end, 16);
      p = end;
   }
   for (unsigned i = 0; i + 1 < numbers_count && count < 256; i += 2)
   {
      parsed[count].address = numbers[i];
      parsed[count].value = (int)numbers[i + 1];
      count++;
   }
   if (!count)
      return false;
   if (!cheat_add_new(&g_cheat_ctx, name, parsed, (int)count))
      return false;
   return cheat_set_enabled(&g_cheat_ctx, name, 1) != 0;
}

/* ------------------------------------------------------------------------
 * Saves, memory, video
 * ---------------------------------------------------------------------- */

void *tico_m64p_save_memory(size_t *size)
{
   if (size)
      *size = sizeof(saved_memory);
   return &saved_memory;
}

void tico_m64p_save_layout(size_t *eeprom, size_t *eeprom_size, size_t *mempak, size_t *mempak_size,
                           size_t *sram, size_t *sram_size, size_t *flashram, size_t *flashram_size)
{
   *eeprom = offsetof(save_memory_data, eeprom);
   *eeprom_size = sizeof(saved_memory.eeprom);
   *mempak = offsetof(save_memory_data, mempack);
   *mempak_size = sizeof(saved_memory.mempack);
   *sram = offsetof(save_memory_data, sram);
   *sram_size = sizeof(saved_memory.sram);
   *flashram = offsetof(save_memory_data, flashram);
   *flashram_size = sizeof(saved_memory.flashram);
}

const struct tico_m64p_memory_region *tico_m64p_memory_regions(unsigned *count)
{
   if (count)
      *count = s_region_count;
   return s_regions;
}

void tico_m64p_screen_size(unsigned *width, unsigned *height)
{
   *width = s_screen_width;
   *height = s_screen_height;
}

float tico_m64p_aspect_ratio(void)
{
   return s_screen_aspect;
}

double tico_m64p_refresh_rate(void)
{
   return vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
}

uint32_t tico_m64p_emu_thread_handle(void)
{
#ifdef HAVE_LIBNX
   return s_emu_thread_handle;
#else
   return 0;
#endif
}

/* The GameShark button: there is none to press, so it stays released. */
static int s_gameshark_active = 0;

int event_gameshark_active(void)
{
   return s_gameshark_active;
}

void event_set_gameshark(int active)
{
   if (!active == !s_gameshark_active)
      return;
   s_gameshark_active = active ? 1 : 0;
   StateChanged(M64CORE_INPUT_GAMESHARK, s_gameshark_active);
}
