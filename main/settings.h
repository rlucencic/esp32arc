#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "arc.h"

/* Machine settings kept in NVS (namespace "arc") */
typedef struct {
    uint8_t model;          /* arc_model_t */
    uint8_t os;             /* arc_os_t (the A3020: RISC OS 3) */
    uint16_t ram_kb;        /* 512, 1024, 2048, 4096 (the A3020: 2048, 4096) */
    uint8_t menu_middle;    /* the mouse's Menu button: 0 the right one, 1 the middle one */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* Each model's and OS's CMOS RAM (ARC_CMOS_SIZE bytes; they use it
 * differently): false if none was saved yet
 */
bool settings_load_cmos(arc_model_t model, arc_os_t os, uint8_t *cmos);
void settings_save_cmos(arc_model_t model, arc_os_t os, const uint8_t *cmos);
