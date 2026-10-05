/// @file audio_tico.c
/// @brief The core's audio output (AI DMA) straight to the frontend.
///
/// The AI hands over each buffer the game plays, at the game's own rate; the
/// frontend resamples (TicoAudio). Samples arrive as big-endian halfword
/// pairs with the channels swapped, so each frame is put in native order
/// here.

#include <stddef.h>
#include <stdint.h>

#include "device/rcp/ai/ai_controller.h"
#include "device/rcp/vi/vi_controller.h"

void tico_m64p_audio_rate(unsigned rate);
void tico_m64p_audio_frames(const int16_t *frames, size_t count);

void tico_m64p_audio_set_format(void *user_data, unsigned int frequency)
{
   (void)user_data;
   tico_m64p_audio_rate(frequency);
}

void tico_m64p_audio_push_samples(void *user_data, const void *buffer, size_t size)
{
   (void)user_data;
   /* the core's own buffer in RDRAM: swap in place, as the core did before */
   uint8_t *p = (uint8_t *)buffer;
   for (size_t i = 0; i + 3 < size; i += 4)
   {
      uint8_t t = p[i];
      p[i] = p[i + 2];
      p[i + 2] = t;
      t = p[i + 1];
      p[i + 1] = p[i + 3];
      p[i + 3] = t;
   }
   tico_m64p_audio_frames((const int16_t *)buffer, size / 4);
}
