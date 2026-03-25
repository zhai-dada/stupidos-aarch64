#include "fs/vfs.h"
#include "errno.h"
#include "sched.h"
#include "lib/libmem.h"
#include "printk.h"

/*
 * 这是一个最小可用的 VFS 层。
 * 当前先做一个最小但可扩展的 VFS 层：
 * 1. 支持根文件系统和少量附加挂载点
 * 2. 按路径逐级 lookup
 * 3. 提供简单的 fd/read/write/lseek/close 抽象
 */
struct vfs_file
{
    bool used;
    int flags;
    uint64_t pos;
    struct vfs_inode inode;
};

struct vfs_mount
{
    bool used;
    size_t path_len;
    int8_t path[VFS_PATH_MAX];
    struct vfs_superblock sb;
    struct vfs_inode root;
};

static struct
{
    bool mounted;
    struct vfs_mount mounts[VFS_MAX_MOUNTS];
    struct vfs_file files[VFS_MAX_FILES];
} vfs_state;

static bool vfs_is_dir(struct vfs_inode *inode)
{
    return (inode->mode & VFS_S_IFMT) == VFS_S_IFDIR;
}

static bool vfs_is_reg(struct vfs_inode *inode)
{
    return (inode->mode & VFS_S_IFMT) == VFS_S_IFREG;
}

static uint8_t vfs_dtype_from_mode(uint16_t mode)
{
    switch (mode & VFS_S_IFMT)
    {
    case VFS_S_IFDIR:
        return 4; /* DT_DIR */
    case VFS_S_IFCHR:
        return 2; /* DT_CHR */
    case VFS_S_IFREG:
        return 8; /* DT_REG */
    default:
        return 0; /* DT_UNKNOWN */
    }
}

static bool vfs_is_valid_dirent(const struct vfs_dirent *dirent)
{
    return dirent && dirent->name[0] != '\0';
}

static void vfs_fill_stat_from_inode(const struct vfs_inode *inode, struct vfs_stat *out)
{
    if (!inode || !out)
    {
        return;
    }

    memset((int8_t *)out, 0, sizeof(*out));
    out->ino = inode->ino;
    out->mode = inode->mode;
    out->nlink = inode->nlink ? inode->nlink : 1;
    out->uid = inode->uid;
    out->gid = inode->gid;
    out->size = inode->size;
    out->blocks = inode->blocks ? inode->blocks : ((inode->size + 511ULL) >> 9);
    out->blksize = inode->blksize ? inode->blksize : 4096;
}

static int vfs_path_reset(int8_t *path, size_t out_len)
{
    if (!path || out_len < 2)
    {
        return -EINVAL;
    }

    path[0] = '/';
    path[1] = '\0';
    return 0;
}

static int vfs_path_pop(int8_t *path)
{
    size_t len;

    if (!path)
    {
        return -EINVAL;
    }

    len = strlen(path);
    if (len <= 1)
    {
        path[0] = '/';
        path[1] = '\0';
        return 0;
    }

    while (len > 1 && path[len - 1] == '/')
    {
        len--;
    }

    while (len > 1 && path[len - 1] != '/')
    {
        len--;
    }

    if (len == 0)
    {
        len = 1;
    }

    path[len] = '\0';
    if (path[0] != '/')
    {
        path[0] = '/';
        path[1] = '\0';
    }

    return 0;
}

static int vfs_path_push(int8_t *path, size_t out_len, const int8_t *name, size_t name_len)
{
    size_t path_len;

    if (!path || !name || name_len == 0)
    {
        return -EINVAL;
    }

    if (name_len == 1 && name[0] == '.')
    {
        return 0;
    }

    if (name_len == 2 && name[0] == '.' && name[1] == '.')
    {
        return vfs_path_pop(path);
    }

    path_len = strlen(path);
    if (path_len == 0)
    {
        if (vfs_path_reset(path, out_len))
        {
            return -EINVAL;
        }
        path_len = 1;
    }

    if (path_len > 1 && path[path_len - 1] != '/')
    {
        if (path_len + 1 >= out_len)
        {
            return -ENAMETOOLONG;
        }
        path[path_len++] = '/';
        path[path_len] = '\0';
    }

    if (path_len + name_len >= out_len)
    {
        return -ENAMETOOLONG;
    }

    memcpy(path + path_len, (int8_t *)name, name_len);
    path[path_len + name_len] = '\0';
    return 0;
}

