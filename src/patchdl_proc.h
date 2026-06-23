#pragma once

/* Set this process's (main thread) name as shown in the PS5 process list. */
void patchdl_proc_set_name(const char *name);

/* SIGKILL any other process whose name matches `name` (excluding self),
   so a freshly launched payload replaces a stale instance cleanly.
   Returns the number of processes killed. */
int patchdl_proc_kill_others(const char *name);
