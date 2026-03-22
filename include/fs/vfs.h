#ifndef __VFS_H__
#define __VFS_H__

#include "asm/types.h"

#define VFS_PATH_MAX        256
#define VFS_NAME_MAX        255
#define VFS_MAX_FILES       16
#define VFS_MAX_MOUNTS      4

#define VFS_O_RDONLY        0x1
#define VFS_O_WRONLY        0x2
#define VFS_O_RDWR          (VFS_O_RDONLY | VFS_O_WRONLY)

#define VFS_SEEK_SET        0
#define VFS_SEEK_CUR        1
#define VFS_SEEK_END        2

#define VFS_S_IFMT          0xF000
#define VFS_S_IFREG         0x8000
#define VFS_S_IFDIR         0x4000
#define VFS_S_IFCHR         0x2000

#define VFS_S_IRUSR         0x0100
#define VFS_S_IWUSR         0x0080
#define VFS_S_IXUSR         0x0040
#define VFS_S_IRGRP         0x0020
#define VFS_S_IWGRP         0x0010
#define VFS_S_IXGRP         0x0008
#define VFS_S_IROTH         0x0004
#define VFS_S_IWOTH         0x0002
#define VFS_S_IXOTH         0x0001

#define VFS_F_GETFD         1
#define VFS_F_SETFD         2
#define VFS_F_GETFL         3
#define VFS_F_SETFL         4
#define VFS_F_DUPFD         0
#define VFS_F_DUPFD_CLOEXEC 1030

struct vfs_inode;
struct vfs_superblock;
struct vfs_dirent;

struct vfs_inode_ops
{
    int (*lookup)(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out);
    int (*readdir)(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out);
    ssize_t (*read)(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len);
    ssize_t (*write)(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len);
};

struct vfs_dirent
{
    uint32_t ino;
    uint16_t mode;
    uint64_t size;
    int8_t name[VFS_NAME_MAX + 1];
};

struct vfs_stat
{
    uint32_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blocks;
    uint32_t blksize;
    uint32_t reserved;
};

struct vfs_inode
{
    struct vfs_superblock *sb;
    const struct vfs_inode_ops *ops;
    uint32_t ino;
    uint16_t mode;
    uint64_t size;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t blksize;
    uint64_t blocks;
    void *fs_private;
    uint64_t private_data[4];
};

struct vfs_superblock
{
    const int8_t *name;
    void *fs_private;
};

int vfs_mount(const int8_t *path, struct vfs_superblock *sb, struct vfs_inode *root);
int vfs_mount_root(struct vfs_superblock *sb, struct vfs_inode *root);
int vfs_canonicalize_path(const int8_t *path, int8_t *out, size_t out_len);
int vfs_chdir(const int8_t *path);
int vfs_open(const int8_t *path, int flags);
int vfs_readdir(const int8_t *path, uint32_t index, struct vfs_dirent *out);
int vfs_stat(const int8_t *path, struct vfs_stat *out);
int vfs_fstat(int fd, struct vfs_stat *out);
ssize_t vfs_read(int fd, void *buf, size_t len);
ssize_t vfs_write(int fd, const void *buf, size_t len);
ssize_t vfs_pread(int fd, void *buf, size_t len, uint64_t offset);
ssize_t vfs_pwrite(int fd, const void *buf, size_t len, uint64_t offset);
int64_t vfs_lseek(int fd, int64_t offset, int whence);
int64_t vfs_file_size(int fd);
int vfs_close(int fd);
int vfs_dup(int fd);
int vfs_dup2(int oldfd, int newfd);
int vfs_fcntl(int fd, int cmd, uint64_t arg);

#endif
