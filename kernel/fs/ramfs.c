#include "fs/ramfs.h"

#include "errno.h"
#include "fs/vfs.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mm/page_alloc.h"
#include "mmu.h"
#include "timer.h"

#define RAMFS_MAX_NODES 256

struct ramfs_node
{
    bool used;
    uint32_t ino;
    uint16_t mode;
    uint16_t name_len;
    int8_t name[VFS_NAME_MAX + 1];
    struct ramfs_node *parent;
    struct ramfs_node *child;
    struct ramfs_node *sibling;

    void *data;
    uint32_t data_order;
    uint64_t size;

    uint64_t atime_sec;
    uint64_t atime_nsec;
    uint64_t mtime_sec;
    uint64_t mtime_nsec;
};

struct ramfs_state
{
    bool mounted;
    uint32_t next_ino;
    struct vfs_superblock sb;
    struct ramfs_node nodes[RAMFS_MAX_NODES];
    struct ramfs_node *root;
};

static struct ramfs_state ramfs_state;

static int ramfs_lookup(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out);
static int ramfs_readdir(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out);
static ssize_t ramfs_read(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len);
static ssize_t ramfs_write(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len);
static int ramfs_create(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out);
static int ramfs_mkdir(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out);
static int ramfs_unlink(struct vfs_inode *dir, const int8_t *name, bool dir_only);
static int ramfs_rename(struct vfs_inode *old_dir, const int8_t *old_name,
                        struct vfs_inode *new_dir, const int8_t *new_name);
static int ramfs_truncate(struct vfs_inode *inode, uint64_t size);
static int ramfs_utimens(struct vfs_inode *inode, const struct vfs_timespec *atime, const struct vfs_timespec *mtime);

static struct vfs_inode_ops ramfs_inode_ops =
{
    .lookup = ramfs_lookup,
    .readdir = ramfs_readdir,
    .read = ramfs_read,
    .write = ramfs_write,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rename = ramfs_rename,
    .truncate = ramfs_truncate,
    .utimens = ramfs_utimens,
};

static void ramfs_touch_now(struct ramfs_node *node, bool access, bool modify)
{
    uint64_t sec;
    uint64_t nsec;

    if (!node)
    {
        return;
    }

    sec = (uint64_t)(jiffies / STUPIDOS_TIMER_HZ);
    nsec = ((uint64_t)(jiffies % STUPIDOS_TIMER_HZ) * 1000000000ULL) / STUPIDOS_TIMER_HZ;
    if (access)
    {
        node->atime_sec = sec;
        node->atime_nsec = nsec;
    }
    if (modify)
    {
        node->mtime_sec = sec;
        node->mtime_nsec = nsec;
    }
}

static struct ramfs_node *ramfs_node_from_inode(const struct vfs_inode *inode)
{
    struct ramfs_node *node;

    if (!inode)
    {
        return 0;
    }

    node = (struct ramfs_node *)inode->fs_private;
    if (!node || !node->used)
    {
        return 0;
    }

    return node;
}

static void ramfs_fill_inode(struct ramfs_node *node, struct vfs_inode *out)
{
    if (!node || !out)
    {
        return;
    }

    memset((int8_t *)out, 0, sizeof(*out));
    out->sb = &ramfs_state.sb;
    out->ops = &ramfs_inode_ops;
    out->ino = node->ino;
    out->mode = node->mode;
    out->size = node->size;
    out->nlink = 1;
    out->uid = 0;
    out->gid = 0;
    out->blksize = PAGE_SIZE;
    out->blocks = (node->size + 511ULL) >> 9;
    out->fs_private = node;
}

static struct ramfs_node *ramfs_node_alloc(struct ramfs_node *parent, const int8_t *name, uint16_t mode)
{
    uint32_t i;
    struct ramfs_node *node;
    size_t name_len;

    if (!name || name[0] == '\0')
    {
        return 0;
    }

    name_len = strlen((int8_t *)name);
    if (name_len > VFS_NAME_MAX)
    {
        return 0;
    }

    for (i = 0; i < RAMFS_MAX_NODES; i++)
    {
        if (!ramfs_state.nodes[i].used)
        {
            break;
        }
    }

