
#pragma once

#include <stdint.h>

void smp_meta_init(void);

/* Stat snapshot exposed via /api/smp/meta. */
typedef struct {
  int      running;
  int      poll_seconds;
  uint64_t last_run_unix;
  int      games_scanned;
  int      icons_healed;
  int      pics_healed;
  int      json_healed;
  int      still_missing;
  char     last_missing[64];   /* TITLE_ID of most recent unfixable game */
} smp_meta_stats_t;

void smp_meta_get_stats(smp_meta_stats_t *out);

/* Web UI handles. */
int  smp_meta_run_now(void);                /* trigger an immediate sweep */
int  smp_meta_set_poll_seconds(int seconds);
int  smp_meta_get_poll_seconds(void);
