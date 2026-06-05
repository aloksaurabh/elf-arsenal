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

#include <arpa/inet.h>
#include <execinfo.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>

#include <zlib.h>

#include <ps5/kernel.h>

#include "elfldr.h"
#include "fs.h"
#include "hbldr.h"
#include "http.h"
#include "notify.h"
#include "pt.h"
#include "backup.h"
#include "sys.h"
#include "websrv.h"
#include "kstuff_updater.h"
#include "smp_updater.h"
#include "jb.h"


#define INCASSET(name, file)			\
  __asm__(".section .rodata\n"			\
	  ".global " #name "\n"			\
	  ".global " #name "_end\n"		\
	  ".global " #name "_size\n"		\
	  ".align 16\n"				\
	  #name ":\n"				\
	  ".incbin \"" file "\"\n"		\
	  #name "_end:\n"			\
	  #name "_size:\n"			\
	  ".quad " #name "_end - " #name "\n"	\
	  ".previous\n");			\
  extern const uint8_t name[];			\
  extern const size_t name##_size;


/* Payloads are embedded gzip-compressed (the gen/payloads gz blobs, produced
   by the Makefile) and inflated on use via gz_inflate(). Symbols below hold the
   COMPRESSED bytes; <name>_size is the COMPRESSED size. spawn_embedded() and
   the disk-write consumers inflate before use. */
INCASSET(ftpsrv_elf,            "gen/payloads/ftpsrv.elf.gz");
INCASSET(zftpd_elf,             "gen/payloads/zftpd.elf.gz");
INCASSET(klogsrv_elf,           "gen/payloads/klogsrv.elf.gz");
INCASSET(backpork_elf,          "gen/payloads/backpork.elf.gz");
/* np-fake-signin is no longer embedded — see sys_spawn_np_fake_signin
   below for the disk-cached lazy-download path. */
INCASSET(np_restore_account_elf,"gen/payloads/np-restore-account.elf.gz");
INCASSET(garlic_worker_elf,     "gen/payloads/garlic-worker.elf.gz");
INCASSET(garlic_savemgr_elf,    "gen/payloads/garlic-savemgr.elf.gz");
INCASSET(nanodns_elf,           "gen/payloads/nanodns.elf.gz");
INCASSET(ps5_app_dumper_elf,    "gen/payloads/ps5-app-dumper.elf.gz");
INCASSET(dpi_elf,               "gen/payloads/dpi.elf.gz");
INCASSET(smp_icon_png,          "gen/payloads/smp_icon.png.gz");
INCASSET(lapyjb_elf,            "gen/payloads/lapyjb.elf.gz");
INCASSET(trophy_unlocker_all_elf,"gen/payloads/trophy-unlocker-all.elf.gz");
INCASSET(trophy_unlocker_uds_elf,"gen/payloads/trophy-unlocker-uds.elf.gz");
INCASSET(trophy_unlock_now_elf,  "gen/payloads/trophy-unlock-now.elf.gz");
INCASSET(backup_helper_elf,     "gen/payloads/backup-helper.elf.gz");
INCASSET(fw_probe_elf,          "gen/payloads/fw_probe.elf.gz");
INCASSET(sdk_changer_elf,       "gen/payloads/sdk-changer.elf.gz");
INCASSET(ps5_linux_loader_elf,  "gen/payloads/ps5-linux-loader.elf.gz");
INCASSET(fw_spoof_elf,          "gen/payloads/ps5-fw-spoof.elf.gz");
INCASSET(cheatrunner_elf,       "gen/payloads/cheatrunner.elf.gz");
INCASSET(fpkg_guard_elf,         "gen/payloads/fpkg-guard.elf.gz");
INCASSET(dpiv2_elf,              "gen/payloads/dpiv2.elf.gz");

#define GARLIC_WORKER_PROC_NAME "garlic-worker.elf"
#define NANODNS_PROC_NAME       "nanodns.elf"
#define LAPYJB_PROC_NAME        "lapyjb.elf"
#define FPKG_GUARD_PROC_NAME    "fpkg-guard"
#define CHEATRUNNER_PROC_NAME   "CheatRunner"
#define DPIV2_PROC_NAME         "dpiv2.elf"
static atomic_int g_dpiv2_enabled = 0;
/* klogsrv & zftpd inherit their thread name from argv[0] passed to
   spawn_embedded(). zftpd handled separately via ftpsrv daemon switch. */
#define KLOGSRV_PROC_NAME       "klogsrv"
#define TROPHY_ALL_PROC_NAME    "trophy-all"
#define TROPHY_UDS_PROC_NAME    "trophy-uds"
#define TROPHY_NOW_PROC_NAME    "trophy-now"

#define BACKPORK_PROC_NAME "backpork.elf"

#define SHADOWMOUNT_PROC_NAME "shadowmountplus.elf"

#define FTPSRV_PROC_NAME "ftpsrv.elf"
#define ZFTPD_PROC_NAME  "zftpd"
#define FTPSRV_DEFAULT_PORT 2121

#define KSTUFF_INSTALL_PATH "/data/kstuff.elf"

#define SHADOWMOUNT_DIR          "/data/shadowmount"
#define SHADOWMOUNT_INSTALL_PATH "/data/shadowmount/shadowmountplus.elf"

#define SHADOWMOUNT_ICON_PATH    "/data/shadowmount/smp_icon.png"

#define SONIC_FIRST_BOOT_MARKER "/data/elf-arsenal/.first_boot_done"


typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;


int  sceUserServiceInitialize(void*);
int  sceUserServiceGetForegroundUser(uint32_t *user_id);
int  sceUserServiceGetUserName(int32_t user_id, char *name, size_t max_size);
void sceUserServiceTerminate(void);

int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char* title_id, char** argv,
			      app_launch_ctx_t* ctx);

int sceKernelGetProsperoSystemSwVersion(void *buf);


static char*
args_decode(const char* s) {
  size_t length = strlen(s);
  char *arg = malloc(length+1);
  size_t off = 0;
  int escape = 0;

  for(size_t i=0; i<length; i++) {
    if(s[i] == '\\' && !escape) {
      escape = 1;
    } else {
      arg[off++] = s[i];
      escape = 0;
    }
  }

  arg[off] = 0;
  return arg;
}


static int
args_split(const char* args, char** argv, size_t size) {
  char* buf = strdup(args);
  size_t len = strlen(buf);
  int escape = 0;
  int argc = 0;

  memset(argv, 0, size*sizeof(char*));
  for(int i=0; i<len && argc<size; i++) {
    if(escape) {
      escape = 0;
      continue;
    }

    if(buf[i] == '\\') {
      escape = 1;
      continue;
    }

    if(buf[i] == ' ') {
      buf[i] = 0;
      continue;
    }

    if(buf[i] && !i) {
      argv[argc++] = buf+i;
      continue;
    }

    if(buf[i] && !buf[i-1]) {
      argv[argc++] = buf+i;
    }
  }

  for(int i=0; i<argc; i++) {
    argv[i] = args_decode(argv[i]);
  }

  free(buf);

  return argc;
}


static pid_t sys_find_pid(const char *name);

/* Public wrapper around the static helper so other translation units
   (e.g. playgo.c) can find a process by its kernel-thread name. */
int
sys_find_pid_by_name(const char *name) {
  return (int)sys_find_pid(name);
}


int
sys_port_is_open(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if(s < 0) return 0;
  struct sockaddr_in sa = {0};
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons((uint16_t)port);
  sa.sin_addr.s_addr = inet_addr("127.0.0.1");
  /* Short blocking connect — DPI binds to loopback so this resolves in
     under a millisecond when it's up. */
  int rc = connect(s, (struct sockaddr*)&sa, sizeof(sa));
  close(s);
  return rc == 0 ? 1 : 0;
}


static pid_t
sys_find_pid(const char* name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }

  if(!(buf=malloc(buf_size))) {
    perror("malloc");
    return -1;
  }

  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) {
    perror("sysctl");
    free(buf);
    return -1;
  }

  for(uint8_t *ptr=buf; ptr<(buf+buf_size);) {
    int ki_structsize = *(int*)ptr;
    pid_t ki_pid = *(pid_t*)&ptr[72];
    char *ki_tdname = (char*)&ptr[447];

    ptr += ki_structsize;
    if(!strcmp(name, ki_tdname) && ki_pid != mypid) {
      pid = ki_pid;
    }
  }

  free(buf);

  return pid;
}


int
sys_launch_homebrew(const char* cwd, const char* path, const char* args,
		    const char* env) {
  char* argv[255];
  char* envp[255];
  int optval = 1;
  int fds[2];
  pid_t pid;

  if(!cwd) {
    cwd = "/";
  }

  if(!args) {
    args = "";
  }

  if(!env) {
    env = "";
  }

  printf("launch homebrew: CWD=%s %s %s %s\n", cwd, env, path, args);

  if(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) {
    perror("socketpair");
    return 1;
  }

  if(setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval)) < 0) {
    perror("setsockopt");
    close(fds[0]);
    close(fds[1]);
    return -11;
  }

  args_split(args, argv, 255);
  args_split(env, envp, 255);
  pid = hbldr_launch(cwd, path, fds[1], argv, envp);

  for(int i=0; argv[i]; i++) {
    free(argv[i]);
  }
  for(int i=0; envp[i]; i++) {
    free(envp[i]);
  }

  close(fds[1]);
  if(pid < 0) {
    close(fds[0]);
    return -1;
  }

  return fds[0];
}