static int vfs_path_apply(int8_t *path, size_t out_len, const int8_t *src)
{
    size_t i;

    if (!path || !src)
    {
        return -EINVAL;
    }

    i = 0;
    while (src[i] != '\0')
    {
        size_t start;
        size_t len;
        int ret;

        while (src[i] == '/')
        {
            i++;
        }

        if (src[i] == '\0')
        {
            break;
        }

        start = i;
        while (src[i] != '\0' && src[i] != '/')
        {
            i++;
        }
        len = i - start;

        ret = vfs_path_push(path, out_len, &src[start], len);
        if (ret)
        {
            return ret;
        }
    }

    return 0;
}

int vfs_canonicalize_path(const int8_t *path, int8_t *out, size_t out_len)
{
    const int8_t *cwd;
    int ret;

    if (!path || !out || out_len < 2)
    {
        return -EINVAL;
    }

    ret = vfs_path_reset(out, out_len);
    if (ret)
    {
        return ret;
    }

    if (path[0] != '/')
    {
        cwd = task_cwd();
        if (cwd && cwd[0] == '/')
        {
            ret = vfs_path_apply(out, out_len, cwd);
            if (ret)
            {
                return ret;
            }
        }
    }

    ret = vfs_path_apply(out, out_len, path);
    if (ret)
    {
        return ret;
    }

    return 0;
}

static bool vfs_mount_path_match(const struct vfs_mount *mnt, const int8_t *path)
{
    if (!mnt->used)
    {
        return false;
    }

    if (mnt->path_len == 1)
    {
        return path[0] == '/';
    }

    if (strncmp(mnt->path, path, mnt->path_len) != 0)
    {
        return false;
    }

    return path[mnt->path_len] == '\0' || path[mnt->path_len] == '/';
}

static int vfs_resolve_mount(const int8_t *path, struct vfs_mount **out_mount, const int8_t **out_subpath)
{
    struct vfs_mount *best;
    size_t best_len;
    int mount_id;

    best = 0;
    best_len = 0;

    for (mount_id = 0; mount_id < VFS_MAX_MOUNTS; mount_id++)
    {
        struct vfs_mount *mnt = &vfs_state.mounts[mount_id];

        if (!vfs_mount_path_match(mnt, path))
        {
            continue;
        }

        if (!best || mnt->path_len > best_len)
        {
            best = mnt;
            best_len = mnt->path_len;
        }
    }

    if (!best)
    {
        return -ENOENT;
    }

    *out_mount = best;
    if (best_len == 1)
    {
        *out_subpath = path;
    }
    else if (path[best_len] == '\0')
    {
        *out_subpath = "/";
    }
    else
    {
        *out_subpath = path + best_len;
    }

    return 0;
}

static int vfs_lookup_path_from(struct vfs_inode *root, const int8_t *path, struct vfs_inode *out)
{
    struct vfs_inode current;
    int8_t name[VFS_NAME_MAX + 1];
    int ret;
    size_t i;
    size_t n;

    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }

    current = *root;

    if (path[1] == '\0')
    {
        *out = current;
        return 0;
    }

    i = 1;
    while (path[i] != '\0')
    {
        while (path[i] == '/')
        {
            i++;
        }

        if (path[i] == '\0')
        {
            break;
        }

        if (!vfs_is_dir(&current) || !current.ops || !current.ops->lookup)
        {
            return -ENOTDIR;
        }

        n = 0;
        while (path[i] != '\0' && path[i] != '/')
        {
            if (n >= VFS_NAME_MAX)
            {
                return -ENAMETOOLONG;
            }

            name[n++] = path[i++];
        }
        name[n] = '\0';

        ret = current.ops->lookup(&current, name, &current);
        if (ret)
        {
            return ret;
        }
    }

    *out = current;
    return 0;
}

