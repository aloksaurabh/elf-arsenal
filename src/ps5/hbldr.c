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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <ps5/kernel.h>

#include "fs.h"
#include "elfldr.h"
#include "hbldr.h"
#include "pt.h"
#include "sys.h"
#include "websrv.h"


#define PSNOW_EBOOT "/system_ex/app/NPXS40106/eboot.bin"
#define FAKE_PATH "/system_ex/app/FAKE00000"
#define SYSTEM_EX_APP "/system_ex/app"

#define IOVEC_ENTRY(x) {x ? x : 0, \
			x ? strlen(x)+1 : 0}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))


static const char param_json[] = "{\n"
  "  \"applicationCategoryType\": 0,\n"
  "  \"attribute\": 1,\n"
  "  \"attribute2\": 0,\n"
  "  \"attribute3\": 4,\n"
  "  \"titleId\": \"FAKE00000\",\n"
  "  \"contentId\": \"IV9999-FAKE00000_00-HOMEBREWLOADER00\",\n"
  "  \"localizedParameters\": {\n"
  "    \"defaultLanguage\": \"en-US\",\n"
  "    \"en-US\": {\n"
  "      \"titleName\": \"Homebrew Loader\"\n"
  "    }\n"
  "  }\n"
  "}\n";


typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;

  // End of SCE fields, the following fields are just used to
  // pass arguments to bigapp_launch_thread().
  char **argv;
} app_launch_ctx_t;


int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceUserServiceTerminate(void);

int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char* title_id, char** argv,
			      app_launch_ctx_t* ctx);

int sceKernelGetAppState(int app_id, int*, int*);


static int
remount_system_ex(void) {
  struct iovec iov[] = {
    IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
    IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
    IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
    IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
    IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
    IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
    IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
  };

  if(nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE)) {
    return -1;
  }

  return 0;
}



static int
fakeapp_pick_base_eboot(char *out, size_t out_size) {
  /* Apps observed across PS5 firmwares 1.x → 12.x. Order = preference. */
  static const char *known[] = {
    PSNOW_EBOOT,                                  /* NPXS40106 (older FWs) */
    "/system_ex/app/NPXS40113/eboot.bin",         /* PS Cloud Gaming */
    "/system_ex/app/NPXS40104/eboot.bin",         /* PS+ */
    "/system_ex/app/NPXS40103/eboot.bin",         /* Trophies */
    "/system_ex/app/NPXS40000/eboot.bin",         /* Welcome */
    "/system_ex/app/NPXS40037/eboot.bin",         /* Initial setup */
    NULL,
  };
  for(int i = 0; known[i]; i++) {
    if(access(known[i], R_OK) == 0) {
      strncpy(out, known[i], out_size - 1);
      out[out_size - 1] = 0;
      return 0;
    }
  }
  /* Hardcoded list missed — scan /system_ex/app for any NPXS* dir.
     Skip FAKE00000 (that's us). First hit wins. */
  DIR *d = opendir(SYSTEM_EX_APP);
  if(!d) return -1;
  struct dirent *e;
  while((e = readdir(d))) {
    if(e->d_name[0] != 'N') continue;  /* NPXS… */
    if(!strncmp(e->d_name, "FAKE", 4)) continue;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path),
                     SYSTEM_EX_APP "/%s/eboot.bin", e->d_name);
    if(n <= 0 || (size_t)n >= sizeof(path)) continue;
    if(access(path, R_OK) == 0) {
      strncpy(out, path, out_size - 1);
      out[out_size - 1] = 0;
      closedir(d);
      return 0;
    }
  }
  closedir(d);
  return -1;
}


/* Per-step error sink so the caller can report the precise failing
   stage. Each step writes a short label here on failure. */
static char g_fakeapp_step[160] = "";

#define FAKEAPP_FAIL(stage)                                            \
  do {                                                                 \
    snprintf(g_fakeapp_step, sizeof(g_fakeapp_step),                   \
             "%s: %s", stage, errno ? strerror(errno) : "unknown");    \
  } while(0)