int
sys_launch_daemon(const char* cwd, const char* uri, const char* args,
		  const char* env) {
  uint8_t* elf = 0;
  char* argv[255];
  char* envp[255];
  int fds[2];
  pid_t pid;

  if(!cwd) {
    cwd = "/";
  }
  if(!args) {
    args = "";
  }
  if(!env) {
    env = "";
  }

  if(uri[0] == '/') {
    if(!(elf=fs_readfile(uri, 0))) {
      return -1;
    }

  } else if(!strncmp(uri, "file:", 5)) {
    if(!(elf=fs_readfile(uri+5, 0))) {
      return -1;
    }

  } else if(!strncmp(uri, "http:", 5) ||
	    !strncmp(uri, "https:", 6)) {
    if(!(elf=http_get(uri, 0))) {
      return -1;
    }
  }

  if(!elf) {
    return -1;
  }

  printf("launch daemon: CWD=%s %s %s %s\n", cwd, env, uri, args);

  if(pipe(fds) == -1) {
    perror("pipe");
    return 1;
  }

  args_split(args, argv, 255);
  args_split(env, envp, 255);
  pid = elfldr_spawn(cwd, fds[1], elf, argv, envp);

  free(elf);
  for(int i=0; argv[i]; i++) {
    free(argv[i]);
  }
  for(int i=0; envp[i]; i++) {
    free(envp[i]);
  }

  close(fds[1]);
  if(pid < 0) {
    close(fds[0]);
    return -1;
  }

  return fds[0];
}


int
sys_launch_payload(const char* cwd, uint8_t* elf, size_t elf_size,
                   const char* args, const char* env) {
  char* argv[255];
  char* envp[255];

  int fds[2];
  pid_t pid;

  if(!cwd) {
    cwd = "/";
  }

  if(!args) {
    args = "";
  }

  if(!env) {
    env = "";
  }

  printf("launch payload: CWD=%s %s %s\n", cwd, env, args);

  if(pipe(fds) == -1) {
    perror("pipe");
    return 1;
  }

  args_split(args, argv, 255);
  args_split(env, envp, 255);
  pid = elfldr_spawn(cwd, fds[1], elf, argv, envp);

  for(int i=0; argv[i]; i++) {
    free(argv[i]);
  }
  for(int i=0; envp[i]; i++) {
    free(envp[i]);
  }

  close(fds[1]);
  if(pid < 0) {
    close(fds[0]);
    return -1;
  }

  return fds[0];
}


int
sys_launch_title(const char* title_id, const char* args) {
  app_launch_ctx_t ctx = {0};
  char* argv[255];
  int argc = 0;
  int app_id;
  int err;
  int have_ctx = 0;

  if(!args) {
    args = "";
  }

  printf("launch title: %s %s\n", title_id, args);

  uint32_t fg_uid = 0xFFFFFFFFu;
  if(sceUserServiceGetForegroundUser(&fg_uid) == 0 &&
     fg_uid != 0xFFFFFFFFu && (int32_t)fg_uid != -1) {
    ctx.structsize = sizeof(ctx);
    ctx.user_id    = fg_uid;
    have_ctx = 1;
  }

  if((app_id=sceSystemServiceGetAppIdOfRunningBigApp()) > 0) {
    if((err=sceSystemServiceKillApp(app_id, -1, 0, 0))) {
      perror("sceSystemServiceKillApp");
      return err;
    }
  }

  argc = args_split(args, argv, 255);
  err = sceSystemServiceLaunchApp(title_id, argv, have_ctx ? &ctx : NULL);
  if(err < 0) {
    perror("sceSystemServiceLaunchApp");
  } else {
    err = 0;
  }

  for(int i=0; i<argc; i++) {
    free(argv[i]);
  }

  return err;
}