    if (i == RAMFS_MAX_NODES)
    {
        return 0;
    }

    node = &ramfs_state.nodes[i];
    memset((int8_t *)node, 0, sizeof(*node));
    node->used = true;
    node->ino = ramfs_state.next_ino++;
    node->mode = mode;
    node->name_len = (uint16_t)name_len;
    memcpy(node->name, (int8_t *)name, name_len + 1);
    node->parent = parent;
    node->data_order = 0;
    node->data = 0;
    node->size = 0;
    ramfs_touch_now(node, true, true);

    if (parent)
    {
        node->sibling = parent->child;
        parent->child = node;
        ramfs_touch_now(parent, false, true);
    }

    return node;
}

static void ramfs_node_detach(struct ramfs_node *node)
{
    struct ramfs_node *prev;
    struct ramfs_node *iter;

    if (!node || !node->parent)
    {
        return;
    }

    prev = 0;
    iter = node->parent->child;
    while (iter)
    {
        if (iter == node)
        {
            if (prev)
            {
                prev->sibling = iter->sibling;
            }
            else
            {
                node->parent->child = iter->sibling;
            }
            node->sibling = 0;
            ramfs_touch_now(node->parent, false, true);
            return;
        }
        prev = iter;
        iter = iter->sibling;
    }
}

static void ramfs_node_free(struct ramfs_node *node)
{
    if (!node)
    {
        return;
    }

    if (node->data)
    {
        free_pages(node->data, node->data_order);
    }

    memset((int8_t *)node, 0, sizeof(*node));
}

static struct ramfs_node *ramfs_child_lookup(struct ramfs_node *dir, const int8_t *name)
{
    struct ramfs_node *iter;
    size_t name_len;

    if (!dir || !name)
    {
        return 0;
    }

    name_len = strlen((int8_t *)name);
    for (iter = dir->child; iter; iter = iter->sibling)
    {
        if (!iter->used || iter->name_len != name_len)
        {
            continue;
        }

        if (strcmp(iter->name, name) == 0)
        {
            return iter;
        }
    }

    return 0;
}

static int ramfs_resize_file(struct ramfs_node *node, uint64_t new_size)
{
    uint64_t cap;
    uint64_t target_cap;
    uint32_t order;
    void *new_buf;

    if (!node || (node->mode & VFS_S_IFMT) != VFS_S_IFREG)
    {
        return -EINVAL;
    }

    if (new_size == 0)
    {
        node->size = 0;
        return 0;
    }

    cap = node->data ? ((uint64_t)PAGE_SIZE << node->data_order) : 0;
    if (new_size <= cap)
    {
        node->size = new_size;
        return 0;
    }

    target_cap = PAGE_SIZE;
    order = 0;
    while (target_cap < new_size && order + 1 < PAGE_ALLOC_MAX_ORDER)
    {
        target_cap <<= 1;
        order++;
    }

    if (target_cap < new_size)
    {
        return -EFBIG;
    }

    new_buf = alloc_pages(order);
    if (!new_buf)
    {
        return -ENOMEM;
    }

    memset((int8_t *)new_buf, 0, (size_t)target_cap);
    if (node->data && node->size)
    {
        memcpy((int8_t *)new_buf, (int8_t *)node->data, (size_t)node->size);
        free_pages(node->data, node->data_order);
    }

    node->data = new_buf;
    node->data_order = order;
    node->size = new_size;
    return 0;
}

