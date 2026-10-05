#ifndef TICO_M64P_MEMORY_H
#define TICO_M64P_MEMORY_H

/* The cartridge and controller pak saves, kept in memory: the core reads and
 * writes them here, and the frontend loads them before the game starts and
 * writes them back to the saves folder (tico_m64p_save_memory()). */

#include <stdint.h>
#define EEPROM_MAX_SIZE 0x800
#include <device/controllers/paks/mempak.h>
#include <device/cart/flashram.h>
#include <device/cart/sram.h>

typedef struct _save_memory_data
{
   uint8_t eeprom[EEPROM_MAX_SIZE];
   uint8_t mempack[MEMPAK_SIZE * 4];
   uint8_t sram[SRAM_SIZE];
   uint8_t flashram[FLASHRAM_SIZE];
} save_memory_data;

extern save_memory_data saved_memory;

#endif
