
#pragma once

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static const char * const sonic_titleid_prefixes[] = {
  "CUSA", "PPSA",                                    /* PS4, PS5 native */
  "ULUS", "ULES", "ULJS", "ULKS",                    /* PSP */
  "SLUS", "SCUS", "SLES", "SCES",                    /* PS1/PS2 */
  "SLPS", "SLPM", "SCED", "SLED", "SCPS",            /* PS1/PS2 (JP/EU/extra) */
  NULL
};


static inline int
title_id_normalize(const char *s, char norm[10]) {
  if(!s) return 0;
  while(*s == ' ' || *s == '\t') s++;
  if(strlen(s) < 9) return 0;

  char up[5];
  for(int i = 0; i < 4; i++) {
    if(!isalpha((unsigned char)s[i])) return 0;
    up[i] = (char)toupper((unsigned char)s[i]);
  }
  up[4] = 0;

  int matched = 0;
  for(const char * const *p = sonic_titleid_prefixes; *p; p++) {
    if(memcmp(up, *p, 4) == 0) { matched = 1; break; }
  }
  if(!matched) return 0;

  const char *digits = s + 4;
  if(*digits == '-' || *digits == '_') digits++;

  for(int i = 0; i < 5; i++) {
    if(!isdigit((unsigned char)digits[i])) return 0;
  }

  if(norm) {
    memcpy(norm, up, 4);
    memcpy(norm + 4, digits, 5);
    norm[9] = 0;
  }
  return 1;
}
