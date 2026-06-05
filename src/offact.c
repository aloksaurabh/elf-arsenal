/* Elf Arsenal — offline account activation.
   Registry plumbing vendored from ps5-payload-dev/offact (GPL-3.0).
   Only the sceRegMgr* side-effects are kept; the SDL2 UI is dropped
   in favour of the existing web Settings panel. */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "offact.h"

int sceRegMgrGetInt(int, int*);
int sceRegMgrGetStr(int, char*, size_t);
int sceRegMgrGetBin(int, void*, size_t);
int sceRegMgrSetInt(int, int);
int sceRegMgrSetBin(int, const void*, size_t);
int sceRegMgrSetStr(int, const char*, size_t);


static int
slot_key(int slot, int base, int stride, int fallback) {
  if(slot < 1 || slot > OFFACT_SLOT_COUNT) return fallback;
  return (slot - 1) * stride + base;
}


#define KEY_NAME(s)   slot_key((s), 125829632, 65536, 127140352)
#define KEY_ID(s)     slot_key((s), 125830400, 65536, 127141120)
#define KEY_TYPE(s)   slot_key((s), 125874183, 65536, 127184903)
#define KEY_FLAGS(s)  slot_key((s), 125831168, 65536, 127141888)


int
offact_get_name(int slot, char out[OFFACT_NAME_MAX]) {
  *out = 0;
  return sceRegMgrGetStr(KEY_NAME(slot), out, OFFACT_NAME_MAX);
}


int
offact_set_name(int slot, const char *name) {
  if(!name) return -1;
  return sceRegMgrSetStr(KEY_NAME(slot), name, OFFACT_NAME_MAX);
}


int
offact_get_id(int slot, uint64_t *out) {
  *out = 0;
  return sceRegMgrGetBin(KEY_ID(slot), out, sizeof(uint64_t));
}


int
offact_set_id(int slot, uint64_t id) {
  return sceRegMgrSetBin(KEY_ID(slot), &id, sizeof(uint64_t));
}


int
offact_get_type(int slot, char out[OFFACT_TYPE_MAX]) {
  *out = 0;
  return sceRegMgrGetStr(KEY_TYPE(slot), out, OFFACT_TYPE_MAX);
}


int
offact_set_type(int slot, const char *type) {
  if(!type) return -1;
  return sceRegMgrSetStr(KEY_TYPE(slot), type, OFFACT_TYPE_MAX);
}


int
offact_get_flags(int slot, int *out) {
  *out = 0;
  return sceRegMgrGetInt(KEY_FLAGS(slot), out);
}


int
offact_set_flags(int slot, int flags) {
  return sceRegMgrSetInt(KEY_FLAGS(slot), flags);
}


uint64_t
offact_gen_id(const char *name) {
  uint64_t h = 0x5EAF00D / 0xCA7F00D;  /* upstream constant */
  if(name && *name) {
    while(*name) {
      h = 0x100000001B3ULL * (h ^ (uint8_t)*name);
      name++;
    }
  }
  return h;
}


int
offact_activate(int slot, uint64_t id) {
  char name[OFFACT_NAME_MAX];
  if(offact_get_name(slot, name) || !name[0]) return -1;
  if(!id) id = offact_gen_id(name);
  if(offact_set_id(slot, id))      return -1;
  if(offact_set_type(slot, "np"))  return -1;
  if(offact_set_flags(slot, OFFACT_DEFAULT_FLAGS)) return -1;
  return 0;
}


int
offact_clear(int slot) {
  if(offact_set_id(slot, 0))   return -1;
  if(offact_set_flags(slot, 0)) return -1;
  return 0;
}
