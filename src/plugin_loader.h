#pragma once

/* Arsenal Plugin Loader — boots ELF plugins from /data/elf-arsenal/plugins/.
   Also creates filez/ and krnlz/ subdirs expected by etaHEN-compatible plugins. */

void plugin_loader_init(void);   /* create dirs, spawn enabled plugins at boot */

/* Returns a malloc'd JSON string: [{name,path,enabled,running,pid}, ...].
   Caller frees. */
char *plugin_loader_list_json(void);

int plugin_loader_spawn(const char *name);  /* 0=ok, -1=err */
int plugin_loader_kill(const char *name);   /* 0=ok, -1=err */

int  plugin_loader_set_enabled(const char *name, int on); /* persist to config */
int  plugin_loader_get_enabled(const char *name);

/* Called by config_save() to emit plugin_<name>=0|1 lines. */
void plugin_loader_config_serialize(FILE *f);
/* Called by config_load() for a plugin_<name>=... key. */
void plugin_loader_config_apply(const char *name, int enabled);
