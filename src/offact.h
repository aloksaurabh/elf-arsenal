/* Elf Arsenal — offline account activation.
   Vendored from ps5-payload-dev/offact (GPL-3.0). The original ships
   an SDL2 fullscreen UI; we keep only the registry side-effects and
   drive them from the web UI. */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define OFFACT_SLOT_COUNT      16
#define OFFACT_NAME_MAX        32
#define OFFACT_TYPE_MAX        17
#define OFFACT_DEFAULT_FLAGS   0x1002

int      offact_get_name(int slot, char out[OFFACT_NAME_MAX]);
int      offact_set_name(int slot, const char *name);
int      offact_get_id(int slot, uint64_t *out);
int      offact_set_id(int slot, uint64_t id);
int      offact_get_type(int slot, char out[OFFACT_TYPE_MAX]);
int      offact_set_type(int slot, const char *type);
int      offact_get_flags(int slot, int *out);
int      offact_set_flags(int slot, int flags);
uint64_t offact_gen_id(const char *name);

int offact_activate(int slot, uint64_t id);

/* Zero out id+flags (leaves the name and type untouched so the slot
   is still recognisable but no longer "activated"). */
int offact_clear(int slot);