static int __attribute__((unused))
sys_notify_address(const char* prefix, int port) {
  char ip[INET_ADDRSTRLEN] = "127.0.0.1";
  struct ifaddrs *ifaddr;

  if(getifaddrs(&ifaddr) == -1) {
    perror("getifaddrs");
    return -1;
  }

  // Enumerate all AF_INET IPs
  for(struct ifaddrs *ifa=ifaddr; ifa!=NULL; ifa=ifa->ifa_next) {
    if(ifa->ifa_addr == NULL) {
      continue;
    }

    if(ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    // skip localhost
    if(!strncmp("lo", ifa->ifa_name, 2)) {
      continue;
    }

    struct sockaddr_in *in = (struct sockaddr_in*)ifa->ifa_addr;
    inet_ntop(AF_INET, &(in->sin_addr), ip, sizeof(ip));

    // skip interfaces without an ip
    if(!strncmp("0.", ip, 2)) {
      continue;
    }
  }

  freeifaddrs(ifaddr);

  notify("%s %s:%d", prefix, ip, port);
  printf("%s %s:%d\n", prefix, ip, port);

  return 0;
}


static void
on_fatal_signal(int sig) {
  void *buf[0x1000];
  int nptrs;

  notify("elf-arsenal.elf: %s", strsignal(sig));

  nptrs = backtrace(buf, sizeof(buf));
  backtrace_symbols_fd(buf, nptrs, open("/dev/console", O_WRONLY));

  _exit(EXIT_FAILURE);
}


static int
install_payload_file(const char *path, const uint8_t *data, size_t size) {
  struct stat st;
  int fd;
  ssize_t n;
  int need_write = 1;

  /* Skip rewriting if the file already exists with the same size and the
     bytes match — saves a few hundred ms on repeat boots. */
  if(stat(path, &st) == 0 && (size_t)st.st_size == size) {
    if((fd=open(path, O_RDONLY)) >= 0) {
      uint8_t *buf = malloc(size);
      if(buf) {
        n = read(fd, buf, size);
        if(n == (ssize_t)size && memcmp(buf, data, size) == 0) {
          need_write = 0;
        }
        free(buf);
      }
      close(fd);
    }
  }

  if(!need_write) {
    return 0;
  }

  if((fd=open(path, O_WRONLY|O_CREAT|O_TRUNC, 0755)) < 0) {
    perror("install_payload_file: open");
    return -1;
  }
  if((n=write(fd, data, size)) != (ssize_t)size) {
    perror("install_payload_file: write");
    close(fd);
    unlink(path);
    return -1;
  }
  close(fd);
  chmod(path, 0755);
  return 0;
}


static uint8_t*
read_elf_file(const char *path, size_t *size_out) {
  struct stat st;
  int fd;
  uint8_t *buf;
  ssize_t n;

  if(stat(path, &st) != 0) {
    return NULL;
  }
  if((fd=open(path, O_RDONLY)) < 0) {
    return NULL;
  }
  if(!(buf=malloc(st.st_size))) {
    close(fd);
    return NULL;
  }
  n = read(fd, buf, st.st_size);
  close(fd);
  if(n != st.st_size) {
    free(buf);
    return NULL;
  }
  *size_out = st.st_size;
  return buf;
}


/* Inflate an embedded gzip blob into a freshly malloc'd buffer. Returns the
   buffer (caller frees) and sets *out_size, or NULL on failure. Uncompressed
   size is taken from the gzip ISIZE trailer (last 4 bytes, little-endian) and
   sanity-capped. Used for both spawn (inflate->spawn->free) and disk-write
   (inflate->write->free) consumers; also called from linux_loader.c. */
uint8_t *
sys_gz_inflate(const uint8_t *gz, size_t gz_size, size_t *out_size) {
  if(out_size) *out_size = 0;
  if(!gz || gz_size < 18) return NULL;   /* smaller than a gzip member */

  uint32_t isize = (uint32_t)gz[gz_size - 4]
                 | ((uint32_t)gz[gz_size - 3] << 8)
                 | ((uint32_t)gz[gz_size - 2] << 16)
                 | ((uint32_t)gz[gz_size - 1] << 24);
  if(isize == 0 || isize > (64u * 1024u * 1024u)) {
    fprintf(stderr, "sys_gz_inflate: implausible ISIZE %u\n", isize);
    return NULL;
  }

  uint8_t *out = malloc(isize);
  if(!out) { fprintf(stderr, "sys_gz_inflate: oom (%u)\n", isize); return NULL; }

  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  zs.next_in   = (Bytef*)gz;
  zs.avail_in  = (uInt)gz_size;
  zs.next_out  = out;
  zs.avail_out = isize;
  if(inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {   /* 16 => gzip wrapper */
    fprintf(stderr, "sys_gz_inflate: inflateInit2 failed\n");
    free(out);
    return NULL;
  }
  int rc = inflate(&zs, Z_FINISH);
  size_t produced = zs.total_out;
  inflateEnd(&zs);
  if(rc != Z_STREAM_END || produced != isize) {
    fprintf(stderr, "sys_gz_inflate: inflate rc=%d (%zu/%u)\n",
            rc, produced, isize);
    free(out);
    return NULL;
  }
  if(out_size) *out_size = produced;
  return out;
}


/* Spawn an already-decompressed (raw) ELF image. Used for payloads loaded
   from disk (kstuff, ShadowMountPlus) which are NOT gzip-embedded. Does not
   take ownership of `elf` — caller frees. */
static int
spawn_raw(const char *label, uint8_t *elf, size_t elf_size) {
  char* argv[2] = {(char*)label, 0};
  char* envp[1] = {0};
  int devnull;
  pid_t pid;

  if((devnull=open("/dev/null", O_WRONLY)) < 0) {
    devnull = -1;
  }

  pid = elfldr_spawn("/", devnull, elf, argv, envp);

  if(devnull >= 0) {
    close(devnull);
  }

  if(pid < 0) {
    /* Failures are still loud — they matter. */
    notify("elf-arsenal: failed to spawn %s", label);
    fprintf(stderr, "elf-arsenal: failed to spawn %s\n", label);
    return -1;
  }

  /* Successful spawns are silent on-screen; the consolidated boot toast
     is emitted by sys_spawn_embedded_payloads() at the end. */
  printf("elf-arsenal: spawned %s (size=%zu pid=%d)\n",
         label, elf_size, (int)pid);
  return 0;
}

/* Spawn a gzip-embedded payload: inflate the INCASSET blob, then spawn_raw. */
static int
spawn_embedded(const char *label, const uint8_t* gz, size_t gz_size) {
  size_t elf_size = 0;
  uint8_t *elf = sys_gz_inflate(gz, gz_size, &elf_size);
  if(!elf) {
    notify("elf-arsenal: failed to inflate %s", label);
    fprintf(stderr, "elf-arsenal: failed to inflate %s\n", label);
    return -1;
  }
  int rc = spawn_raw(label, elf, elf_size);
  free(elf);   /* elfldr_spawn copies the ELF into the child synchronously */
  return rc;
}


/* Spawn variant that takes a full argv. For sub-payloads that accept
   command-line flags (ftpsrv accepts -p PORT). */
static int
spawn_embedded_argv(const uint8_t *gz, size_t gz_size, char **argv) {
  size_t elf_size = 0;
  uint8_t *elf = sys_gz_inflate(gz, gz_size, &elf_size);
  if(!elf) {
    notify("elf-arsenal: failed to inflate %s", argv[0] ? argv[0] : "(elf)");
    fprintf(stderr, "elf-arsenal: failed to inflate %s\n",
            argv[0] ? argv[0] : "(elf)");
    return -1;
  }
  char *envp[1] = {0};
  int devnull = open("/dev/null", O_WRONLY);
  pid_t pid = elfldr_spawn("/", devnull, elf, argv, envp);
  if(devnull >= 0) close(devnull);
  free(elf);
  if(pid < 0) {
    notify("elf-arsenal: failed to spawn %s", argv[0] ? argv[0] : "(elf)");
    fprintf(stderr, "elf-arsenal: failed to spawn %s\n",
            argv[0] ? argv[0] : "(elf)");
    return -1;
  }
  printf("elf-arsenal: spawned %s (size=%zu pid=%d)\n",
         argv[0] ? argv[0] : "(elf)", elf_size, (int)pid);
  return 0;
}



int
sys_spawn_backup_helper(int argc, const char **argv_in) {
  if (argc < 1 || argc > 8) return -1;
  char *argv[10] = {0};
  /* spawn_embedded_argv writes argv[0] as the proc name; the rest are
     the actual op + args the helper parses. */
  argv[0] = (char *)"backup-helper";
  for (int i = 0; i < argc; i++) {
    argv[i + 1] = (char *)argv_in[i];
  }
  return spawn_embedded_argv(backup_helper_elf, backup_helper_elf_size, argv);
}


int
sys_spawn_sdk_changer(int argc, const char **argv_in) {
  if (argc < 1 || argc > 8) return -1;
  char *argv[10] = {0};
  argv[0] = (char *)"sdk-changer";
  for (int i = 0; i < argc; i++) {
    argv[i + 1] = (char *)argv_in[i];
  }
  return spawn_embedded_argv(sdk_changer_elf, sdk_changer_elf_size, argv);
}


/* ─────── ftpsrv toggle + port + auth + transfer-type config ─────── */

static atomic_int g_ftpsrv_port = FTPSRV_DEFAULT_PORT;

static char g_ftpsrv_user[64] = "anonymous";
static char g_ftpsrv_pass[64] = "";
static char g_ftpsrv_type[8]  = "auto";   /* "auto" | "binary" | "ascii" */
static char g_ftpsrv_daemon[8] = "ftpsrv"; /* "ftpsrv" | "zftpd" */

static atomic_int g_klogsrv_enabled    = 1;
static atomic_int g_trophy_all_enabled = 0;
static atomic_int g_trophy_uds_enabled = 0;


int
sys_ftpsrv_get_port(void) {
  int p = atomic_load(&g_ftpsrv_port);
  if(p < 1 || p > 65535) p = FTPSRV_DEFAULT_PORT;
  return p;
}

void
sys_ftpsrv_set_port(int port) {
  if(port < 1 || port > 65535) port = FTPSRV_DEFAULT_PORT;
  atomic_store(&g_ftpsrv_port, port);
}

const char* sys_ftpsrv_get_user(void) { return g_ftpsrv_user; }
const char* sys_ftpsrv_get_pass(void) { return g_ftpsrv_pass; }
const char* sys_ftpsrv_get_type(void) { return g_ftpsrv_type; }

void
sys_ftpsrv_set_user(const char *user) {
  if(!user || !*user) {
    strcpy(g_ftpsrv_user, "anonymous");
  } else {
    strncpy(g_ftpsrv_user, user, sizeof(g_ftpsrv_user) - 1);
    g_ftpsrv_user[sizeof(g_ftpsrv_user) - 1] = '\0';
  }
}

void
sys_ftpsrv_set_pass(const char *pass) {
  if(!pass) pass = "";
  strncpy(g_ftpsrv_pass, pass, sizeof(g_ftpsrv_pass) - 1);
  g_ftpsrv_pass[sizeof(g_ftpsrv_pass) - 1] = '\0';
}

void
sys_ftpsrv_set_type(const char *type) {
  if(!type || !*type) type = "auto";
  if(!strcasecmp(type, "binary") || !strcasecmp(type, "i") ||
     !strcasecmp(type, "image"))
    strcpy(g_ftpsrv_type, "binary");
  else if(!strcasecmp(type, "ascii") || !strcasecmp(type, "a"))
    strcpy(g_ftpsrv_type, "ascii");
  else
    strcpy(g_ftpsrv_type, "auto");
}


const char* sys_ftpsrv_get_daemon(void) { return g_ftpsrv_daemon; }

void
sys_ftpsrv_set_daemon(const char *daemon) {
  if(!daemon || !*daemon) daemon = "ftpsrv";
  if(!strcasecmp(daemon, "zftpd"))
    strcpy(g_ftpsrv_daemon, "zftpd");
  else
    strcpy(g_ftpsrv_daemon, "ftpsrv");
}

int
sys_ftpsrv_is_running(void) {
  /* Either daemon's thread name lives in p_comm. Check both — saves
     the caller from having to peek g_ftpsrv_daemon. */
  if(sys_find_pid(FTPSRV_PROC_NAME) > 0) return 1;
  if(sys_find_pid(ZFTPD_PROC_NAME)  > 0) return 1;
  return 0;
}


static int
spawn_ftpsrv(void) {
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%d", sys_ftpsrv_get_port());

  int use_zftpd = !strcasecmp(g_ftpsrv_daemon, "zftpd");

  char *argv[16];
  int n = 0;
  argv[n++] = use_zftpd ? (char*)"zftpd" : (char*)"ftpsrv";
  argv[n++] = (char*)"-p"; argv[n++] = port_str;
  if(!use_zftpd) {
    argv[n++] = (char*)"-t"; argv[n++] = (char*)sys_ftpsrv_get_type();
    if(g_ftpsrv_user[0] && strcasecmp(g_ftpsrv_user, "anonymous") != 0) {
      argv[n++] = (char*)"-u"; argv[n++] = g_ftpsrv_user;
      argv[n++] = (char*)"-P"; argv[n++] = g_ftpsrv_pass;
    }
  } else {
    /* zftpd accepts the port as a positional fallback if -p isn't
       recognised; harmless if both are interpreted. */
    argv[n++] = port_str;
  }
  argv[n] = NULL;

  if(use_zftpd) {
    return spawn_embedded_argv(zftpd_elf,  zftpd_elf_size,  argv);
  }
  return   spawn_embedded_argv(ftpsrv_elf, ftpsrv_elf_size, argv);
}

int
sys_ftpsrv_set_enabled(int on) {
  pid_t existing;
  int rc;

  if(on) {
    if(sys_find_pid(FTPSRV_PROC_NAME) > 0) { rc = 1; goto persist; }
    if(spawn_ftpsrv() != 0) { rc = -1; goto persist; }
    rc = 1;  /* spawned — assume PID will register shortly */
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(FTPSRV_PROC_NAME) > 0) break;
      usleep(100000);
    }
    goto persist;
  }

  const char *names[2] = { FTPSRV_PROC_NAME, ZFTPD_PROC_NAME };
  for(int j = 0; j < 2; j++) {
    while((existing = sys_find_pid(names[j])) > 0) {
      if(kill(existing, SIGTERM) != 0) {
        perror("kill ftp daemon");
        rc = -1;
        goto persist;
      }
      int exited = 0;
      for(int i = 0; i < 30; i++) {
        usleep(100000);
        if(sys_find_pid(names[j]) <= 0) { exited = 1; break; }
      }
      if(!exited) kill(existing, SIGKILL);
    }
  }
  rc = 0;

persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}

int
sys_ftpsrv_restart(void) {
  /* Kill whichever daemon is running, then spawn whichever is
     currently selected by g_ftpsrv_daemon. */
  const char *names[2] = { FTPSRV_PROC_NAME, ZFTPD_PROC_NAME };
  for(int j = 0; j < 2; j++) {
    pid_t pid = sys_find_pid(names[j]);
    if(pid > 0) {
      kill(pid, SIGTERM);
      for(int i = 0; i < 30; i++) {
        usleep(100000);
        if(sys_find_pid(names[j]) <= 0) break;
      }
      if((pid = sys_find_pid(names[j])) > 0) kill(pid, SIGKILL);
    }
  }
  return spawn_ftpsrv();
}



int sys_klogsrv_is_running(void) {
  return sys_find_pid(KLOGSRV_PROC_NAME) > 0 ? 1 : 0;
}

int sys_klogsrv_get_enabled(void) {
  return atomic_load(&g_klogsrv_enabled);
}