static int vfs_lookup_path(const int8_t *path, struct vfs_inode *out)
{
    int8_t resolved[VFS_PATH_MAX];
    struct vfs_mount *mnt;
    const int8_t *subpath;
    int ret;

    if (!vfs_state.mounted)
    {
        return -ENODEV;
    }

    if (!path)
    {
        return -EINVAL;
    }

    ret = vfs_canonicalize_path(path, resolved, sizeof(resolved));
    if (ret)
    {
        return ret;
    }

    ret = vfs_resolve_mount(resolved, &mnt, &subpath);
    if (ret)
    {
        return ret;
    }

    return vfs_lookup_path_from(&mnt->root, subpath, out);
}

static int vfs_split_parent_path(const int8_t *path,
                                 int8_t *parent_out, size_t parent_len,
                                 int8_t *name_out, size_t name_len)
{
    int8_t resolved[VFS_PATH_MAX];
    size_t len;
    size_t pos;
    size_t name_size;

    if (!path || !parent_out || !name_out || parent_len < 2 || name_len < 2)
    {
        return -EINVAL;
    }

    if (vfs_canonicalize_path(path, resolved, sizeof(resolved)))
    {
        return -EINVAL;
    }

    len = strlen((int8_t *)resolved);
    if (len <= 1)
    {
        return -EINVAL;
    }

    pos = len - 1;
    while (pos > 0 && resolved[pos] != '/')
    {
        pos--;
    }

    if (resolved[pos] != '/')
    {
        return -EINVAL;
    }

    name_size = len - pos - 1;
    if (name_size == 0 || name_size + 1 > name_len)
    {
        return -EINVAL;
    }

    if (pos == 0)
    {
        if (parent_len < 2)
        {
            return -ENAMETOOLONG;
        }
        parent_out[0] = '/';
        parent_out[1] = '\0';
    }
    else
    {
        if (pos + 1 > parent_len)
        {
            return -ENAMETOOLONG;
        }
        memcpy(parent_out, resolved, pos);
        parent_out[pos] = '\0';
    }

    memcpy(name_out, &resolved[pos + 1], name_size);
    name_out[name_size] = '\0';
    return 0;
}

int vfs_mount(const int8_t *path, struct vfs_superblock *sb, struct vfs_inode *root)
{
    int mount_id;
    size_t path_len;

    if (!path || !sb || !root || path[0] != '/')
    {
        return -EINVAL;
    }

    path_len = strlen((int8_t *)path);
    if (path_len == 0 || path_len >= VFS_PATH_MAX)
    {
        return -EINVAL;
    }

    for (mount_id = 0; mount_id < VFS_MAX_MOUNTS; mount_id++)
    {
        if (!vfs_state.mounts[mount_id].used)
        {
            break;
        }
    }

    if (mount_id == VFS_MAX_MOUNTS)
    {
        return -ENOSPC;
    }

    memset((int8_t *)&vfs_state.mounts[mount_id], 0, sizeof(vfs_state.mounts[mount_id]));
    vfs_state.mounts[mount_id].used = true;
    vfs_state.mounts[mount_id].path_len = path_len;
    memcpy(vfs_state.mounts[mount_id].path, (int8_t *)path, path_len + 1);
    vfs_state.mounts[mount_id].sb = *sb;
    vfs_state.mounts[mount_id].root = *root;
    vfs_state.mounted = true;

    return 0;
}

int vfs_mount_root(struct vfs_superblock *sb, struct vfs_inode *root)
{
    memset((int8_t *)&vfs_state, 0, sizeof(vfs_state));
    return vfs_mount((const int8_t *)"/", sb, root);
}

