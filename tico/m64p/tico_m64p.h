/// @file tico_m64p.h
/// @brief The tico frontend's handle on mupen64plus: no libretro in between.
///
/// The core, its plugins (GLideN64 or paraLLEl-RDP for video, the HLE or
/// paraLLEl RSP) and this glue are linked statically. The frontend fills in
/// tico_m64p_callbacks, loads a ROM and starts the emulation thread, which
/// runs until tico_m64p_stop(). Each frame the plugins scan out reaches the
/// frontend on that thread through present_vulkan / present_gl.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tico_m64p_renderer
{
   /* paraLLEl-RDP and the paraLLEl RSP, on the frontend's Vulkan device */
   TICO_M64P_RENDERER_PARALLEL = 0,
   /* GLideN64 and the HLE RSP, on the emulation thread's GL context */
   TICO_M64P_RENDERER_GLIDEN64,
};

/* N64 controller buttons, as BUTTONS.Value in m64p_plugin.h */
enum
{
   TICO_N64_DPAD_RIGHT = 0x0001,
   TICO_N64_DPAD_LEFT  = 0x0002,
   TICO_N64_DPAD_DOWN  = 0x0004,
   TICO_N64_DPAD_UP    = 0x0008,
   TICO_N64_START      = 0x0010,
   TICO_N64_Z          = 0x0020,
   TICO_N64_B          = 0x0040,
   TICO_N64_A          = 0x0080,
   TICO_N64_C_RIGHT    = 0x0100,
   TICO_N64_C_LEFT     = 0x0200,
   TICO_N64_C_DOWN     = 0x0400,
   TICO_N64_C_UP       = 0x0800,
   TICO_N64_R          = 0x1000,
   TICO_N64_L          = 0x2000,
};

struct tico_m64p_memory_region;
struct tico_vk_interface;
struct tico_vk_context;

typedef struct tico_m64p_callbacks
{
   /* 0 debug, 1 info, 2 warning, 3 error; msg ends in a newline */
   void (*log)(int level, const char *msg);

   /* A core option by its key (mupen64plus-*), or NULL for its default.
    * Read when the ROM loads and on tico_m64p_apply_options(). */
   const char *(*get_option)(const char *key);

   /* Audio from the emulation thread: the game's own rate, then
    * interleaved stereo frames at that rate. */
   void (*audio_rate)(unsigned rate);
   void (*audio_samples)(const int16_t *frames, size_t count);

   /* once per VI on the emulation thread, before the pads are read */
   void (*input_poll)(void);
   void (*rumble)(unsigned port, bool on);

   /* A frame is ready (emulation thread). paraLLEl: its image went to the
    * interface's set_image; GLideN64: it is in the default framebuffer and
    * the emulation thread's GL context is current. */
   void (*present_vulkan)(unsigned width, unsigned height);
   void (*present_gl)(unsigned width, unsigned height);

   /* GLideN64 only: the framebuffer it renders into, created on the
    * emulation thread's context, and the GL entry points. */
   unsigned (*gl_default_framebuffer)(unsigned width, unsigned height);
   void *(*gl_get_proc_address)(const char *name);

   /* The emulation thread starting and ending, on that thread: e.g. make a
    * GL context current there and release it again. */
   void (*emu_thread_begin)(void);
   void (*emu_thread_end)(void);
} tico_m64p_callbacks;

/* Starts the core. system_dir holds mupen64plus.ini, the 64DD IPL and the
 * GLideN64 caches (a Mupen64plus/ folder is made inside it). */
bool tico_m64p_init(const tico_m64p_callbacks *callbacks, const char *system_dir);
void tico_m64p_shutdown(void);

/* The renderer the next ROM loads with. */
void tico_m64p_set_renderer(enum tico_m64p_renderer renderer);
enum tico_m64p_renderer tico_m64p_renderer(void);

/* paraLLEl-RDP: create the Vulkan device it needs on the frontend's
 * instance and surface, then hand back the interface it renders through.
 * Both before tico_m64p_load_rom(). */
bool tico_m64p_vulkan_create_device(struct tico_vk_context *context, void *instance, void *gpu,
                                    void *surface, void *get_instance_proc_addr,
                                    const char **device_extensions, unsigned num_device_extensions);
void tico_m64p_vulkan_set_interface(const struct tico_vk_interface *iface);

/* Loads a ROM (any byte order) and starts the plugins. The data is copied. */
bool tico_m64p_load_rom(const void *data, size_t size);
/* The game's internal name and region, after loading */
const char *tico_m64p_rom_name(void);
bool tico_m64p_rom_is_pal(void);

/* Re-reads the options that may change while a game runs. */
void tico_m64p_apply_options(void);

/* The emulation thread. Stop waits for it to exit and closes the ROM. */
bool tico_m64p_start(void);
void tico_m64p_stop(void);
bool tico_m64p_running(void);

void tico_m64p_reset(bool hard);

/* Save states, in memory. The emulation thread takes them at its next safe
 * point, so it must be running (not blocked on the frontend) until these
 * return. */
size_t tico_m64p_state_size(void);
bool tico_m64p_save_state(void *data, size_t size);
bool tico_m64p_load_state(const void *data, size_t size);

/* GameShark codes: "XXXXXXXX YYYY" lines. Enabled cheats apply from the next
 * VI; call with the emulation thread blocked. */
void tico_m64p_cheats_clear(void);
bool tico_m64p_cheat_add(const char *name, const char *codes);

/* A controller, from the frontend's thread. stick_x/y in -80..80. */
void tico_m64p_set_pad(unsigned port, bool present, uint32_t buttons, int8_t stick_x, int8_t stick_y);

/* The cartridge and pak saves, in memory: load before tico_m64p_start(),
 * write back after tico_m64p_stop() (and periodically). */
void *tico_m64p_save_memory(size_t *size);
/* offsets and sizes of each kind of save in it */
void tico_m64p_save_layout(size_t *eeprom, size_t *eeprom_size, size_t *mempak, size_t *mempak_size,
                           size_t *sram, size_t *sram_size, size_t *flashram, size_t *flashram_size);

/* Memory the core exposes at its physical addresses (RDRAM from 0). */
const struct tico_m64p_memory_region *tico_m64p_memory_regions(unsigned *count);

/* Video: the size GLideN64 renders at, the aspect ratio the core reports and
 * the VI rate (60 NTSC, 50 PAL). */
void tico_m64p_screen_size(unsigned *width, unsigned *height);
float tico_m64p_aspect_ratio(void);
double tico_m64p_refresh_rate(void);

/* The emulation thread, for profiling (Horizon handle), 0 when not running */
uint32_t tico_m64p_emu_thread_handle(void);

#ifdef __cplusplus
}
#endif