int
sys_klogsrv_set_enabled(int on) {
  /* klogsrv is locked on — the klog viewer + remote :3232 tail are
     considered core infrastructure. Ignore `off` requests and ensure
     the daemon is running. */
  (void)on;
  atomic_store(&g_klogsrv_enabled, 1);
  if(sys_find_pid(KLOGSRV_PROC_NAME) <= 0) {
    spawn_embedded(KLOGSRV_PROC_NAME, klogsrv_elf, klogsrv_elf_size);
  }
  { extern void config_save(void); config_save(); }
  return 1;
}



int sys_trophy_all_is_running(void) {
  return sys_find_pid(TROPHY_ALL_PROC_NAME) > 0 ? 1 : 0;
}

int sys_trophy_all_get_enabled(void) {
  return atomic_load(&g_trophy_all_enabled);
}

int
sys_trophy_all_set_enabled(int on) {
  atomic_store(&g_trophy_all_enabled, on ? 1 : 0);
  if(on) {
    if(sys_find_pid(TROPHY_ALL_PROC_NAME) <= 0) {
      spawn_embedded(TROPHY_ALL_PROC_NAME,
                     trophy_unlocker_all_elf,
                     trophy_unlocker_all_elf_size);
    }
  } else {
    pid_t pid;
    while((pid = sys_find_pid(TROPHY_ALL_PROC_NAME)) > 0) {
      kill(pid, SIGKILL);
      usleep(100000);
    }
  }
  { extern void config_save(void); config_save(); }
  return atomic_load(&g_trophy_all_enabled);
}



int sys_trophy_uds_is_running(void) {
  return sys_find_pid(TROPHY_UDS_PROC_NAME) > 0 ? 1 : 0;
}

int sys_trophy_uds_get_enabled(void) {
  return atomic_load(&g_trophy_uds_enabled);
}

int
sys_trophy_uds_set_enabled(int on) {
  atomic_store(&g_trophy_uds_enabled, on ? 1 : 0);
  if(on) {
    if(sys_find_pid(TROPHY_UDS_PROC_NAME) <= 0) {
      spawn_embedded(TROPHY_UDS_PROC_NAME,
                     trophy_unlocker_uds_elf,
                     trophy_unlocker_uds_elf_size);
    }
  } else {
    pid_t pid;
    while((pid = sys_find_pid(TROPHY_UDS_PROC_NAME)) > 0) {
      kill(pid, SIGKILL);
      usleep(100000);
    }
  }
  { extern void config_save(void); config_save(); }
  return atomic_load(&g_trophy_uds_enabled);
}


int
sys_trophy_unlock_now(void) {
  /* One-shot: spawn the direct libSceNpTrophy unlocker for the currently
     running (or suspended) game. Auto-detects via sceNpTrophyIntGetRunningTitle.
     Works on any FW because kstuff-lite already patches ShellCore's trophy
     validation, so RegisterContext succeeds without a separate core patch. */
  return spawn_embedded(TROPHY_NOW_PROC_NAME,
                        trophy_unlock_now_elf, trophy_unlock_now_elf_size);
}


int
sys_kill_title(const char *title_id, int grace_ms) {
  (void)title_id;
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  printf("sys_kill_title(%s): running bigapp=%d\n",
         title_id ? title_id : "?", app_id);
  if(app_id <= 0) return -1;

  int err = sceSystemServiceKillApp(app_id, -1, 0, 0);
  if(err) {
    printf("sys_kill_title: sceSystemServiceKillApp(%d) -> 0x%x — escalating\n",
           app_id, (unsigned)err);
  }

  int poll_n = (grace_ms < 100 ? 1 : grace_ms / 100);
  for(int i = 0; i < poll_n; i++) {
    if(sceSystemServiceGetAppIdOfRunningBigApp() <= 0) {
      printf("sys_kill_title: clean kill OK after %d ms\n", i * 100);
      return 0;
    }
    usleep(100000);
  }

  printf("sys_kill_title: clean kill timed out — SIGKILL pass\n");
  static const char *KILL_NAMES[] = { "eboot.bin", "SceSysCore.elf", NULL };
  int killed = 0;
  for(int i = 0; KILL_NAMES[i]; i++) {
    pid_t pid;
    while((pid = sys_find_pid(KILL_NAMES[i])) > 0) {
      if(kill(pid, SIGKILL) == 0) { killed++; }
      else { perror("kill SIGKILL"); break; }
      usleep(50000);
    }
  }
  if(killed > 0) {
    printf("sys_kill_title: SIGKILL'd %d game procs\n", killed);
    return 0;
  }
  usleep(1000000);
  if(sceSystemServiceGetAppIdOfRunningBigApp() <= 0) return 0;
  return -1;
}


/* reboot(2) flags — FreeBSD <sys/reboot.h> */
#ifndef RB_AUTOBOOT
#define RB_AUTOBOOT 0
#endif
#ifndef RB_POWEROFF
#define RB_POWEROFF 0x4000
#endif
#ifndef RB_HALT
#define RB_HALT 0x8
#endif

/* __syscall bypasses the SCE IPC layer entirely; reboot/poweroff via
   sceSystemServiceRequest* are silently ignored from non-app processes. */
extern long __syscall(long number, ...);

int sceSystemStateMgrEnterStandby(void);

int
sys_console_reboot(void) {
  return (int)__syscall(SYS_reboot, RB_AUTOBOOT);
}

int
sys_console_poweroff(void) {
  return (int)__syscall(SYS_reboot, RB_POWEROFF);
}

int
sys_console_sleep(void) {
  return sceSystemStateMgrEnterStandby();
}


typedef struct sys_app_info {
  uint32_t app_id;
  uint64_t unknown1;
  char     title_id[14];
  char     unknown2[0x3c];
} sys_app_info_t;
int sceKernelGetAppInfo(pid_t pid, sys_app_info_t *info);

int
sys_get_running_title_id(char *out, size_t out_size) {
  if(out && out_size) out[0] = 0;
  if(!out || out_size < 10) return -1;

  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if(app_id <= 0) return -1;

  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t bufsz = 0;
  if(sysctl(mib, 4, NULL, &bufsz, NULL, 0)) return -1;
  uint8_t *buf = malloc(bufsz);
  if(!buf) return -1;
  if(sysctl(mib, 4, buf, &bufsz, NULL, 0)) { free(buf); return -1; }

  int rc = -1;
  sys_app_info_t info;
  for(uint8_t *p = buf; p < buf + bufsz; ) {
    int ks = *(int *)p;
    pid_t pid = *(pid_t *)&p[72];
    p += ks;
    memset(&info, 0, sizeof info);
    if(sceKernelGetAppInfo(pid, &info) == 0 &&
       info.app_id == (uint32_t)app_id &&
       info.title_id[0]) {
      size_t n = strnlen(info.title_id, sizeof info.title_id);
      if(n >= out_size) n = out_size - 1;
      memcpy(out, info.title_id, n);
      out[n] = 0;
      rc = 0;
      break;
    }
  }
  free(buf);
  return rc;
}


unsigned int
sys_get_firmware_version(void) {
  uint32_t fw = kernel_get_fw_version();
  if(fw != 0) return (unsigned int)fw;

  uint8_t buf[0x18] = {0};
  *((uint32_t*)buf) = sizeof(buf);
  if(sceKernelGetProsperoSystemSwVersion(buf) != 0) return 0;
  return *(uint32_t*)&buf[0x14];
}


/* Restart SceShellUI so the home screen re-reads app.db. Needed when titles
   are registered after ShellUI has already loaded (e.g. SMP-mounted fpkg
   titles after a Sony DB rebuild) — the rows are correct in app.db but the
   home screen won't display them until ShellUI reloads. ShellCore respawns
   SceShellUI automatically. Returns 0 if a process was signalled. */
int
sys_refresh_shellui(void) {
  /* Try the known shell-UI thread names; first match wins. */
  static const char *names[] = { "SceShellUI", "ShellUI", "SceShellCore", 0 };
  for(int i = 0; names[i]; i++) {
    pid_t pid = sys_find_pid(names[i]);
    if(pid > 0) {
      if(kill(pid, SIGKILL) == 0) {
        printf("refresh_shellui: killed %s pid=%d (ShellCore will respawn)\n",
               names[i], (int)pid);
        return 0;
      }
      fprintf(stderr, "refresh_shellui: kill %s failed\n", names[i]);
    }
  }
  fprintf(stderr, "refresh_shellui: no shell-UI process found\n");
  return -1;
}


/* Spawn the fw-spoof payload to patch the reported FW version to `target`
   (32-bit packed, e.g. 0x09600000). The payload reads the value as argv[1]
   (hex), scans live kernel data and overwrites — effective immediately. */
int
sys_fw_spoof_run(uint32_t target) {
  char ver[16];
  snprintf(ver, sizeof(ver), "%08x", (unsigned)target);
  char *argv[3] = { "fw-spoof", ver, 0 };
  return spawn_embedded_argv(fw_spoof_elf, fw_spoof_elf_size, argv);
}


int
sys_dpi_ensure_running(void) {
  static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&mtx);
  if(sys_port_is_open(9040)) { pthread_mutex_unlock(&mtx); return 1; }
  if(spawn_embedded("dpi", dpi_elf, dpi_elf_size) != 0) { pthread_mutex_unlock(&mtx); return -1; }
  /* Escalate so sceAppInstUtilInitialize succeeds on FW 11.00+. */
  for(int i = 0; i < 30; i++) {
    pid_t p = sys_find_pid("dpi");
    if(p > 0) { jb_escalate_pid(p); break; }
    usleep(100000);
  }
  /* DPI v1 waits 25 s at startup before binding; poll up to 30 s. */
  for(int i = 0; i < 300; i++) {
    usleep(100000);
    if(sys_port_is_open(9040)) { pthread_mutex_unlock(&mtx); return 1; }
  }
  pthread_mutex_unlock(&mtx);
  return 0;
}


