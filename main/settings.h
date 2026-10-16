#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cpc.h"

/* Machine settings kept in NVS (namespace "cpc") */
typedef struct {
    uint8_t model;          /* cpc_model_t */
} settings_t;

extern settings_t settings;

void settings_load(void);
void settings_save(void);

/* The model's RAM in KB: 64 (464, 664, 464 Plus) or 128 (6128, 6128 Plus) */
uint16_t settings_ram_kb(void);