static int
fakeapp_create_if_missing(void) {
  struct stat info;
  uint8_t* buf;
  size_t size;
  int fd;

  g_fakeapp_step[0] = 0;
  errno = 0;

  if(stat(FAKE_PATH, &info)) {
    if(mkdir(FAKE_PATH, 0755)) {
      FAKEAPP_FAIL("mkdir " FAKE_PATH);
      return -1;
    }
  }

  if(stat(FAKE_PATH "/sce_sys", &info)) {
    if(mkdir(FAKE_PATH "/sce_sys", 0755)) {
      FAKEAPP_FAIL("mkdir " FAKE_PATH "/sce_sys");
      return -1;
    }
  }

  if(stat(FAKE_PATH "/sce_sys/param.json", &info)) {
    if((fd=open(FAKE_PATH "/sce_sys/param.json", O_CREAT|O_WRONLY, 0644)) < 0) {
      FAKEAPP_FAIL("open " FAKE_PATH "/sce_sys/param.json");
      return -1;
    }
    if(write(fd, param_json, sizeof(param_json)-1) != sizeof(param_json)-1) {
      FAKEAPP_FAIL("write param.json");
      close(fd);
      return -1;
    }
    close(fd);
  }

  if(stat(FAKE_PATH "/eboot.bin", &info)) {
    char base_eboot[PATH_MAX];
    if(fakeapp_pick_base_eboot(base_eboot, sizeof(base_eboot))) {
      snprintf(g_fakeapp_step, sizeof(g_fakeapp_step),
               "no base eboot.bin found under " SYSTEM_EX_APP
               " (tried NPXS40106/40113/40104/40103/40000/40037 + scan)");
      return -1;
    }
    if(!(buf=fs_readfile(base_eboot, &size))) {
      snprintf(g_fakeapp_step, sizeof(g_fakeapp_step),
               "fs_readfile %s failed", base_eboot);
      return -1;
    }
    if((fd=open(FAKE_PATH "/eboot.bin", O_CREAT|O_WRONLY, 0755)) < 0) {
      FAKEAPP_FAIL("open " FAKE_PATH "/eboot.bin");
      free(buf);
      return -1;
    }
    if(write(fd, buf, size) != size) {
      FAKEAPP_FAIL("write " FAKE_PATH "/eboot.bin");
      free(buf);
      close(fd);
      return -1;
    }
    free(buf);
    close(fd);
  }
  return 0;
}


const char *
fakeapp_last_step(void) {
  return g_fakeapp_step;
}


static pid_t
find_pid(const char* name) {
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
    return -1;
  }

  for(uint8_t *ptr=buf; ptr<(buf+buf_size);) {
    int ki_structsize = *(int*)ptr;
    pid_t ki_pid = *(pid_t*)&ptr[72];
    char *ki_tdname = (char*)&ptr[447];

    ptr += ki_structsize;
    if(!strcmp(name, ki_tdname) && mypid != ki_pid) {
      pid = ki_pid;
    }
  }

  free(buf);

  return pid;
}


static void*
bigapp_launch_thread(void* args) {
  app_launch_ctx_t *ctx  = (app_launch_ctx_t *)args;

  sceSystemServiceLaunchApp("FAKE00000", ctx->argv, ctx);

  return 0;
}