#define SHADOWMOUNT_ICON_MARKER  SHADOWMOUNT_DIR "/.smp_icon_v2"
static void
seed_shadowmount_icon(void) {
  struct stat st;
  mkdir(SHADOWMOUNT_DIR, 0755);

  int file_present   = (stat(SHADOWMOUNT_ICON_PATH,   &st) == 0 && st.st_size > 0);
  int marker_present = (stat(SHADOWMOUNT_ICON_MARKER, &st) == 0);

  /* Up-to-date already — nothing to do. */
  if(file_present && marker_present) return;

  size_t icon_size = 0;
  uint8_t *icon = sys_gz_inflate(smp_icon_png, smp_icon_png_size, &icon_size);
  if(icon) {
    install_payload_file(SHADOWMOUNT_ICON_PATH, icon, icon_size);
    free(icon);
  }

  /* Mark this version as installed so subsequent boots skip the
     overwrite path. */
  int fd = open(SHADOWMOUNT_ICON_MARKER,
                O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0) {
    const char *msg = "elf-arsenal-v1\n";
    write(fd, msg, strlen(msg));
    close(fd);
  }
}


static int
first_boot_marker_present(void) {
  struct stat st;
  return stat(SONIC_FIRST_BOOT_MARKER, &st) == 0;
}


static void
write_first_boot_marker(void) {
  mkdir("/data/elf-arsenal", 0755);
  int fd = open(SONIC_FIRST_BOOT_MARKER, O_WRONLY|O_CREAT|O_TRUNC, 0644);
  if(fd >= 0) {
    const char *msg = "sonic-loader first boot complete\n";
    (void)write(fd, msg, strlen(msg));
    close(fd);
  }
}


void
sys_spawn_embedded_payloads(void) {
  uint8_t *smp_buf    = NULL;
  size_t   smp_buf_size = 0;
  int      first_boot = !first_boot_marker_present();
  struct stat st;

  {
    extern void homebrew_wipe_staged_pkgs(void);
    homebrew_wipe_staged_pkgs();
  }

  spawn_embedded("fw_probe", fw_probe_elf, fw_probe_elf_size);

  seed_shadowmount_icon();

  /* Auto-install kstuff + SMP if kstuff.elf is absent. Runs early so the
     file is on disk before the spawn at the end of this function. */
  if(stat(KSTUFF_INSTALL_PATH, &st) != 0 || st.st_size <= 4) {
    unsigned int fw = sys_get_firmware_version();
    int use_drakmor = (fw != 0 && fw <= 0x10010000u);
    const char *smp_v = use_drakmor ? "104" : "103";
    const char *combo_label = use_drakmor
        ? "drakmor (firmware ≤ 10.01)"
        : "EchoStretch (firmware > 10.01)";

    if(fw == 0) {
      notify("Elf Arsenal: couldn't read firmware version — falling "
             "back to EchoStretch combo.");
    } else {
      notify("Elf Arsenal first boot: detected firmware %02x.%02x — "
             "auto-installing %s combo from GitHub…",
             (unsigned)((fw >> 24) & 0xff),
             (unsigned)((fw >> 16) & 0xff),
             combo_label);
    }

    int kr = kstuff_install_direct(use_drakmor);
    int sr = smp_install_direct(smp_v);

    if(kr == 0 && sr == 0) {
      notify("Elf Arsenal: auto-install OK (kstuff %s + SMP %s). "
             "Loading kstuff now.",
             use_drakmor ? "drakmor" : "EchoStretch", smp_v);
    } else {
      notify("Elf Arsenal: auto-install failed (kstuff=%d, SMP=%d). "
             "Open Settings → Install kstuff-lite to retry manually.",
             kr, sr);
    }
  }

  if(atomic_load(&g_klogsrv_enabled)) {
    spawn_embedded(KLOGSRV_PROC_NAME, klogsrv_elf, klogsrv_elf_size);
  }
  /* ftpsrv: spawn with the configured port (default 2121). The port
     is loaded from /data/elf-arsenal/config.ini before this runs. */
  spawn_ftpsrv();

  if(atomic_load(&g_trophy_all_enabled)) {
    backup_snapshot_file("trophy-all", "/system_data/priv/mms/app.db");
    backup_snapshot_file("trophy-all", "/system_data/priv/mms/appinfo.db");
    spawn_embedded(TROPHY_ALL_PROC_NAME,
                   trophy_unlocker_all_elf, trophy_unlocker_all_elf_size);
  }

  if(atomic_load(&g_trophy_uds_enabled)) {
    spawn_embedded(TROPHY_UDS_PROC_NAME,
                   trophy_unlocker_uds_elf, trophy_unlocker_uds_elf_size);
  }

  if(stat(SHADOWMOUNT_INSTALL_PATH, &st) == 0 && st.st_size > 64) {
    smp_buf = read_elf_file(SHADOWMOUNT_INSTALL_PATH, &smp_buf_size);
  }
  if(smp_buf) {
    spawn_raw("shadowmountplus", smp_buf, smp_buf_size);   /* raw disk ELF */
    free(smp_buf);
  } else {
    notify("Elf Arsenal: ShadowMountPlus not installed yet. Open "
           "Settings → ShadowMountPlus and pick a release to install.");
  }

  spawn_embedded("nanodns",          nanodns_elf,      nanodns_elf_size);

  /* Check config.ini directly — config_load() hasn't run yet at this point.
     If dpiv2=1 is set, skip DPI v1 and go straight to v2. */
  {
    int want_v2 = 0;
    FILE *cf = fopen("/data/elf-arsenal/config.ini", "r");
    if(cf) {
      char line[128];
      while(fgets(line, sizeof(line), cf)) {
        if(strncmp(line, "dpiv2=1", 7) == 0) { want_v2 = 1; break; }
      }
      fclose(cf);
    }
    if(want_v2) {
      atomic_store(&g_dpiv2_enabled, 1);
      spawn_embedded(DPIV2_PROC_NAME, dpiv2_elf, dpiv2_elf_size);
      /* Escalate authid so dpiv2 can call sceAppInstUtilInitialize on
         FW 11.00+ without needing kernel_set_ucred_authid itself. */
      for(int i = 0; i < 30; i++) {
        pid_t p = sys_find_pid(DPIV2_PROC_NAME);
        if(p > 0) { jb_escalate_pid(p); break; }
        usleep(100000);
      }
    } else {
      if(!sys_port_is_open(9040)) {
        spawn_embedded("dpi", dpi_elf, dpi_elf_size);
        /* Escalate so sceAppInstUtilInitialize succeeds on FW 11.00+.
           DPI v1 waits 25 s before init; the PID is available immediately. */
        for(int i = 0; i < 30; i++) {
          pid_t p = sys_find_pid("dpi");
          if(p > 0) { jb_escalate_pid(p); break; }
          usleep(100000);
        }
      }
    }
  }

  if(sys_fpkgguard_get_enabled())
    spawn_embedded("fpkg-guard", fpkg_guard_elf, fpkg_guard_elf_size);

  if(first_boot) {
    mkdir("/data/elf-arsenal", 0755);
    int fd = open("/data/elf-arsenal/.first_boot_redirect_pending",
                  O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(fd >= 0) close(fd);

    sleep(3);
    notify("Elf Arsenal first boot: open Settings → Install kstuff-lite "
           "+ ShadowMountPlus to finish setup.");

#ifdef EA_AUTOLAUNCH_HBL
    sleep(1);
    sys_launch_title("FAKE00000", "");
#endif

    write_first_boot_marker();
  }

  /* kstuff spawns last so all other daemons are already running when
     kernel patches are applied. */
  if(stat(KSTUFF_INSTALL_PATH, &st) == 0 && st.st_size > 4) {
    uint8_t *kstuff_buf = NULL;
    size_t   kstuff_buf_size = 0;
    kstuff_buf = read_elf_file(KSTUFF_INSTALL_PATH, &kstuff_buf_size);
    if(kstuff_buf) {
      spawn_raw("kstuff-lite", kstuff_buf, kstuff_buf_size);
      free(kstuff_buf);
    }
  } else {
    notify("Elf Arsenal: no kstuff installed yet. Open Homebrew Loader "
           "or http://<your-ps5-ip>:6969/ → Settings → Install kstuff-lite "
           "to enable kernel patches.");
  }

  /* No boot toast here — sys_notify_address() already fired the single
     "Elf Arsenal serving HTTP on …" notification. */
}


int
sys_backpork_is_running(void) {
  return sys_find_pid(BACKPORK_PROC_NAME) > 0 ? 1 : 0;
}


int
sys_nanodns_is_running(void) {
  return sys_find_pid(NANODNS_PROC_NAME) > 0 ? 1 : 0;
}


int
sys_wait_for_nanodns(int timeout_sec) {
  if(timeout_sec <= 0) return sys_nanodns_is_running();
  int ticks = timeout_sec * 2;   /* 500 ms per tick */
  for(int i = 0; i < ticks; i++) {
    if(sys_nanodns_is_running()) {
      fprintf(stderr,
              "wait_for_nanodns: ready after %d.%01d s\n",
              i / 2, (i % 2) * 5);
      return 1;
    }
    usleep(500 * 1000);
  }
  fprintf(stderr,
          "wait_for_nanodns: nanodns didn't come up in %d s "
          "— downstream HTTP fetches may fail DNS\n", timeout_sec);
  return 0;
}


int
sys_nanodns_set_enabled(int on) {
  pid_t existing;
  int rc;

  if(on) {
    if(sys_find_pid(NANODNS_PROC_NAME) > 0) {
      rc = 1;
      goto nanodns_persist;
    }
    if(spawn_embedded("nanodns", nanodns_elf, nanodns_elf_size) != 0) {
      rc = -1;
      goto nanodns_persist;
    }
    /* Wait for the proc to register so the UI toggle doesn't snap
       back off — same backstop pattern the etaHEN toggle uses. */
    rc = -1;
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(NANODNS_PROC_NAME) > 0) { rc = 1; break; }
      usleep(100000);
    }
    goto nanodns_persist;
  }

  while((existing = sys_find_pid(NANODNS_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill nanodns");
      rc = -1;
      goto nanodns_persist;
    }
    sleep(1);
  }
  rc = 0;

nanodns_persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


int
sys_backpork_set_enabled(int on) {
  pid_t existing;
  int rc;

  if(on) {
    if(sys_find_pid(BACKPORK_PROC_NAME) > 0) {
      rc = 1;
      goto persist;
    }
    if(spawn_embedded("backpork", backpork_elf, backpork_elf_size) != 0) {
      rc = -1;
      goto persist;
    }
    rc = -1;
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(BACKPORK_PROC_NAME) > 0) { rc = 1; break; }
      usleep(100000);  /* 100 ms */
    }
    if(rc != 1) {
      /* Spawn returned 0 but the proc never showed up — surface the
         failure to the UI rather than silently flipping back to off. */
      rc = -1;
    }
    goto persist;
  }

  /* Off — kill any running instance(s). */
  while((existing = sys_find_pid(BACKPORK_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill backpork");
      rc = -1;
      goto persist;
    }
    sleep(1);
  }
  rc = 0;

persist:
  /* Persist the desired state so it survives a redeploy. */
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


/* ── DPI v2 — HTTP install bridge, public port 12800 ─────────────────── */

int
sys_dpiv2_is_running(void) {
  return sys_find_pid(DPIV2_PROC_NAME) > 0 ? 1 : 0;
}

int
sys_dpiv2_get_enabled(void) {
  return atomic_load(&g_dpiv2_enabled);
}

int
sys_dpiv2_set_enabled(int on) {
  int was = atomic_exchange(&g_dpiv2_enabled, on ? 1 : 0);
  pid_t existing;
  int rc;

  if(on) {
    /* Kill DPI v1 first so both don't compete for installs */
    pid_t dpi1;
    while((dpi1 = sys_find_pid("dpi")) > 0) {
      kill(dpi1, SIGKILL);
      usleep(200000);
    }
    if(sys_find_pid(DPIV2_PROC_NAME) > 0) { rc = 1; goto dpiv2_persist; }
    if(spawn_embedded(DPIV2_PROC_NAME, dpiv2_elf, dpiv2_elf_size) != 0) {
      rc = -1; goto dpiv2_persist;
    }
    rc = -1;
    for(int i = 0; i < 30; i++) {
      pid_t p = sys_find_pid(DPIV2_PROC_NAME);
      if(p > 0) {
        jb_escalate_pid(p);  /* authid for sceAppInstUtilInitialize on FW 11+ */
        rc = 1;
        break;
      }
      usleep(100000);
    }
    goto dpiv2_persist;
  }

  /* Turning off v2 — only kill+restart v1 if v2 was actually enabled.
     Avoids double-spawning dpi at boot when config_load reads dpiv2=0. */
  if(!was) { rc = 0; goto dpiv2_persist; }

  while((existing = sys_find_pid(DPIV2_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill dpiv2");
      rc = -1;
      goto dpiv2_persist;
    }
    sleep(1);
  }
  sys_dpi_ensure_running();
  rc = 0;

dpiv2_persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


int
sys_smp_is_running(void) {
  return sys_find_pid(SHADOWMOUNT_PROC_NAME) > 0 ? 1 : 0;
}


/* ── Task Manager — process list ─────────────────────────────────────── */
char *
sys_proc_list_json(void) {
  int mib[4] = {1, 14, 8, 0};
  size_t buf_size = 0;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0) || !buf_size) return NULL;
  if(!(buf = malloc(buf_size))) return NULL;
  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) { free(buf); return NULL; }

  size_t out_cap = 4096 + (buf_size / 4);
  char *out = malloc(out_cap);
  if(!out) { free(buf); return NULL; }

  size_t pos = 0;
  out[pos++] = '[';
  int first = 1;

  for(uint8_t *ptr = buf; ptr + 8 <= buf + buf_size;) {
    int sz = *(int*)ptr;
    if(sz <= 0 || (size_t)sz > buf_size || ptr + sz > buf + buf_size) break;

    pid_t ki_pid  = *(pid_t*)&ptr[72];
    pid_t ki_ppid = *(pid_t*)&ptr[76];
    char *raw     = (char*)&ptr[447];

    char name[21];
    int ni;
    for(ni = 0; ni < 20 && raw[ni]; ni++) {
      char c = raw[ni];
      name[ni] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? c : '?';
    }
    name[ni] = 0;

    if(ni > 0) {
      if(!first) {
        if(pos + 2 >= out_cap) {
          out_cap *= 2;
          char *t = realloc(out, out_cap);
          if(!t) { free(out); free(buf); return NULL; }
          out = t;
        }
        out[pos++] = ',';
      }
      first = 0;
      int n = snprintf(out + pos, out_cap - pos,
        "{\"pid\":%d,\"ppid\":%d,\"name\":\"%s\"}", ki_pid, ki_ppid, name);
      if(n > 0) pos += (size_t)n;
    }

    ptr += sz;
  }

  if(pos + 4 >= out_cap) {
    char *t = realloc(out, out_cap + 4);
    if(!t) { free(out); free(buf); return NULL; }
    out = t;
  }
  out[pos++] = ']';
  out[pos]   = 0;
  free(buf);
  return out;
}