int vfs_chdir(const int8_t *path)
{
    int8_t resolved[VFS_PATH_MAX];
    struct vfs_inode inode;
    int ret;

    ret = vfs_canonicalize_path(path, resolved, sizeof(resolved));
    if (ret)
    {
        return ret;
    }

    ret = vfs_lookup_path(resolved, &inode);
    if (ret)
    {
        return ret;
    }

    if (!vfs_is_dir(&inode))
    {
        return -ENOTDIR;
    }

    return task_set_cwd(resolved);
}

int vfs_readdir(const int8_t *path, uint32_t index, struct vfs_dirent *out)
{
    struct vfs_inode inode;
    int ret;

    if (!out)
    {
        return -EINVAL;
    }

    memset((int8_t *)out, 0, sizeof(*out));

    ret = vfs_lookup_path(path, &inode);
    if (ret)
    {
        return ret;
    }

    if (!vfs_is_dir(&inode))
    {
        return -ENOTDIR;
    }

    if (!inode.ops || !inode.ops->readdir)
    {
        return -ENOSYS;
    }

    ret = inode.ops->readdir(&inode, index, out);
    if (!ret && !vfs_is_valid_dirent(out))
    {
        return -ENOENT;
    }

    return ret;
}

int vfs_open(const int8_t *path, int flags)
{
    struct vfs_inode parent;
    struct vfs_inode inode;
    int8_t parent_path[VFS_PATH_MAX];
    int8_t name[VFS_NAME_MAX + 1];
    int ret;
    int fd;

    ret = vfs_lookup_path(path, &inode);
    if (ret)
    {
        if (!(flags & VFS_O_CREAT) || ret != -ENOENT)
        {
            return ret;
        }

        ret = vfs_split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
        if (ret)
        {
            return ret;
        }

        ret = vfs_lookup_path(parent_path, &parent);
        if (ret)
        {
            return ret;
        }

        if (!vfs_is_dir(&parent))
        {
            return -ENOTDIR;
        }

        if (!parent.ops || !parent.ops->create)
        {
            return -EROFS;
        }

        ret = parent.ops->create(&parent, name, (uint16_t)(VFS_S_IFREG | 0644), &inode);
        if (ret)
        {
            return ret;
        }
    }

    if (!vfs_is_reg(&inode))
    {
        return -EISDIR;
    }

    if (flags & VFS_O_TRUNC)
    {
        if (!inode.ops || !inode.ops->truncate)
        {
            return -EROFS;
        }

        ret = inode.ops->truncate(&inode, 0);
        if (ret)
        {
            return ret;
        }
        inode.size = 0;
    }

    for (fd = 0; fd < VFS_MAX_FILES; fd++)
    {
        if (!vfs_state.files[fd].used)
        {
            vfs_state.files[fd].used = true;
            vfs_state.files[fd].flags = flags;
            vfs_state.files[fd].pos = (flags & VFS_O_APPEND) ? inode.size : 0;
            vfs_state.files[fd].inode = inode;
            return fd;
        }
    }

    return -EMFILE;
}

int vfs_stat(const int8_t *path, struct vfs_stat *out)
{
    struct vfs_inode inode;
    int ret;

    if (!out)
    {
        return -EINVAL;
    }

    memset((int8_t *)out, 0, sizeof(*out));
    ret = vfs_lookup_path(path, &inode);
    if (ret)
    {
        return ret;
    }

    vfs_fill_stat_from_inode(&inode, out);
    return 0;
}

int vfs_fstat(int fd, struct vfs_stat *out)
{
    if (!out)
    {
        return -EINVAL;
    }

    memset((int8_t *)out, 0, sizeof(*out));

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        if (fd >= 0 && fd < 3)
        {
            /*
             * 标准输入输出先按“字符设备”返回，方便 shell / Python 这类程序
             * 通过 isatty() 或 fstat() 识别交互终端。
            */
            out->ino = (uint32_t)fd;
            out->mode = VFS_S_IFCHR | 0600;
            out->nlink = 1;
            out->uid = 0;
            out->gid = 0;
            out->size = 0;
            out->blocks = 0;
            out->blksize = 4096;
            return 0;
        }
        return -EBADF;
    }

    vfs_fill_stat_from_inode(&vfs_state.files[fd].inode, out);
    return 0;
}

