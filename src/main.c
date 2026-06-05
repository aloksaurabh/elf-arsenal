
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include "activity.h"
#include "cheats.h"
#include "config.h"
#include "backup.h"
#include "linux_loader.h"
#include "sonic_migrate.h"
#include "build_clean.h"
#include "releases.h"
#include "fan.h"
#include "jb.h"
#include "dumper.h"
#include "kmonitor.h"
#include "mdns.h"
#include "homebrew.h"
#include "notif_inbox.h"
#include "offline_pack.h"
#include "smp_meta.h"
#include "smp_updater.h"
#include "sys.h"
#include "websrv.h"
#include "y2jb_updater.h"
#include "payload_registry.h"
#include "plugin_loader.h"
#include "ps5/notify.h"


int
main(int argc, char** argv) {
  const uint16_t port = 6969;

  puts(".----------------------------------------------------------.");
  puts("|   _____ _  __    _                            _          |");
  puts("|  | ____| |/ _|  / \\   _ __ ___  ___ _ __   __ _| |        |");
  puts("|  |  _| | | |_  / _ \\ | '__/ __|/ _ \\ '_ \\ / _` | |        |");
  puts("|  | |___| |  _|/ ___ \\| |  \\__ \\  __/ | | | (_| | |        |");
  puts("|  |_____|_|_|/_/   \\_\\_|  |___/\\___|_| |_|\\__,_|_|        |");
  puts("|                                                          |");
  printf("|  %-22s    all-in-one PS5 payload     |\n", VERSION_TAG);
  puts("'----------------------------------------------------------'");
  puts("");
  puts("  bundled: kstuff-lite + klogsrv + ftpsrv + zftpd + ShadowMountPlus +");
  puts("           nanoDNS + BackPork + garlic-worker + garlic-savemgr + LapyJB +");
  puts("           np-fake-signin + np-restore-account +");
  puts("           trophy-unlocker-all + trophy-unlocker-uds + ps5-app-dumper +");
  puts("           ps5-linux-loader + dpi + websrv + kmon");

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

#ifdef __SCE__
  jb_escalate_pid(getpid());

  backup_worker_init();

  data_dir_migrate();
  build_clean_run_once();

  backup_init();

  config_save_set_inhibit(1);

  /* Create /data/elf-arsenal/cheats early so the bundled ftpsrv can drop
     uploaded cheat .json files straight into it. */
  cheats_init();
  notif_inbox_init();
  activity_init();


  sys_spawn_embedded_payloads();
  /* klogsrv was started inside sys_spawn_embedded_payloads(); give it a
     beat to bind its TCP socket before kmonitor tries to connect. */
  sleep(2);
  kmonitor_start();

  sys_wait_for_nanodns(10);

  sonic_migrate_run_once();

  releases_init();

  fan_init();

  sys_garlic_seed_config();
  sys_garlic_worker_set_enabled(1);
  /* SaveMgr is now mandatory — the launcher exposes it via the top-bar
     tab and there is no longer a way to turn it off from the UI. */
  sys_garlic_savemgr_set_enabled(1);

  sys_lapyjb_set_enabled(0);

  sys_cheatrunner_set_enabled(1);

  config_load();

  cheats_patches_start_watcher();

  linux_loader_init();

  smp_meta_init();
  sys_start_wake_watchdog();

  dumper_seed_configs();


  y2jb_startup_init();

  payload_registry_boot_update();

  plugin_loader_init();

  homebrew_auto_install_tile_init();
#endif


  while(1) {
#ifdef __SCE__
    mdns_discovery_start();
#endif
    websrv_listen(port);
    sleep(3);
  }

  return 0;
}
