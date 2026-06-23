#include "patchdl_proc.h"

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/sysctl.h>

/* kinfo_proc field offsets for the FreeBSD 9-based PS5 kernel.
   Same layout used by ps5-payload-dev/ftpsrv (verified on fw 11.60):
   ki_pid at +72, ki_tdname (thread name) at +447, ki_structsize at +0. */
#define KINFO_OFF_STRUCTSIZE 0
#define KINFO_OFF_PID        72
#define KINFO_OFF_TDNAME     447

void
patchdl_proc_set_name(const char *name) {
    /* tid -1 = current thread */
    syscall(SYS_thr_set_name, -1, name);
}

static pid_t
find_pid(const char *name) {
    int      mib[4] = {1, 14, 8, 0}; /* CTL_KERN, KERN_PROC, KERN_PROC_ALL */
    pid_t    mypid = getpid();
    pid_t    pid = -1;
    size_t   buf_size;
    uint8_t *buf;

    if (sysctl(mib, 4, 0, &buf_size, 0, 0)) return -1;
    if (!(buf = malloc(buf_size)))          return -1;
    if (sysctl(mib, 4, buf, &buf_size, 0, 0)) { free(buf); return -1; }

    for (uint8_t *ptr = buf; ptr < buf + buf_size; ) {
        int   ki_structsize = *(int *)(ptr + KINFO_OFF_STRUCTSIZE);
        pid_t ki_pid        = *(pid_t *)(ptr + KINFO_OFF_PID);
        char *ki_tdname     = (char *)(ptr + KINFO_OFF_TDNAME);

        if (ki_structsize <= 0) break; /* guard against malformed entries */
        ptr += ki_structsize;

        if (!strcmp(name, ki_tdname) && ki_pid != mypid)
            pid = ki_pid;
    }

    free(buf);
    return pid;
}

int
patchdl_proc_kill_others(const char *name) {
    int   killed = 0;
    pid_t pid;

    while ((pid = find_pid(name)) > 0) {
        if (kill(pid, SIGKILL))
            break;
        killed++;
        sleep(1); /* let the port + process slot free up */
    }
    return killed;
}
