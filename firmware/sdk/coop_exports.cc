/*
 * Core symbols a coop plugin file may import, by ELF name (with the C underscore): the loader (plugin_load.cc)
 * resolves a file's import table against this list and refuses a file that imports anything else. Append-only:
 * never remove or change an entry a released plugin uses (modelled on kennel's kn_exports.c).
 *
 * Milestone P1: nothing yet. The first plugins carry their own DSP and their own buffers.
 *
 * Copyright (C) 2026 coldsoldr. GPL-3.0-or-later with the OP-1 firmware permission.
 */
#include <stdint.h>
#include "../core/sdram/plugin_load.h"

extern "C" const kn_export coop_exports[] = {
    { "_coop_exports_end", 0 },                      /* a placeholder so the table is never empty */
};
extern "C" const int coop_exports_count = 0;          /* entries a plugin may use (the placeholder is not one) */