static pid_t
bigapp_launch(uint32_t user_id, char** argv) {
  app_launch_ctx_t ctx = {.user_id = user_id, .argv = argv};
  pthread_t trd;
  pid_t parent;
  pid_t child;

  if((parent=find_pid("SceSysCore.elf")) < 0) {
    puts("SceSysCore.elf is not running?");
    return -1;
  }

  if(pt_attach(parent) < 0) {
    perror("pt_attach");
    return -1;
  }

  if(pt_follow_fork(parent) < 0) {
    perror("pt_follow_fork");
    pt_detach(parent, 0);
    return -1;
  }

  if(pt_continue(parent, SIGCONT) < 0) {
    perror("pt_continue");
    pt_detach(parent, 0);
    return -1;
  }

  pthread_create(&trd, 0, &bigapp_launch_thread, &ctx);
  if((child=pt_await_child(parent)) < 0) {
    perror("pt_await_child");
    pt_detach(parent, 0);
    return -1;
  }

  if(pt_detach(parent, 0) < 0) {
    perror("pt_detach");
    return -1;
  }

  if(pt_follow_exec(child) < 0) {
    perror("pt_follow_exec");
    pt_detach(child, SIGKILL);
    return -1;
  }

  if(pt_continue(child, SIGCONT) < 0) {
    perror("pt_continue");
    pt_detach(child, SIGKILL);
    return -1;
  }

  if(pt_await_exec(child)) {
    perror("pt_await_exec");
    pt_detach(child, SIGKILL);
    return -1;
  }

  pthread_join(trd, 0);

  return child;
}


static int
bigapp_set_argv0(pid_t pid, const char* argv0) {
  intptr_t pos = pt_getargv(pid);
  intptr_t buf = 0;

  // allocate memory
  if((buf=pt_mmap(pid, 0, PAGE_SIZE, PROT_WRITE | PROT_READ,
		  MAP_ANONYMOUS | MAP_PRIVATE,
		  -1, 0)) == -1) {
    pt_perror(pid, "pt_mmap");
    return -1;
  }

  // copy string
  if(pt_copyin(pid, argv0, buf, strlen(argv0)+1)) {
    perror("pt_copyin");
    pt_munmap(pid, buf, PAGE_SIZE);
    return -1;
  }

  // copy pointer to string
  if(pt_setlong(pid, pos, buf)) {
    perror("pt_setlong");
    pt_munmap(pid, buf, PAGE_SIZE);
    return -1;
  }

  return 0;
}