static int ramfs_lookup(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out)
{
    struct ramfs_node *dir_node;
    struct ramfs_node *child;

    if (!dir || !name || !out)
    {
        return -EINVAL;
    }

    dir_node = ramfs_node_from_inode(dir);
    if (!dir_node)
    {
        return -ENOENT;
    }

    if ((dir_node->mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    child = ramfs_child_lookup(dir_node, name);
    if (!child)
    {
        return -ENOENT;
    }

    ramfs_fill_inode(child, out);
    return 0;
}

static int ramfs_readdir(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct ramfs_node *dir_node;
    struct ramfs_node *iter;
    uint32_t curr;

    if (!dir || !out)
    {
        return -EINVAL;
    }

    dir_node = ramfs_node_from_inode(dir);
    if (!dir_node)
    {
        return -ENOENT;
    }

    if ((dir_node->mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    curr = 0;
    for (iter = dir_node->child; iter; iter = iter->sibling)
    {
        if (curr == index)
        {
            memset((int8_t *)out, 0, sizeof(*out));
            out->ino = iter->ino;
            out->mode = iter->mode;
            out->size = iter->size;
            memcpy(out->name, iter->name, iter->name_len + 1);
            return 0;
        }
        curr++;
    }

    return -ENOENT;
}

static ssize_t ramfs_read(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len)
{
    struct ramfs_node *node;
    uint64_t avail;
    size_t nread;

    if (!inode || !buf)
    {
        return -EINVAL;
    }

    node = ramfs_node_from_inode(inode);
    if (!node)
    {
        return -ENOENT;
    }

    if ((node->mode & VFS_S_IFMT) != VFS_S_IFREG)
    {
        return -EISDIR;
    }

    if (offset >= node->size)
    {
        return 0;
    }

    avail = node->size - offset;
    nread = (avail < len) ? (size_t)avail : len;
    if (nread && node->data)
    {
        memcpy((int8_t *)buf, (int8_t *)((uint8_t *)node->data + offset), nread);
    }

    ramfs_touch_now(node, true, false);
    inode->size = node->size;
    return (ssize_t)nread;
}

static ssize_t ramfs_write(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len)
{
    struct ramfs_node *node;
    uint64_t end;
    int ret;

    if (!inode || (!buf && len))
    {
        return -EINVAL;
    }

    node = ramfs_node_from_inode(inode);
    if (!node)
    {
        return -ENOENT;
    }

    if ((node->mode & VFS_S_IFMT) != VFS_S_IFREG)
    {
        return -EISDIR;
    }

    end = offset + len;
    if (end < offset)
    {
        return -EOVERFLOW;
    }

    ret = ramfs_resize_file(node, (end > node->size) ? end : node->size);
    if (ret)
    {
        return ret;
    }

    if (len)
    {
        memcpy((int8_t *)((uint8_t *)node->data + offset), (int8_t *)buf, len);
    }

    if (end > node->size)
    {
        node->size = end;
    }
    ramfs_touch_now(node, true, true);
    inode->size = node->size;
    return (ssize_t)len;
}

static int ramfs_create(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out)
{
    struct ramfs_node *dir_node;
    struct ramfs_node *node;

    dir_node = ramfs_node_from_inode(dir);
    if (!dir_node)
    {
        return -ENOENT;
    }

    if ((dir_node->mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    if (ramfs_child_lookup(dir_node, name))
    {
        return -EEXIST;
    }

    node = ramfs_node_alloc(dir_node, name, (uint16_t)(VFS_S_IFREG | (mode & 0777)));
    if (!node)
    {
        return -ENOSPC;
    }

    if (out)
    {
        ramfs_fill_inode(node, out);
    }

    return 0;
}

static int ramfs_mkdir(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out)
{
    struct ramfs_node *dir_node;
    struct ramfs_node *node;

    dir_node = ramfs_node_from_inode(dir);
    if (!dir_node)
    {
        return -ENOENT;
    }

    if ((dir_node->mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    if (ramfs_child_lookup(dir_node, name))
    {
        return -EEXIST;
    }

    node = ramfs_node_alloc(dir_node, name, (uint16_t)(VFS_S_IFDIR | (mode & 0777)));
    if (!node)
    {
        return -ENOSPC;
    }

    if (out)
    {
        ramfs_fill_inode(node, out);
    }

    return 0;
}

static int ramfs_unlink(struct vfs_inode *dir, const int8_t *name, bool dir_only)
{
    struct ramfs_node *dir_node;
    struct ramfs_node *node;
    uint16_t type;

    dir_node = ramfs_node_from_inode(dir);
    if (!dir_node)
    {
        return -ENOENT;
    }

    if ((dir_node->mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    node = ramfs_child_lookup(dir_node, name);
    if (!node)
    {
        return -ENOENT;
    }

    type = node->mode & VFS_S_IFMT;
    if (dir_only)
    {
        if (type != VFS_S_IFDIR)
        {
            return -ENOTDIR;
        }
        if (node->child)
        {
            return -ENOTEMPTY;
        }
    }
    else
    {
        if (type == VFS_S_IFDIR)
        {
            return -EISDIR;
        }
    }

    ramfs_node_detach(node);
    ramfs_node_free(node);
    return 0;
}

static int ramfs_rename(struct vfs_inode *old_dir, const int8_t *old_name,
                        struct vfs_inode *new_dir, const int8_t *new_name)
{
    struct ramfs_node *old_parent;
    struct ramfs_node *new_parent;
    struct ramfs_node *node;
    struct ramfs_node *existing;
    size_t new_len;

    old_parent = ramfs_node_from_inode(old_dir);
    new_parent = ramfs_node_from_inode(new_dir);
    if (!old_parent || !new_parent)
    {
        return -ENOENT;
    }

    if ((old_parent->mode & VFS_S_IFMT) != VFS_S_IFDIR ||
        (new_parent->mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    node = ramfs_child_lookup(old_parent, old_name);
    if (!node)
    {
        return -ENOENT;
    }

    new_len = strlen((int8_t *)new_name);
    if (new_len == 0 || new_len > VFS_NAME_MAX)
    {
        return -EINVAL;
    }

    existing = ramfs_child_lookup(new_parent, new_name);
    if (existing && existing != node)
    {
        if ((existing->mode & VFS_S_IFMT) == VFS_S_IFDIR && existing->child)
        {
            return -ENOTEMPTY;
        }

        ramfs_node_detach(existing);
        ramfs_node_free(existing);
    }

    ramfs_node_detach(node);
    node->parent = new_parent;
    node->sibling = new_parent->child;
    new_parent->child = node;
    node->name_len = (uint16_t)new_len;
    memcpy(node->name, (int8_t *)new_name, new_len + 1);
    ramfs_touch_now(node, false, true);
    ramfs_touch_now(new_parent, false, true);
    return 0;
}

static int ramfs_truncate(struct vfs_inode *inode, uint64_t size)
{
    struct ramfs_node *node;
    int ret;

    node = ramfs_node_from_inode(inode);
    if (!node)
    {
        return -ENOENT;
    }

    if ((node->mode & VFS_S_IFMT) != VFS_S_IFREG)
    {
        return -EISDIR;
    }

    ret = ramfs_resize_file(node, size);
    if (ret)
    {
        return ret;
    }

    node->size = size;
    ramfs_touch_now(node, false, true);
    inode->size = node->size;
    return 0;
}

static int ramfs_utimens(struct vfs_inode *inode, const struct vfs_timespec *atime, const struct vfs_timespec *mtime)
{
    struct ramfs_node *node;

    node = ramfs_node_from_inode(inode);
    if (!node)
    {
        return -ENOENT;
    }

    if (atime)
    {
        node->atime_sec = (uint64_t)atime->tv_sec;
        node->atime_nsec = (uint64_t)atime->tv_nsec;
    }
    if (mtime)
    {
        node->mtime_sec = (uint64_t)mtime->tv_sec;
        node->mtime_nsec = (uint64_t)mtime->tv_nsec;
    }

    if (!atime && !mtime)
    {
        ramfs_touch_now(node, true, true);
    }
    return 0;
}

int ramfs_mount(const int8_t *path)
{
    struct vfs_inode root_inode;
    struct ramfs_node *root;

    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }

    if (!ramfs_state.mounted)
    {
        memset((int8_t *)&ramfs_state, 0, sizeof(ramfs_state));
        ramfs_state.next_ino = 2;
        ramfs_state.sb.name = "ramfs";
        ramfs_state.sb.fs_private = &ramfs_state;

        root = &ramfs_state.nodes[0];
        memset((int8_t *)root, 0, sizeof(*root));
        root->used = true;
        root->ino = 1;
        root->mode = VFS_S_IFDIR | 0777;
        root->name[0] = '/';
        root->name[1] = '\0';
        ramfs_touch_now(root, true, true);
        ramfs_state.root = root;
        ramfs_state.mounted = true;
    }

    ramfs_fill_inode(ramfs_state.root, &root_inode);
    return vfs_mount(path, &ramfs_state.sb, &root_inode);
}
