#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "arc.h"

#define ROM_MAX_SIZE    ARC_ROM_MAX

/* Load the ROM of `os` into buf (ROM_MAX_SIZE bytes): from its flash
 * partition (rom_arthur120, rom_riscos200, rom_riscos311: roms/arthur120.rom,
 * riscos200.rom, riscos311.rom, written by idf.py flash), else from a file
 * in the SD card's "roms" folder that holds that OS. Its size in *size, its
 * version ("RISC OS 3.11 (29 Sep 1992)") in *name.
 */
bool rom_store_load(arc_os_t os, uint8_t *buf, uint32_t *size, const char **name);
