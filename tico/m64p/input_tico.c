/// @file input_tico.c
/// @brief The core's input plugin: N64 pads as the frontend last set them.
///
/// The frontend maps the Switch controllers to N64 buttons and stick
/// positions (tico_m64p_set_pad) and the core reads them here on every VI.
/// The rumble pak is a raw pak: writes to its motor address reach the
/// frontend's rumble callback.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "api/m64p_plugin.h"
#include "api/m64p_types.h"
#include "plugin/plugin.h"

void tico_m64p_rumble(unsigned port, bool on);

#define RD_READPAK   0x02
#define RD_WRITEPAK  0x03
#define PAK_IO_RUMBLE 0xC000

static CONTROL *s_controls[4];
static int s_paks[4] = {PLUGIN_MEMPAK, PLUGIN_NONE, PLUGIN_NONE, PLUGIN_NONE};

/* written by the frontend's thread, read on the emulation thread; one word
 * per pad so a read never sees half an update */
static volatile uint32_t s_pads[4];
static volatile bool s_present[4] = {true, false, false, false};

void tico_m64p_set_pad(unsigned port, bool present, uint32_t buttons, int8_t stick_x, int8_t stick_y)
{
   if (port >= 4)
      return;
   BUTTONS keys;
   keys.Value = 0;
   keys.Value = buttons & 0xFFFF;
   keys.X_AXIS = stick_x;
   keys.Y_AXIS = stick_y;
   s_pads[port] = keys.Value;
   s_present[port] = present;
   if (s_controls[port])
      s_controls[port]->Present = present;
}

void tico_m64p_input_set_paks(const int paks[4])
{
   for (int i = 0; i < 4; i++)
   {
      s_paks[i] = paks[i];
      if (s_controls[i])
         s_controls[i]->Plugin = paks[i];
   }
}

m64p_error inputPluginGetVersion(m64p_plugin_type *type, int *version, int *api_version, const char **name,
                                 int *capabilities)
{
   if (type)
      *type = M64PLUGIN_INPUT;
   if (version)
      *version = 0x010000;
   if (api_version)
      *api_version = INPUT_API_VERSION;
   if (name)
      *name = "tico";
   if (capabilities)
      *capabilities = 0;
   return M64ERR_SUCCESS;
}

static unsigned char DataCRC(unsigned char *data, int length)
{
   unsigned char remainder = data[0];
   int byte = 1;
   unsigned char bit = 0;
   while (byte <= length)
   {
      int high_bit = (remainder & 0x80) != 0;
      remainder = remainder << 1;
      remainder += (byte < length && data[byte] & (0x80 >> bit)) ? 1 : 0;
      remainder ^= high_bit ? 0x85 : 0;
      bit++;
      byte += bit / 8;
      bit %= 8;
   }
   return remainder;
}

void inputControllerCommand(int control, unsigned char *command)
{
   unsigned char *data = &command[5];
   if (control < 0 || control >= 4 || !s_controls[control] || s_controls[control]->Plugin != PLUGIN_RAW)
      return;

   const unsigned address = (command[3] << 8) + (command[4] & 0xE0);
   switch (command[2])
   {
   case RD_READPAK:
      memset(data, address >= 0x8000 && address < 0x9000 ? 0x80 : 0x00, 32);
      data[32] = DataCRC(data, 32);
      break;
   case RD_WRITEPAK:
      data[32] = DataCRC(data, 32);
      if (address == PAK_IO_RUMBLE)
         tico_m64p_rumble((unsigned)control, *data != 0);
      break;
   default:
      break;
   }
}

void inputGetKeys_default(int control, BUTTONS *keys)
{
   keys->Value = (control >= 0 && control < 4) ? s_pads[control] : 0;
}

void inputInitiateControllers(CONTROL_INFO info)
{
   for (int i = 0; i < 4; i++)
   {
      s_controls[i] = info.Controls + i;
      s_controls[i]->Present = s_present[i];
      s_controls[i]->RawData = 0;
      s_controls[i]->Plugin = s_paks[i];
   }
}

void inputReadController(int control, unsigned char *command)
{
   inputControllerCommand(control, command);
}

void inputRomClosed(void)
{
}

int inputRomOpen(void)
{
   return 1;
}