static int
spawn_smp(void) {
  struct stat st;
  if(stat(SHADOWMOUNT_INSTALL_PATH, &st) != 0 || st.st_size <= 64) {
    notify("ShadowMountPlus not installed — pick a release in Settings.");
    return -1;
  }
  size_t sz = 0;
  uint8_t *buf = read_elf_file(SHADOWMOUNT_INSTALL_PATH, &sz);
  if(!buf) return -1;
  int rc = spawn_raw("shadowmountplus", buf, sz);   /* raw disk ELF */
  free(buf);
  return rc;
}


int
sys_smp_set_enabled(int on) {
  pid_t existing;
  int rc;

  if(on) {
    if(sys_find_pid(SHADOWMOUNT_PROC_NAME) > 0) { rc = 1; goto persist; }
    if(spawn_smp() != 0) { rc = -1; goto persist; }
    rc = 1;
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(SHADOWMOUNT_PROC_NAME) > 0) break;
      usleep(100000);
    }
    goto persist;
  }

  while((existing = sys_find_pid(SHADOWMOUNT_PROC_NAME)) > 0) {
    if(kill(existing, SIGTERM) != 0) {
      perror("kill shadowmountplus");
      rc = -1;
      goto persist;
    }
    /* Give SMP up to ~5s to exit cleanly before escalating. */
    int exited = 0;
    for(int i = 0; i < 50; i++) {
      usleep(100000);
      if(sys_find_pid(SHADOWMOUNT_PROC_NAME) <= 0) { exited = 1; break; }
    }
    if(!exited) kill(existing, SIGKILL);
  }
  rc = 0;

persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


int
sys_smp_restart(void) {
  pid_t pid = sys_find_pid(SHADOWMOUNT_PROC_NAME);
  if(pid > 0) {
    kill(pid, SIGTERM);
    for(int i = 0; i < 10; i++) {
      usleep(100000);
      if(sys_find_pid(SHADOWMOUNT_PROC_NAME) <= 0) break;
    }
    if((pid = sys_find_pid(SHADOWMOUNT_PROC_NAME)) > 0) {
      kill(pid, SIGKILL);
      for(int i = 0; i < 5; i++) {
        usleep(50000);
        if(sys_find_pid(SHADOWMOUNT_PROC_NAME) <= 0) break;
      }
    }
  }

  int rc = spawn_smp();
  if(rc != 0) return rc;

  for(int i = 0; i < 30; i++) {
    if(sys_find_pid(SHADOWMOUNT_PROC_NAME) > 0) break;
    usleep(100000);
  }
  return 0;
}


/* Aggressively SIGKILL every SMP instance with no SIGTERM — used on wake
   where a stale SMP must be cleared before a clean respawn. Hammers until
   the process table shows it gone, or gives up after ~3 seconds. */
static void
kill_smp_aggressive(void) {
  for(int attempt = 0; attempt < 6; attempt++) {
    pid_t pid = sys_find_pid(SHADOWMOUNT_PROC_NAME);
    if(pid <= 0) return;
    kill(pid, SIGKILL);
    for(int i = 0; i < 10; i++) {
      usleep(100000);
      if(sys_find_pid(SHADOWMOUNT_PROC_NAME) <= 0) return;
    }
  }
  fprintf(stderr, "wake_watchdog: SMP still alive after aggressive kill — giving up\n");
}

/* Poll every 5 s using wall-clock drift: if real time jumped by more than
   3× the poll interval the process was suspended → console woke from sleep.
   On wake, tear down the stale SMP aggressively and respawn it. */
#define WAKE_POLL_S    5
#define WAKE_THRESH_S  15