static pid_t
bigapp_replace(pid_t pid, uint8_t* elf, const char* progname, int stdio,
	       const char* cwd, char** envp) {
  uint8_t int3instr = 0xcc;
  intptr_t brkpoint;
  uint8_t orginstr;

  // Let the kernel assign process parameters accessed via sceKernelGetProcParam()
  if(pt_syscall(pid, 599)) {
    puts("sys_dynlib_process_needed_and_relocate failed");
    //return -1;
  }

  // Allow libc to allocate arbitrary amount of memory.
  elfldr_set_heap_size(pid, -1);

  if(!(brkpoint=kernel_dynlib_entry_addr(pid, 0))) {
    puts("kernel_dynlib_entry_addr failed");
    return -1;
  }
  brkpoint += 58;// offset to invocation of main()

  if(kernel_mprotect(pid, brkpoint, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC)) {
    puts("kernel_mprotect failed");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  if(pt_copyout(pid, brkpoint, &orginstr, sizeof(orginstr))) {
    perror("pt_copyout");
    return -1;
  }
  if(pt_copyin(pid, &int3instr, brkpoint, sizeof(int3instr))) {
    perror("pt_copyin");
    return -1;
  }

  // Continue execution until we hit the breakpoint, then remove it.
  if(pt_continue(pid, SIGCONT)) {
    perror("pt_continue");
    return -1;
  }
  if(waitpid(pid, 0, 0) == -1) {
    perror("waitpid");
    return -1;
  }
  if(pt_copyin(pid, &orginstr, brkpoint, sizeof(orginstr))) {
    perror("pt_copyin");
    return -1;
  }

  bigapp_set_argv0(pid, progname);
  elfldr_set_procname(pid, basename(progname));
  elfldr_set_environ(pid, envp);
  elfldr_set_cwd(pid, cwd);
  elfldr_set_stdio(pid, stdio);

#if 0
  pt_syscall(pid, 0x23b, 0);
#endif
  
  // Execute the ELF
  if(elfldr_exec(pid, elf)) {
    return -1;
  }

  return pid;
}


static char g_hbldr_last_error[256] = "";
static int  g_hbldr_last_errno = 0;

const char *
hbldr_last_error(void) {
  return g_hbldr_last_error;
}

int
hbldr_last_errno(void) {
  return g_hbldr_last_errno;
}

#define HBLDR_FAIL(stage)                                              \
  do {                                                                 \
    g_hbldr_last_errno = errno;                                        \
    snprintf(g_hbldr_last_error, sizeof(g_hbldr_last_error),           \
             "%s: %s", stage,                                          \
             g_hbldr_last_errno ? strerror(g_hbldr_last_errno)         \
                                : "unknown");                          \
    perror(stage);                                                     \
  } while(0)

pid_t
hbldr_launch(const char*cwd, const char* path, int stdio, char** argv,
	     char** envp) {
  char buf[PATH_MAX];
  uint32_t user_id;
  uint8_t* elf;
  int app_id;
  pid_t pid;

  /* Reset diagnostic state for this attempt. */
  g_hbldr_last_error[0] = 0;
  g_hbldr_last_errno = 0;

  if(fakeapp_create_if_missing()) {
    if(remount_system_ex()) {
      HBLDR_FAIL("remount_system_ex");
      return -1;
    }
    if(fakeapp_create_if_missing()) {
      const char *step = fakeapp_last_step();
      g_hbldr_last_errno = errno;
      snprintf(g_hbldr_last_error, sizeof(g_hbldr_last_error),
               "fakeapp_create_if_missing: %s",
               (step && *step) ? step
                                : (errno ? strerror(errno) : "unknown"));
      fprintf(stderr, "%s\n", g_hbldr_last_error);
      return -1;
    }
  }

  if(sceUserServiceGetForegroundUser(&user_id)) {
    snprintf(g_hbldr_last_error, sizeof(g_hbldr_last_error),
             "sceUserServiceGetForegroundUser: no signed-in user "
             "(sign in to a PS5 user before launching homebrew)");
    fprintf(stderr, "%s\n", g_hbldr_last_error);
    return -1;
  }

  if((app_id=sceSystemServiceGetAppIdOfRunningBigApp()) > 0) {
    if(sceSystemServiceKillApp(app_id, -1, 0, 0)) {
      HBLDR_FAIL("sceSystemServiceKillApp");
      return -1;
    }

    while(!sceKernelGetAppState(app_id, 0, 0)) {
      printf("Waiting for App with id 0x%x to terminate\n", app_id);
      sleep(1);
    }
  }

  if(path[0] == '/') {
    strncpy(buf, path, sizeof(buf));
  } else {
    snprintf(buf, sizeof(buf), "%s/%s", cwd, path);
  }

  if(!(elf=fs_readfile(buf, 0))) {
    snprintf(g_hbldr_last_error, sizeof(g_hbldr_last_error),
             "fs_readfile: cannot read homebrew ELF at %.*s",
             (int)(sizeof(g_hbldr_last_error) - 64), buf);
    fprintf(stderr, "%s\n", g_hbldr_last_error);
    return -1;
  }

  if((pid=bigapp_launch(user_id, argv)) < 0) {
    snprintf(g_hbldr_last_error, sizeof(g_hbldr_last_error),
             "bigapp_launch: ptrace/SceSysCore handshake failed "
             "(jailbreak may have lapsed — try Settings → restart loader)");
    fprintf(stderr, "%s\n", g_hbldr_last_error);
    free(elf);
    return -1;
  }

  elfldr_raise_privileges(pid);

  if(bigapp_replace(pid, elf, path, stdio, cwd, envp) < 0) {
    snprintf(g_hbldr_last_error, sizeof(g_hbldr_last_error),
             "bigapp_replace: elfldr_exec rejected the homebrew ELF "
             "(check that %s is a valid prospero ELF)", path);
    fprintf(stderr, "%s\n", g_hbldr_last_error);
    pt_detach(pid, SIGKILL);
    pid = -1;
  }

  free(elf);

  return pid;
}
