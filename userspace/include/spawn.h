#ifndef __STUPIDOS_SPAWN_H__
#define __STUPIDOS_SPAWN_H__

#include <sys/types.h>
#include <sched.h>
#include <signal.h>

typedef struct
{
    int _opaque;
} posix_spawn_file_actions_t;

typedef struct
{
    short _flags;
    pid_t _pgrp;
    struct sched_param _sp;
    int _policy;
    sigset_t _ss;
    sigset_t _sd;
} posix_spawnattr_t;

#ifndef POSIX_SPAWN_RESETIDS
#define POSIX_SPAWN_RESETIDS 0x01
#endif
#ifndef POSIX_SPAWN_SETPGROUP
#define POSIX_SPAWN_SETPGROUP 0x02
#endif
#ifndef POSIX_SPAWN_SETSIGDEF
#define POSIX_SPAWN_SETSIGDEF 0x04
#endif
#ifndef POSIX_SPAWN_SETSIGMASK
#define POSIX_SPAWN_SETSIGMASK 0x08
#endif
#ifndef POSIX_SPAWN_SETSCHEDPARAM
#define POSIX_SPAWN_SETSCHEDPARAM 0x10
#endif
#ifndef POSIX_SPAWN_SETSCHEDULER
#define POSIX_SPAWN_SETSCHEDULER 0x20
#endif

int posix_spawn(pid_t *pid,
                const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[],
                char *const envp[]);
int posix_spawnp(pid_t *pid,
                 const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[],
                 char *const envp[]);

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions, int fd, const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions, int fd);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions, int fd, int newfd);

int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);
int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup);
int posix_spawnattr_setschedpolicy(posix_spawnattr_t *attr, int policy);
int posix_spawnattr_setschedparam(posix_spawnattr_t *attr, const struct sched_param *param);
int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sigdefault);
int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask);

#endif