static void *
wake_watchdog_thread(void *arg) {
  (void)arg;
  syscall(SYS_thr_set_name, -1, "wake-watchdog");

  for(;;) {
    time_t t0 = time(NULL);
    sleep(WAKE_POLL_S);
    time_t elapsed = time(NULL) - t0;

    if(elapsed < WAKE_THRESH_S) continue;

    fprintf(stderr,
            "wake_watchdog: resume detected (slept %llds) — restarting SMP\n",
            (long long)elapsed);
    notify("Elf Arsenal: console resumed — restarting ShadowMountPlus\xe2\x80\xa6");

    kill_smp_aggressive();

    if(spawn_smp() == 0) {
      /* Wait up to 3 s for SMP to appear. */
      for(int i = 0; i < 30; i++) {
        if(sys_find_pid(SHADOWMOUNT_PROC_NAME) > 0) break;
        usleep(100000);
      }
      fprintf(stderr, "wake_watchdog: SMP respawned\n");
    } else {
      fprintf(stderr, "wake_watchdog: spawn_smp() failed after wake\n");
    }
  }
  return NULL;
}

void
sys_start_wake_watchdog(void) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_create(&t, &a, wake_watchdog_thread, NULL);
  pthread_attr_destroy(&a);
}



static atomic_int g_lapyjb_enabled = 0;

int
sys_lapyjb_get_enabled(void) {
  return atomic_load(&g_lapyjb_enabled);
}

int
sys_lapyjb_is_running(void) {
  return sys_find_pid(LAPYJB_PROC_NAME) > 0 ? 1 : 0;
}


int
sys_lapyjb_set_enabled(int on) {
  pid_t existing;
  int rc;

  atomic_store(&g_lapyjb_enabled, on ? 1 : 0);

  if(on) {
    if(sys_find_pid(LAPYJB_PROC_NAME) > 0) {
      rc = 1;
      goto lapyjb_persist;
    }
    if(spawn_embedded("LapyJB", lapyjb_elf, lapyjb_elf_size) != 0) {
      rc = -1;
      goto lapyjb_persist;
    }
    rc = -1;
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(LAPYJB_PROC_NAME) > 0) { rc = 1; break; }
      usleep(100000);
    }
    goto lapyjb_persist;
  }

  while((existing = sys_find_pid(LAPYJB_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill lapyjb");
      rc = -1;
      goto lapyjb_persist;
    }
    sleep(1);
  }
  rc = 0;

lapyjb_persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


/* ───── ps5-app-dumper (EchoStretch) — one-shot spawn ───── */

int
sys_spawn_app_dumper(void) {
  return spawn_embedded("app-dumper",
                        ps5_app_dumper_elf, ps5_app_dumper_elf_size);
}


static atomic_int g_cheatrunner_enabled = 0;

int
sys_cheatrunner_get_enabled(void) {
  return atomic_load(&g_cheatrunner_enabled);
}

int
sys_cheatrunner_is_running(void) {
  return sys_find_pid(CHEATRUNNER_PROC_NAME) > 0 ? 1 : 0;
}

int
sys_cheatrunner_set_enabled(int on) {
  atomic_store(&g_cheatrunner_enabled, on ? 1 : 0);
  pid_t existing;
  int rc;

  if(on) {
    if(sys_find_pid(CHEATRUNNER_PROC_NAME) > 0) {
      rc = 1;
      goto cr_persist;
    }
    if(spawn_embedded("CheatRunner",
                      cheatrunner_elf, cheatrunner_elf_size) != 0) {
      rc = -1;
      goto cr_persist;
    }
    rc = -1;
    for(int i = 0; i < 30; i++) {
      pid_t cr_pid = sys_find_pid(CHEATRUNNER_PROC_NAME);
      if(cr_pid > 0) {
        jb_escalate_pid(cr_pid);
        rc = 1;
        break;
      }
      usleep(100000);
    }
    goto cr_persist;
  }

  while((existing = sys_find_pid(CHEATRUNNER_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill cheatrunner");
      rc = -1;
      goto cr_persist;
    }
    sleep(1);
  }
  rc = 0;

cr_persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


static atomic_int g_fpkgguard_enabled = 0;

int
sys_fpkgguard_get_enabled(void) {
  return atomic_load(&g_fpkgguard_enabled);
}

int
sys_fpkgguard_is_running(void) {
  return sys_find_pid(FPKG_GUARD_PROC_NAME) > 0 ? 1 : 0;
}

int
sys_fpkgguard_set_enabled(int on) {
  pid_t existing;
  int rc;

  atomic_store(&g_fpkgguard_enabled, on ? 1 : 0);

  if(on) {
    if(sys_find_pid(FPKG_GUARD_PROC_NAME) > 0) {
      rc = 1;
      goto fg_persist;
    }
    if(spawn_embedded("fpkg-guard", fpkg_guard_elf, fpkg_guard_elf_size) != 0) {
      rc = -1;
      goto fg_persist;
    }
    rc = -1;
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(FPKG_GUARD_PROC_NAME) > 0) { rc = 1; break; }
      usleep(100000);
    }
    goto fg_persist;
  }

  while((existing = sys_find_pid(FPKG_GUARD_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill fpkg-guard");
      rc = -1;
      goto fg_persist;
    }
    sleep(1);
  }
  rc = 0;

fg_persist:
  {
    extern void config_save(void);
    config_save();
  }
  return rc;
}


uint32_t
sys_get_foreground_user(char *name_out, size_t name_out_size) {
  uint32_t uid = 0;
  if(sceUserServiceGetForegroundUser(&uid) != 0 || uid == 0) {
    if(name_out && name_out_size > 0) name_out[0] = 0;
    return 0;
  }
  if(name_out && name_out_size > 0) {
    char tmp[17] = {0};
    if(sceUserServiceGetUserName((int32_t)uid, tmp, sizeof(tmp)) != 0) {
      tmp[0] = 0;
    }
    size_t n = strlen(tmp);
    if(n >= name_out_size) n = name_out_size - 1;
    memcpy(name_out, tmp, n);
    name_out[n] = 0;
  }
  return uid;
}


#define NP_FAKE_SIGNIN_PATH "/data/elf-arsenal/np-fake-signin.elf"
#define NP_FAKE_SIGNIN_URL  \
    "https://git.etawen.dev/soniciso/elf-arsenal/raw/branch/main/" \
    "payloads/np-fake-signin.elf"

static int
np_fake_signin_ensure_on_disk(void) {
  struct stat st;
  if(stat(NP_FAKE_SIGNIN_PATH, &st) == 0 && st.st_size > 1024) {
    return 0;  /* already cached */
  }

  /* Lazy fetch. */
  fprintf(stderr,
          "np-fake-signin: cache miss, fetching from %s\n",
          NP_FAKE_SIGNIN_URL);
  size_t len = 0;
  uint8_t *body = http_get(NP_FAKE_SIGNIN_URL, &len);
  if(!body || len < 1024) {
    free(body);
    fprintf(stderr,
            "np-fake-signin: download failed (got %zu bytes)\n", len);
    return -1;
  }
  /* ELF magic sniff so a CDN HTML page doesn't get spawned. */
  if(body[0] != 0x7f || body[1] != 'E' ||
     body[2] != 'L'  || body[3] != 'F') {
    free(body);
    fprintf(stderr,
            "np-fake-signin: payload from gitea raw is not an ELF\n");
    return -1;
  }

  mkdir("/data/elf-arsenal", 0755);
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s.tmp", NP_FAKE_SIGNIN_PATH);
  int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0755);
  if(fd < 0) {
    free(body);
    perror("np-fake-signin: open .tmp");
    return -1;
  }
  size_t off = 0;
  while(off < len) {
    ssize_t w = write(fd, body + off, len - off);
    if(w <= 0) { close(fd); unlink(tmp); free(body); return -1; }
    off += (size_t)w;
  }
  fsync(fd);
  close(fd);
  if(rename(tmp, NP_FAKE_SIGNIN_PATH) != 0) {
    unlink(tmp);
    free(body);
    perror("np-fake-signin: rename");
    return -1;
  }
  free(body);
  fprintf(stderr, "np-fake-signin: cached %zu bytes → %s\n",
          len, NP_FAKE_SIGNIN_PATH);
  return 0;
}

int
sys_spawn_np_fake_signin(void) {
  if(np_fake_signin_ensure_on_disk() != 0) return -1;

  size_t buf_sz = 0;
  uint8_t *buf = fs_readfile(NP_FAKE_SIGNIN_PATH, &buf_sz);
  if(!buf || buf_sz < 1024) {
    free(buf);
    fprintf(stderr, "np-fake-signin: fs_readfile failed\n");
    return -1;
  }
  int rc = spawn_raw("np-fake-signin", buf, buf_sz);   /* raw disk ELF */
  free(buf);
  return rc;
}


int
sys_spawn_np_restore_account(void) {
  return spawn_embedded("np-restore-account",
                        np_restore_account_elf, np_restore_account_elf_size);
}


/* ───── Garlic — community save-processing worker + interactive savemgr ───── */

#define GARLIC_DIR         "/data/garlic"
#define GARLIC_CONFIG_PATH "/data/garlic/config.ini"

static const char *GARLIC_DEFAULT_HOST     = "garlicsaves.com";
static const int   GARLIC_DEFAULT_PORT     = 80;
static const char *GARLIC_DEFAULT_KEY      =
  "9f1378109a83aa79a9e10e8f5523c4aad3fa6880c8f7da8d749c58a25c522f34";
static const int   GARLIC_DEFAULT_POLL     = 30;


static int
garlic_write_config(int poll_interval) {
  mkdir("/data", 0755);
  mkdir(GARLIC_DIR, 0777);
  FILE *f = fopen(GARLIC_CONFIG_PATH ".tmp", "w");
  if(!f) return -1;
  fprintf(f,
    "serverHost=%s\n"
    "serverPort=%d\n"
    "workerKey=%s\n"
    "pollInterval=%d\n",
    GARLIC_DEFAULT_HOST, GARLIC_DEFAULT_PORT,
    GARLIC_DEFAULT_KEY, poll_interval);
  fclose(f);
  return rename(GARLIC_CONFIG_PATH ".tmp", GARLIC_CONFIG_PATH);
}


static int
garlic_force_canonical_key(void) {
  FILE *f = fopen(GARLIC_CONFIG_PATH, "r");
  if(!f) return -1;

  /* Slurp the file. Cap at 4 KB — config.ini is tiny. */
  char  buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';

  /* Walk lines: keep everything except a workerKey= line, replace that
     with the canonical key. If no workerKey= line exists, append one. */
  char out[4200];
  size_t outlen = 0;
  int    saw_key = 0;
  char  *line = buf;
  while(line && *line) {
    char *eol = strpbrk(line, "\r\n");
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    if(len >= 10 && strncmp(line, "workerKey=", 10) == 0) {
      saw_key = 1;
      int wrote = snprintf(out + outlen, sizeof(out) - outlen,
                           "workerKey=%s\n", GARLIC_DEFAULT_KEY);
      if(wrote < 0 || (size_t)wrote >= sizeof(out) - outlen) return -1;
      outlen += (size_t)wrote;
    } else if(len > 0) {
      if(outlen + len + 1 >= sizeof(out)) return -1;
      memcpy(out + outlen, line, len);
      outlen += len;
      out[outlen++] = '\n';
    }
    if(!eol) break;
    line = eol + 1;
    while(*line == '\r' || *line == '\n') line++;
  }
  if(!saw_key) {
    int wrote = snprintf(out + outlen, sizeof(out) - outlen,
                         "workerKey=%s\n", GARLIC_DEFAULT_KEY);
    if(wrote < 0 || (size_t)wrote >= sizeof(out) - outlen) return -1;
    outlen += (size_t)wrote;
  }

  FILE *o = fopen(GARLIC_CONFIG_PATH ".tmp", "w");
  if(!o) return -1;
  if(fwrite(out, 1, outlen, o) != outlen) { fclose(o); return -1; }
  fclose(o);
  return rename(GARLIC_CONFIG_PATH ".tmp", GARLIC_CONFIG_PATH);
}


void
sys_garlic_seed_config(void) {
  struct stat st;
  if(stat(GARLIC_CONFIG_PATH, &st) != 0 || st.st_size <= 0) {
    /* No config yet — write the full default. */
    if(garlic_write_config(GARLIC_DEFAULT_POLL) == 0) {
      printf("garlic: seeded %s\n", GARLIC_CONFIG_PATH);
    }
    return;
  }
  if(garlic_force_canonical_key() == 0) {
    printf("garlic: pinned canonical workerKey in %s\n", GARLIC_CONFIG_PATH);
  }
}


int
sys_garlic_get_poll_interval(void) {
  FILE *f = fopen(GARLIC_CONFIG_PATH, "r");
  if(!f) return GARLIC_DEFAULT_POLL;
  char line[256];
  int v = GARLIC_DEFAULT_POLL;
  while(fgets(line, sizeof(line), f)) {
    int n;
    if(sscanf(line, "pollInterval=%d", &n) == 1 && n > 0) { v = n; break; }
  }
  fclose(f);
  return v;
}


int
sys_garlic_set_poll_interval(int seconds) {
  if(seconds < 5)    seconds = 5;
  if(seconds > 3600) seconds = 3600;
  /* Read current config, rewrite with new pollInterval, preserve other keys. */
  char host[128] = {0};
  int port = GARLIC_DEFAULT_PORT;
  char key[128] = {0};
  FILE *f = fopen(GARLIC_CONFIG_PATH, "r");
  if(f) {
    char line[256];
    while(fgets(line, sizeof(line), f)) {
      sscanf(line, "serverHost=%127[^\r\n]", host);
      sscanf(line, "serverPort=%d", &port);
      sscanf(line, "workerKey=%127[^\r\n]", key);
    }
    fclose(f);
  }
  if(!host[0]) strncpy(host, GARLIC_DEFAULT_HOST, sizeof(host)-1);
  /* workerKey is always forced to the canonical community value — never
     trust whatever was previously on disk. */
  (void)key;
  strncpy(key, GARLIC_DEFAULT_KEY, sizeof(key)-1);
  key[sizeof(key)-1] = '\0';

  mkdir("/data", 0755);
  mkdir(GARLIC_DIR, 0777);
  FILE *o = fopen(GARLIC_CONFIG_PATH ".tmp", "w");
  if(!o) return -1;
  fprintf(o,
    "serverHost=%s\nserverPort=%d\nworkerKey=%s\npollInterval=%d\n",
    host, port, key, seconds);
  fclose(o);
  return rename(GARLIC_CONFIG_PATH ".tmp", GARLIC_CONFIG_PATH);
}


int
sys_garlic_worker_is_running(void) {
  return sys_find_pid(GARLIC_WORKER_PROC_NAME) > 0 ? 1 : 0;
}


int
sys_garlic_worker_set_enabled(int on) {
  pid_t existing;
  if(on) {
    if(sys_find_pid(GARLIC_WORKER_PROC_NAME) > 0) return 1;
    sys_garlic_seed_config();
    if(spawn_embedded("garlic-worker",
                      garlic_worker_elf, garlic_worker_elf_size) != 0) {
      return -1;
    }
    /* Same race-mitigation as BackPork — wait for the proc to register. */
    for(int i = 0; i < 30; i++) {
      if(sys_find_pid(GARLIC_WORKER_PROC_NAME) > 0) return 1;
      usleep(100000);
    }
    return -1;
  }
  while((existing = sys_find_pid(GARLIC_WORKER_PROC_NAME)) > 0) {
    if(kill(existing, SIGKILL) != 0) {
      perror("kill garlic-worker");
      return -1;
    }
    sleep(1);
  }
  return 0;
}


/* SaveMgr probe — TCP connect to 127.0.0.1:8082 with a 250 ms timeout. */
static int
garlic_savemgr_port_open(void) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if(s < 0) return 0;
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port   = htons(8082);
  a.sin_addr.s_addr = htonl(0x7f000001);
  int rc = connect(s, (struct sockaddr*)&a, sizeof(a));
  int ok = 0;
  if(rc == 0) {
    ok = 1;
  } else if(errno == EINPROGRESS) {
    fd_set wset; FD_ZERO(&wset); FD_SET(s, &wset);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
    if(select(s + 1, NULL, &wset, NULL, &tv) > 0) {
      int err = 0; socklen_t el = sizeof(err);
      if(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) ok = 1;
    }
  }
  close(s);
  return ok;
}


