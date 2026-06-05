/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#pragma once

#include <stdint.h>
#include <stddef.h>


int sys_launch_title(const char* title_id, const char* args);
int sys_launch_homebrew(const char* cwd, const char* path, const char* args,
			const char* env);
int sys_launch_daemon(const char* cwd, const char* uri, const char* args,
		      const char* env);
int sys_launch_payload(const char* cwd, uint8_t* elf, size_t elf_size,
                       const char* argv, const char* env);

void sys_spawn_embedded_payloads(void);

int sys_backpork_set_enabled(int on);
int sys_backpork_is_running(void);

int sys_nanodns_set_enabled(int on);
int sys_nanodns_is_running(void);

int sys_wait_for_nanodns(int timeout_sec);

int sys_lapyjb_set_enabled(int on);
int sys_lapyjb_get_enabled(void);
int sys_lapyjb_is_running(void);

int sys_cheatrunner_set_enabled(int on);
int sys_cheatrunner_get_enabled(void);
int sys_cheatrunner_is_running(void);

int sys_fpkgguard_get_enabled(void);
int sys_fpkgguard_set_enabled(int on);
int sys_fpkgguard_is_running(void);

int sys_spawn_app_dumper(void);

uint32_t sys_get_foreground_user(char *name_out, size_t name_out_size);

int sys_spawn_np_fake_signin(void);
int sys_spawn_np_restore_account(void);

/* Spawn backup-helper.elf as a fresh process. Required workaround for */
int sys_spawn_backup_helper(int argc, const char **argv);

int sys_spawn_sdk_changer(int argc, const char **argv);

char *sys_proc_list_json(void);

int sys_garlic_worker_set_enabled(int on);
int sys_garlic_worker_is_running(void);

int sys_garlic_savemgr_set_enabled(int on);
int sys_garlic_savemgr_is_running(void);

void sys_garlic_seed_config(void);

int sys_garlic_set_poll_interval(int seconds);
int sys_garlic_get_poll_interval(void);

/* Look up a PID by its kernel-thread name (the same matching
   sys_backpork_is_running uses). Returns -1 if not found. */
int sys_find_pid_by_name(const char *name);

int sys_port_is_open(int port);

int sys_dpi_ensure_running(void);

int sys_dpiv2_set_enabled(int on);
int sys_dpiv2_get_enabled(void);
int sys_dpiv2_is_running(void);

int  sys_ftpsrv_set_enabled(int on);
int  sys_ftpsrv_is_running(void);
int  sys_ftpsrv_restart(void);
int  sys_ftpsrv_get_port(void);
void sys_ftpsrv_set_port(int port);

const char* sys_ftpsrv_get_user(void);
const char* sys_ftpsrv_get_pass(void);
const char* sys_ftpsrv_get_type(void);
void sys_ftpsrv_set_user(const char *user);
void sys_ftpsrv_set_pass(const char *pass);
void sys_ftpsrv_set_type(const char *type);

const char* sys_ftpsrv_get_daemon(void);
void sys_ftpsrv_set_daemon(const char *daemon);

int sys_klogsrv_set_enabled(int on);
int sys_klogsrv_get_enabled(void);
int sys_klogsrv_is_running(void);

int sys_trophy_all_set_enabled(int on);
int sys_trophy_all_get_enabled(void);
int sys_trophy_all_is_running(void);
int sys_trophy_uds_set_enabled(int on);
int sys_trophy_uds_get_enabled(void);
int sys_trophy_uds_is_running(void);
int sys_trophy_unlock_now(void);

int sys_kill_title(const char *title_id, int grace_ms);
int sys_get_running_title_id(char *out, size_t out_size);
int sys_console_reboot(void);
int sys_console_poweroff(void);
int sys_console_sleep(void);

unsigned int sys_get_firmware_version(void);

/* Inflate an embedded gzip payload (the gen/payloads gz blobs) into a
   malloc'd buffer the caller must free. Returns NULL on failure. */
uint8_t *sys_gz_inflate(const uint8_t *gz, size_t gz_size, size_t *out_size);

/* Spawn the ps5-fw-spoof payload to set the reported FW version to
   `target` (32-bit packed, e.g. 0x09600000 for "09.60"). One-shot;
   patches live kernel data, needs kstuff. Returns 0 on spawn ok. */
int sys_fw_spoof_run(uint32_t target);

/* Restart SceShellUI so the home screen re-reads app.db (surfaces titles
   registered after ShellUI loaded). Returns 0 if a process was signalled. */
int sys_refresh_shellui(void);
