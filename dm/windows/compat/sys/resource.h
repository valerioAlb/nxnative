// <sys/resource.h> on Windows: thread priorities stand in for nice values (setpriority on a thread id: SetThreadPriority), getrusage from GetProcessTimes
#pragma once
#include "dm_windows.h"
#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2
#define PRIO_DARWIN_THREAD 3
#define PRIO_DARWIN_BG 0x1000
#define RUSAGE_SELF 0
#define RUSAGE_THREAD 1
#define RLIMIT_NOFILE 7
#define RLIMIT_STACK 3
#define RLIMIT_CORE 4
#define RLIMIT_AS 9
#define RLIM_INFINITY (~0ull)
typedef uint64_t rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
struct rusage { struct timeval ru_utime, ru_stime; long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw; };
int setpriority(int which, id_t who, int prio);
int getpriority(int which, id_t who);
int getrlimit(int r, struct rlimit *l);
int setrlimit(int r, const struct rlimit *l);
int getrusage(int who, struct rusage *u);