int
sys_garlic_savemgr_is_running(void) {
  return garlic_savemgr_port_open();
}


/* Track our own savemgr child so we can kill it. The bundled binary
   doesn't set a thread name, so PID lookup by name doesn't work. */
static pid_t g_savemgr_pid = 0;

int
sys_garlic_savemgr_set_enabled(int on) {
  if(on) {
    if(garlic_savemgr_port_open()) return 1;
    char* argv[2] = {"garlic-savemgr", 0};
    char* envp[1] = {0};
    size_t sm_size = 0;
    uint8_t *sm_elf = sys_gz_inflate(garlic_savemgr_elf,
                                     garlic_savemgr_elf_size, &sm_size);
    if(!sm_elf) return -1;
    int devnull = open("/dev/null", O_WRONLY);
    pid_t pid = elfldr_spawn("/", devnull, sm_elf, argv, envp);
    if(devnull >= 0) close(devnull);
    free(sm_elf);
    if(pid < 0) return -1;
    g_savemgr_pid = pid;
    /* Wait up to ~3s for the listener to bind. */
    for(int i = 0; i < 30; i++) {
      if(garlic_savemgr_port_open()) return 1;
      usleep(100000);
    }
    return -1;
  }
  if(g_savemgr_pid > 0) {
    if(kill(g_savemgr_pid, SIGKILL) == 0) {
      g_savemgr_pid = 0;
    }
  }
  /* SaveMgr's port may take a beat to release. */
  for(int i = 0; i < 10; i++) {
    if(!garlic_savemgr_port_open()) break;
    usleep(100000);
  }
  return 0;
}


__attribute__((constructor)) static void
sys_init(void) {
  pid_t pid;
  int err;

  signal(SIGSEGV, on_fatal_signal);
  signal(SIGABRT, on_fatal_signal);
  signal(SIGFPE, on_fatal_signal);
  signal(SIGILL, on_fatal_signal);
  signal(SIGBUS, on_fatal_signal);
  signal(SIGTRAP, on_fatal_signal);
  signal(SIGSYS, on_fatal_signal);

  kernel_set_ucred_authid(-1, 0x4801000000000013L);

  /* Self-name so any future Elf Arsenal can find + kill us too. */
  syscall(SYS_thr_set_name, -1, "elf-arsenal.elf");

  /* Kill any prior instance. Cap the wait so a stuck old instance
     can't deadlock our boot. */
  for(int attempts = 0; attempts < 10; attempts++) {
    pid = sys_find_pid("elf-arsenal.elf");
    if(pid <= 0) break;
    if(kill(pid, SIGKILL) != 0) {
      perror("kill old elf-arsenal.elf");
      kill(pid, SIGTERM);
      break;
    }
    sleep(1);
  }

  if((err=sceUserServiceInitialize(0)) && err != 0x80960003 /* SCE_USER_SERVICE_ERROR_NOT_TERMINATED */) {
    perror("sceUserServiceInitialize");
    /* fall through — we can still serve the web UI without it. */
  }

  /* Elf Arsenal: HTTP-serving toast suppressed per user request. The
     web UI is at http://<console-ip>:6969/ — find the IP via FTP/klog. */
}


__attribute__((destructor)) static void
sys_fini(void) {
  sceUserServiceTerminate();
}
