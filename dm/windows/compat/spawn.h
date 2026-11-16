// posix_spawn on CreateProcess (posix_windows.c): the program and its arguments, the environment given; file actions and attributes are accepted and ignored
#pragma once
#include "dm_windows.h"
typedef struct { int unused; } posix_spawn_file_actions_t;
typedef struct { int unused; } posix_spawnattr_t;
static inline int posix_spawn_file_actions_init(posix_spawn_file_actions_t *a) { (void)a; return 0; }
static inline int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *a) { (void)a; return 0; }
static inline int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *a, int fd, const char *p, int fl, int m) { (void)a; (void)fd; (void)p; (void)fl; (void)m; return 0; }
static inline int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *a, int fd, int to) { (void)a; (void)fd; (void)to; return 0; }
static inline int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *a, int fd) { (void)a; (void)fd; return 0; }
static inline int posix_spawnattr_init(posix_spawnattr_t *a) { (void)a; return 0; }
static inline int posix_spawnattr_destroy(posix_spawnattr_t *a) { (void)a; return 0; }
static inline int posix_spawnattr_setflags(posix_spawnattr_t *a, short f) { (void)a; (void)f; return 0; }
#define POSIX_SPAWN_SETSIGMASK 8
#define POSIX_SPAWN_SETSIGDEF 4
static inline int posix_spawnattr_setsigmask(posix_spawnattr_t *a, const sigset_t *s) { (void)a; (void)s; return 0; }
static inline int posix_spawnattr_setsigdefault(posix_spawnattr_t *a, const sigset_t *s) { (void)a; (void)s; return 0; }
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const argv[], char *const envp[]);
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const argv[], char *const envp[]);
