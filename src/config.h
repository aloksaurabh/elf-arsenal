
#pragma once

/* Load config file (creates the directory if missing) and apply each
   value to the relevant subsystem. Safe to call multiple times. */
void config_load(void);

void config_save(void);

void config_save_set_inhibit(int on);