ssize_t vfs_read(int fd, void *buf, size_t len)
{
    struct vfs_file *file;
    ssize_t ret;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    file = &vfs_state.files[fd];
    if (!(file->flags & VFS_O_RDONLY))
    {
        return -EACCES;
    }

    if (!file->inode.ops || !file->inode.ops->read)
    {
        return -ENOSYS;
    }

    ret = file->inode.ops->read(&file->inode, file->pos, buf, len);
    if (ret > 0)
    {
        file->pos += ret;
    }

    return ret;
}

ssize_t vfs_write(int fd, const void *buf, size_t len)
{
    struct vfs_file *file;
    ssize_t ret;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    file = &vfs_state.files[fd];
    if (!(file->flags & VFS_O_WRONLY))
    {
        return -EACCES;
    }

    if (!file->inode.ops || !file->inode.ops->write)
    {
        return -ENOSYS;
    }

    if (file->flags & VFS_O_APPEND)
    {
        file->pos = file->inode.size;
    }

    ret = file->inode.ops->write(&file->inode, file->pos, buf, len);
    if (ret > 0)
    {
        file->pos += ret;
        if (file->pos > file->inode.size)
        {
            file->inode.size = file->pos;
        }
    }

    return ret;
}

ssize_t vfs_pread(int fd, void *buf, size_t len, uint64_t offset)
{
    struct vfs_file *file;
    uint64_t old_pos;
    ssize_t ret;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    if (fd < 3)
    {
        return vfs_read(fd, buf, len);
    }

    file = &vfs_state.files[fd];
    old_pos = file->pos;
    file->pos = offset;
    ret = vfs_read(fd, buf, len);
    file->pos = old_pos;
    return ret;
}

ssize_t vfs_pwrite(int fd, const void *buf, size_t len, uint64_t offset)
{
    struct vfs_file *file;
    uint64_t old_pos;
    ssize_t ret;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    if (fd < 3)
    {
        return vfs_write(fd, buf, len);
    }

    file = &vfs_state.files[fd];
    old_pos = file->pos;
    file->pos = offset;
    ret = vfs_write(fd, buf, len);
    file->pos = old_pos;
    return ret;
}

int64_t vfs_lseek(int fd, int64_t offset, int whence)
{
    struct vfs_file *file;
    int64_t pos;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    file = &vfs_state.files[fd];

    if (whence == VFS_SEEK_SET)
    {
        pos = offset;
    }
    else if (whence == VFS_SEEK_CUR)
    {
        pos = (int64_t)file->pos + offset;
    }
    else if (whence == VFS_SEEK_END)
    {
        pos = (int64_t)file->inode.size + offset;
    }
    else
    {
        return -EINVAL;
    }

    if (pos < 0)
    {
        return -EINVAL;
    }

    file->pos = (uint64_t)pos;
    return pos;
}

int64_t vfs_file_size(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    return (int64_t)vfs_state.files[fd].inode.size;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    memset((int8_t *)&vfs_state.files[fd], 0, sizeof(vfs_state.files[fd]));
    return 0;
}

int vfs_dup(int fd)
{
    int newfd;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    for (newfd = 0; newfd < VFS_MAX_FILES; newfd++)
    {
        if (!vfs_state.files[newfd].used)
        {
            return vfs_dup2(fd, newfd);
        }
    }

    return -EMFILE;
}

