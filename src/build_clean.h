#pragma once

/* First-run cleanup: on the first boot of a new build, deletes everything
   under /data/elf-arsenal except config.ini, the cheats/ tree, and the
   stamp file itself. Safe to call before cheats_init(). */
void build_clean_run_once(void);
