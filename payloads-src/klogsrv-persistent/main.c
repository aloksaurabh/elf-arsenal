/*
 * klogsrv-persistent — streams kernel log to TCP :3232, loops on
 * client disconnect instead of exiting.  Notifies once on first bind.
 *
 * On each new client connection:
 *   1. Dumps kern.msgbuf (the dmesg ring buffer) so historical boot
 *      messages appear immediately.
 *   2. Streams /dev/klog in real-time for new messages.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define KLOG_PORT 3232
#define KLOG_DEV  "/dev/klog"
#define BUF_SZ    4096

typedef struct { char pad[45]; char msg[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int, notify_req_t *, size_t, int);

static void
notify_once(void)
{
    char ip[INET_ADDRSTRLEN] = "?";
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!strncmp(p->ifa_name, "lo", 2)) continue;
            struct sockaddr_in *s = (struct sockaddr_in *)p->ifa_addr;
            inet_ntop(AF_INET, &s->sin_addr, ip, sizeof ip);
            if (strncmp(ip, "0.", 2)) break;
        }
        freeifaddrs(ifa);
    }
    notify_req_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.msg, sizeof req.msg,
             "Serving /dev/klog on %s:%d", ip, KLOG_PORT);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
    printf("[klogsrv] %s\n", req.msg);
}

/* Dump kern.msgbuf (the dmesg ring buffer) to csock.
   Returns 0 on success, -1 if the sysctl fails or write fails. */
static int
dump_msgbuf(int csock)
{
    size_t len = 0;
    /* First call: get size */
    if (sysctlbyname("kern.msgbuf", NULL, &len, NULL, 0) < 0 || len == 0)
        return -1;

    char *buf = malloc(len + 1);
    if (!buf) return -1;

    if (sysctlbyname("kern.msgbuf", buf, &len, NULL, 0) < 0) {
        free(buf);
        return -1;
    }

    /* kern.msgbuf is a raw ring buffer — ensure NUL-terminated */
    buf[len] = '\0';

    /* Write in chunks to avoid overwhelming the socket */
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(csock, buf + sent, len - sent);
        if (n <= 0) { free(buf); return -1; }
        sent += (size_t)n;
    }

    /* Separator so the client knows the historical dump ended */
    const char *sep = "\n--- live klog ---\n";
    write(csock, sep, strlen(sep));

    free(buf);
    return 0;
}

int
main(void)
{
    signal(SIGPIPE, SIG_IGN);
    syscall(SYS_thr_set_name, -1, "klogsrv");

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(KLOG_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(lsock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return 1;
    }
    if (listen(lsock, 1) < 0) { perror("listen"); return 1; }

    notify_once();

    for (;;) {
        int csock = accept(lsock, NULL, NULL);
        if (csock < 0) { sleep(1); continue; }

        /* Historical dump first */
        dump_msgbuf(csock);

        /* Then stream /dev/klog for new messages */
        int kfd = open(KLOG_DEV, O_RDONLY | O_NONBLOCK);
        if (kfd < 0) { close(csock); sleep(1); continue; }

        char buf[BUF_SZ];
        ssize_t n;
        while (1) {
            n = read(kfd, buf, sizeof buf);
            if (n < 0) {
                if (errno == EAGAIN) { usleep(50000); continue; }
                break;
            }
            if (n == 0) { usleep(50000); continue; }
            if (write(csock, buf, n) != n) break;
        }

        close(kfd);
        close(csock);
    }

    close(lsock);
    return 0;
}
