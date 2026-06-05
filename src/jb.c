
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <ps5/kernel.h>

#include "jb.h"


#define JB_AUTHID  0x4801000000000013ULL


int
jb_escalate_pid(pid_t pid) {
  if(pid <= 0) return -1;

  intptr_t proc = kernel_get_proc(pid);
  if(!proc) return -1;

  int rc = 0;

  /* uid → 0 (root) on every cred slot the kernel checks. */
  if(kernel_set_ucred_uid (pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_ruid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_svuid(pid, 0)!= 0) rc = -1;
  if(kernel_set_ucred_rgid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_svgid(pid,0) != 0) rc = -1;

  intptr_t rootvnode = kernel_get_root_vnode();
  if(rootvnode) {
    if(kernel_set_proc_rootdir(pid, rootvnode) != 0) rc = -1;
    if(kernel_set_proc_jaildir(pid, rootvnode) != 0) rc = -1;
  }

  if(kernel_set_ucred_authid(pid, JB_AUTHID) != 0) rc = -1;
  uint8_t caps[16];
  memset(caps, 0xff, sizeof(caps));
  if(kernel_set_ucred_caps(pid, caps) != 0) rc = -1;

  /* cr_sceAttr[0] = 0x80 — single byte high-attr flag. */
  if(kernel_set_ucred_attrs(pid, 0x80) != 0) rc = -1;

  return rc;
}