int vfs_dup2(int oldfd, int newfd)
{
    struct vfs_file dup_file;

    /*
     * 最小 dup2：
     * - 先复用现有全局 fd 表
     * - 复制打开状态、偏移和 inode 元数据
     * - 不做引用计数，先满足 shell / 后续用户态程序对“复制描述符”的基本需求
     *
     * 这一步还不等价于完整 Linux 语义，但足够作为 Python / shell 的起点。
     */
    if (oldfd < 0 || oldfd >= VFS_MAX_FILES || !vfs_state.files[oldfd].used)
    {
        return -EBADF;
    }

    if (newfd < 0 || newfd >= VFS_MAX_FILES)
    {
        return -EBADF;
    }

    if (oldfd == newfd)
    {
        return newfd;
    }

    dup_file = vfs_state.files[oldfd];
    if (vfs_state.files[newfd].used)
    {
        memset((int8_t *)&vfs_state.files[newfd], 0, sizeof(vfs_state.files[newfd]));
    }

    vfs_state.files[newfd] = dup_file;
    vfs_state.files[newfd].used = true;
    return newfd;
}

int vfs_fcntl(int fd, int cmd, uint64_t arg)
{
    struct vfs_file *file;
    int newfd;

    if (fd < 0 || fd >= VFS_MAX_FILES)
    {
        return -EBADF;
    }

    if (!vfs_state.files[fd].used)
    {
        if (fd < 3 && (cmd == VFS_F_GETFD || cmd == VFS_F_GETFL))
        {
            return 0;
        }
        return -EBADF;
    }

    file = &vfs_state.files[fd];
    switch (cmd)
    {
    case VFS_F_GETFD:
        return 0;
    case VFS_F_SETFD:
        return 0;
    case VFS_F_GETFL:
        return file->flags;
    case VFS_F_SETFL:
        file->flags = (file->flags & (VFS_O_RDONLY | VFS_O_WRONLY)) | (int)arg;
        return 0;
    case VFS_F_DUPFD:
    case VFS_F_DUPFD_CLOEXEC:
        newfd = (int)arg;
        if (newfd < 0 || newfd >= VFS_MAX_FILES)
        {
            return -EINVAL;
        }
        return vfs_dup2(fd, newfd);
    default:
        return -ENOTTY;
    }
}

int vfs_getdents64(int fd, void *buf, size_t len)
{
    struct vfs_file *file;
    uint8_t *out;
    size_t used;
    uint32_t index;

    if (!buf || len == 0)
    {
        return -EINVAL;
    }

    if (fd < 0 || fd >= VFS_MAX_FILES)
    {
        return -EBADF;
    }

    file = &vfs_state.files[fd];
    if (!file->used)
    {
        return -EBADF;
    }

    if (!vfs_is_dir(&file->inode))
    {
        return -ENOTDIR;
    }

    if (!file->inode.ops || !file->inode.ops->readdir)
    {
        return -ENOTSUP;
    }

    out = (uint8_t *)buf;
    used = 0;
    index = (uint32_t)file->pos;
    while (used < len)
    {
        struct vfs_dirent ent;
        struct vfs_linux_dirent64 *dst;
        size_t name_len;
        size_t reclen;
        int ret;

        ret = file->inode.ops->readdir(&file->inode, index, &ent);
        if (ret < 0)
        {
            if (ret == -ENOENT)
            {
                break;
            }
            return used ? (int)used : ret;
        }

        name_len = strlen(ent.name);
        /*
         * Linux dirent64 兼容布局：
         *   ino(8) + off(8) + reclen(2) + type(1) + name + '\0'
         * 再按 8 字节对齐，便于用户态按标准步进解析。
         */
        reclen = sizeof(uint64_t) + sizeof(int64_t) + sizeof(uint16_t) + sizeof(uint8_t) + name_len + 1U;
        reclen = (reclen + 7U) & ~7U;
        if (used + reclen > len)
        {
            break;
        }

        dst = (struct vfs_linux_dirent64 *)(void *)(out + used);
        memset((int8_t *)dst, 0, reclen);
        dst->d_ino = ent.ino;
        dst->d_off = (int64_t)(index + 1U);
        dst->d_reclen = (uint16_t)reclen;
        dst->d_type = vfs_dtype_from_mode(ent.mode);
        memcpy(dst->d_name, ent.name, name_len);
        dst->d_name[name_len] = '\0';

        used += reclen;
        index++;
    }

    file->pos = index;
    return (int)used;
}

