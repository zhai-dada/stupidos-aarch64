#include "fs/vfs.h"
#include "errno.h"
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

static bool vfs_is_valid_dirent(const struct vfs_dirent *dirent)
{
    return dirent && dirent->name[0] != '\0';
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
    struct vfs_mount *mnt;
    const int8_t *subpath;
    int ret;

    if (!vfs_state.mounted)
    {
        return -ENODEV;
    }

    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }

    ret = vfs_resolve_mount(path, &mnt, &subpath);
    if (ret)
    {
        return ret;
    }

    return vfs_lookup_path_from(&mnt->root, subpath, out);
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
    struct vfs_inode inode;
    int ret;
    int fd;

    ret = vfs_lookup_path(path, &inode);
    if (ret)
    {
        return ret;
    }

    if (!vfs_is_reg(&inode))
    {
        return -EISDIR;
    }

    for (fd = 0; fd < VFS_MAX_FILES; fd++)
    {
        if (!vfs_state.files[fd].used)
        {
            vfs_state.files[fd].used = true;
            vfs_state.files[fd].flags = flags;
            vfs_state.files[fd].pos = 0;
            vfs_state.files[fd].inode = inode;
            return fd;
        }
    }

    return -EMFILE;
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

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FILES || !vfs_state.files[fd].used)
    {
        return -EBADF;
    }

    memset((int8_t *)&vfs_state.files[fd], 0, sizeof(vfs_state.files[fd]));
    return 0;
}
