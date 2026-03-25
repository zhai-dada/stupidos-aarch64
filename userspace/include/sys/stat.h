#ifndef __STUPIDOS_SYS_STAT_H__
#define __STUPIDOS_SYS_STAT_H__

#include_next <sys/stat.h>
#include "stupidos_user.h"
#include <time.h>

/* 这里保留 stupidos 既有的文件类型/权限掩码，兼容内核返回值。 */
#undef S_IFMT
#undef S_IFREG
#undef S_IFDIR
#undef S_IFCHR
#define S_IFMT   STUPIDOS_VFS_S_IFMT
#define S_IFREG  STUPIDOS_VFS_S_IFREG
#define S_IFDIR  STUPIDOS_VFS_S_IFDIR
#define S_IFCHR  STUPIDOS_VFS_S_IFCHR

#undef S_IRUSR
#undef S_IWUSR
#undef S_IXUSR
#undef S_IRGRP
#undef S_IWGRP
#undef S_IXGRP
#undef S_IROTH
#undef S_IWOTH
#undef S_IXOTH
#define S_IRUSR  STUPIDOS_VFS_S_IRUSR
#define S_IWUSR  STUPIDOS_VFS_S_IWUSR
#define S_IXUSR  STUPIDOS_VFS_S_IXUSR
#define S_IRGRP  STUPIDOS_VFS_S_IRGRP
#define S_IWGRP  STUPIDOS_VFS_S_IWGRP
#define S_IXGRP  STUPIDOS_VFS_S_IXGRP
#define S_IROTH  STUPIDOS_VFS_S_IROTH
#define S_IWOTH  STUPIDOS_VFS_S_IWOTH
#define S_IXOTH  STUPIDOS_VFS_S_IXOTH

/*
 * 某些交叉工具链头在 freestanding 场景下不会暴露完整原型。
 * 这里补齐我们在 compat.c 已实现、且用户态程序会直接使用的接口声明，
 * 避免出现 implicit declaration 警告。
 */
int mkdir(const char *path, mode_t mode);
int mkdirat(int dirfd, const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
int fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);

#endif