int vfs_mkdir(const int8_t *path, uint16_t mode)
{
    struct vfs_inode parent;
    struct vfs_inode created;
    struct vfs_inode existing;
    int8_t parent_path[VFS_PATH_MAX];
    int8_t name[VFS_NAME_MAX + 1];
    int ret;

    ret = vfs_lookup_path(path, &existing);
    if (ret == 0)
    {
        return -EEXIST;
    }
    if (ret != -ENOENT)
    {
        return ret;
    }

    ret = vfs_split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret)
    {
        return ret;
    }

    ret = vfs_lookup_path(parent_path, &parent);
    if (ret)
    {
        return ret;
    }

    if (!vfs_is_dir(&parent))
    {
        return -ENOTDIR;
    }

    if (!parent.ops || !parent.ops->mkdir)
    {
        return -EROFS;
    }

    ret = parent.ops->mkdir(&parent, name, mode, &created);
    return ret;
}

int vfs_unlink(const int8_t *path, bool dir_only)
{
    struct vfs_inode parent;
    int8_t parent_path[VFS_PATH_MAX];
    int8_t name[VFS_NAME_MAX + 1];
    int ret;

    ret = vfs_split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret)
    {
        return ret;
    }

    ret = vfs_lookup_path(parent_path, &parent);
    if (ret)
    {
        return ret;
    }

    if (!vfs_is_dir(&parent))
    {
        return -ENOTDIR;
    }

    if (!parent.ops || !parent.ops->unlink)
    {
        return -EROFS;
    }

    return parent.ops->unlink(&parent, name, dir_only);
}

int vfs_rename(const int8_t *old_path, const int8_t *new_path)
{
    struct vfs_inode old_parent;
    struct vfs_inode new_parent;
    int8_t old_parent_path[VFS_PATH_MAX];
    int8_t new_parent_path[VFS_PATH_MAX];
    int8_t old_name[VFS_NAME_MAX + 1];
    int8_t new_name[VFS_NAME_MAX + 1];
    int ret;

    ret = vfs_split_parent_path(old_path, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name));
    if (ret)
    {
        return ret;
    }

    ret = vfs_split_parent_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name));
    if (ret)
    {
        return ret;
    }

    ret = vfs_lookup_path(old_parent_path, &old_parent);
    if (ret)
    {
        return ret;
    }

    ret = vfs_lookup_path(new_parent_path, &new_parent);
    if (ret)
    {
        return ret;
    }

    if (old_parent.sb != new_parent.sb)
    {
        return -EXDEV;
    }

    if (!old_parent.ops || !old_parent.ops->rename)
    {
        return -EROFS;
    }

    return old_parent.ops->rename(&old_parent, old_name, &new_parent, new_name);
}

int vfs_truncate(const int8_t *path, uint64_t size)
{
    struct vfs_inode inode;
    int ret;

    ret = vfs_lookup_path(path, &inode);
    if (ret)
    {
        return ret;
    }

    if (!vfs_is_reg(&inode))
    {
        return -EISDIR;
    }

    if (!inode.ops || !inode.ops->truncate)
    {
        return -EROFS;
    }

    return inode.ops->truncate(&inode, size);
}

int vfs_ftruncate(int fd, uint64_t size)
{
    struct vfs_file *file;
    int ret;

    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    file = &vfs_state.files[fd];
    if (!file->inode.ops || !file->inode.ops->truncate)
    {
        return -EROFS;
    }

    ret = file->inode.ops->truncate(&file->inode, size);
    if (ret)
    {
        return ret;
    }

    file->inode.size = size;
    if (file->pos > size)
    {
        file->pos = size;
    }

    return 0;
}

int vfs_utimens(const int8_t *path, const struct vfs_timespec *atime, const struct vfs_timespec *mtime)
{
    struct vfs_inode inode;
    int ret;

    ret = vfs_lookup_path(path, &inode);
    if (ret)
    {
        return ret;
    }

    if (!inode.ops || !inode.ops->utimens)
    {
        return -EROFS;
    }

    return inode.ops->utimens(&inode, atime, mtime);
}
