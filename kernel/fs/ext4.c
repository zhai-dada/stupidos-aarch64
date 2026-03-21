#include "fs/ext4.h"
#include "fs/vfs.h"
#include "driver/virtio_blk.h"
#include "errno.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "printk.h"

/*
 * 这是一个面向当前内核启动环境的简化 ext4 驱动：
 * 1. 从 GPT 第 2 分区挂载 ext4
 * 2. 支持 extent 树查块
 * 3. 支持目录 lookup、普通文件 read/write
 *
 * 当前明确不支持：
 * - block/inode 分配
 * - truncate / create / unlink
 * - journal 回放与提交
 */
#define GPT_SIGNATURE               "EFI PART"
#define GPT_ENTRY_SIZE_MIN          128
#define EXT4_SUPER_MAGIC            0xEF53
#define EXT4_ROOT_INO               2
#define EXT4_NDIR_BLOCKS            15
#define EXT4_EXTENTS_FL             0x00080000
#define EXT4_EXT_MAGIC              0xF30A

#define EXT4_FT_UNKNOWN             0
#define EXT4_FT_REG_FILE            1
#define EXT4_FT_DIR                 2

struct gpt_header
{
    uint8_t signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry
{
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} __attribute__((packed));

struct ext4_super_block
{
    uint32_t inodes_count;
    uint32_t blocks_count_lo;
    uint32_t r_blocks_count_lo;
    uint32_t free_blocks_count_lo;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    uint8_t volume_name[16];
    uint8_t last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks;
    uint8_t prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum;
    uint32_t journal_dev;
    uint32_t last_orphan;
    uint32_t hash_seed[4];
    uint8_t def_hash_version;
    uint8_t jnl_backup_type;
    uint16_t desc_size;
    uint32_t default_mount_opts;
    uint32_t first_meta_bg;
    uint32_t mkfs_time;
    uint32_t jnl_blocks[17];
    uint32_t blocks_count_hi;
    uint32_t r_blocks_count_hi;
    uint32_t free_blocks_count_hi;
    uint16_t min_extra_isize;
    uint16_t want_extra_isize;
    uint32_t flags;
    uint16_t raid_stride;
    uint16_t mmp_interval;
    uint64_t mmp_block;
    uint32_t raid_stripe_width;
    uint8_t log_groups_per_flex;
    uint8_t checksum_type;
    uint16_t reserved_pad;
    uint64_t kbytes_written;
    uint32_t snapshot_inum;
    uint32_t snapshot_id;
    uint64_t snapshot_r_blocks_count;
    uint32_t snapshot_list;
    uint32_t error_count;
    uint32_t first_error_time;
    uint32_t first_error_ino;
    uint64_t first_error_block;
    uint8_t first_error_func[32];
    uint32_t first_error_line;
    uint32_t last_error_time;
    uint32_t last_error_ino;
    uint32_t last_error_line;
    uint64_t last_error_block;
    uint8_t last_error_func[32];
    uint8_t mount_opts[64];
    uint32_t usr_quota_inum;
    uint32_t grp_quota_inum;
    uint32_t overhead_clusters;
    uint32_t backup_bgs[2];
    uint8_t encrypt_algos[4];
    uint8_t encrypt_pw_salt[16];
    uint32_t lpf_ino;
    uint32_t prj_quota_inum;
    uint32_t checksum_seed;
} __attribute__((packed));

struct ext4_group_desc
{
    uint32_t block_bitmap_lo;
    uint32_t inode_bitmap_lo;
    uint32_t inode_table_lo;
    uint16_t free_blocks_count_lo;
    uint16_t free_inodes_count_lo;
    uint16_t used_dirs_count_lo;
    uint16_t flags;
    uint32_t exclude_bitmap_lo;
    uint16_t block_bitmap_csum_lo;
    uint16_t inode_bitmap_csum_lo;
    uint16_t itable_unused_lo;
    uint16_t checksum;
    uint32_t block_bitmap_hi;
    uint32_t inode_bitmap_hi;
    uint32_t inode_table_hi;
    uint16_t free_blocks_count_hi;
    uint16_t free_inodes_count_hi;
    uint16_t used_dirs_count_hi;
    uint16_t itable_unused_hi;
    uint32_t exclude_bitmap_hi;
    uint16_t block_bitmap_csum_hi;
    uint16_t inode_bitmap_csum_hi;
    uint32_t reserved;
} __attribute__((packed));

struct ext4_inode_disk
{
    uint16_t mode;
    uint16_t uid;
    uint32_t size_lo;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks_lo;
    uint32_t flags;
    uint32_t osd1;
    uint8_t block[60];
    uint32_t generation;
    uint32_t file_acl_lo;
    uint32_t size_high;
    uint32_t obso_faddr;
    uint8_t osd2[12];
} __attribute__((packed));

struct ext4_extent_header
{
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;
    uint32_t generation;
} __attribute__((packed));

struct ext4_extent
{
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} __attribute__((packed));

struct ext4_extent_idx
{
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed));

struct ext4_dir_entry_2
{
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type;
    char name[];
} __attribute__((packed));

struct ext4_fs
{
    struct vfs_superblock sb;
    uint64_t part_lba_start;
    uint64_t part_lba_count;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t group_desc_size;
    uint32_t gdt_block;
};

static struct ext4_fs ext4_root_fs;
static uint8_t ext4_sector_buf[VIRTIO_BLK_SECTOR_SIZE];
static uint8_t ext4_block_buf_a[4096];
static uint8_t ext4_block_buf_b[4096];

static int ext4_lookup(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out);
static int ext4_readdir(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out);
static ssize_t ext4_read(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len);
static ssize_t ext4_write(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len);

static const struct vfs_inode_ops ext4_inode_ops = {
    .lookup = ext4_lookup,
    .readdir = ext4_readdir,
    .read = ext4_read,
    .write = ext4_write,
};

static uint64_t ext4_inode_size(const struct ext4_inode_disk *inode)
{
    return ((uint64_t)inode->size_high << 32) | inode->size_lo;
}

static void ext4_set_inode_size(struct ext4_inode_disk *inode, uint64_t size)
{
    inode->size_lo = (uint32_t)size;
    inode->size_high = (uint32_t)(size >> 32);
}

static uint64_t ext4_extent_start(const struct ext4_extent *extent)
{
    return ((uint64_t)extent->ee_start_hi << 32) | extent->ee_start_lo;
}

static uint64_t ext4_extent_leaf(const struct ext4_extent_idx *idx)
{
    return ((uint64_t)idx->ei_leaf_hi << 32) | idx->ei_leaf_lo;
}

static int ext4_read_sector(uint64_t lba, void *buf)
{
    return virtio_blk_read(lba, buf, 1);
}

static int ext4_read_block(const struct ext4_fs *fs, uint64_t block, void *buf)
{
    uint64_t lba;
    uint32_t sectors;

    sectors = fs->block_size / VIRTIO_BLK_SECTOR_SIZE;
    lba = fs->part_lba_start + (block * sectors);
    return virtio_blk_read(lba, buf, sectors);
}

static int ext4_write_block(const struct ext4_fs *fs, uint64_t block, const void *buf)
{
    uint64_t lba;
    uint32_t sectors;

    sectors = fs->block_size / VIRTIO_BLK_SECTOR_SIZE;
    lba = fs->part_lba_start + (block * sectors);
    return virtio_blk_write(lba, buf, sectors);
}

static int ext4_read_gpt_entry(uint32_t index, struct gpt_entry *entry)
{
    struct gpt_header *header;
    uint64_t entry_lba;
    uint32_t offset;

    if (ext4_read_sector(1, ext4_sector_buf))
    {
        return -EIO;
    }

    header = (struct gpt_header *)ext4_sector_buf;
    if (memcmp(header->signature, GPT_SIGNATURE, 8))
    {
        return -EINVAL;
    }

    if (header->size_of_partition_entry < GPT_ENTRY_SIZE_MIN || index >= header->num_partition_entries)
    {
        return -EINVAL;
    }

    entry_lba = header->partition_entry_lba + ((uint64_t)index * header->size_of_partition_entry) / VIRTIO_BLK_SECTOR_SIZE;
    offset = ((uint32_t)index * header->size_of_partition_entry) % VIRTIO_BLK_SECTOR_SIZE;

    if (ext4_read_sector(entry_lba, ext4_sector_buf))
    {
        return -EIO;
    }

    memcpy((int8_t *)entry, (int8_t *)(ext4_sector_buf + offset), sizeof(*entry));
    return 0;
}

static int ext4_read_group_desc(const struct ext4_fs *fs, uint32_t group, struct ext4_group_desc *desc)
{
    uint64_t byte_offset;
    uint64_t block;
    uint32_t offset;

    byte_offset = (uint64_t)group * fs->group_desc_size;
    block = fs->gdt_block + (byte_offset / fs->block_size);
    offset = byte_offset % fs->block_size;

    if (ext4_read_block(fs, block, ext4_block_buf_a))
    {
        return -EIO;
    }

    memcpy((int8_t *)desc, (int8_t *)(ext4_block_buf_a + offset), sizeof(*desc));
    return 0;
}

static int ext4_read_inode_raw(struct ext4_fs *fs, uint32_t ino, struct ext4_inode_disk *inode)
{
    struct ext4_group_desc desc;
    uint32_t group;
    uint32_t index;
    uint64_t table_block;
    uint64_t block;
    uint32_t offset;

    if (ino == 0)
    {
        return -EINVAL;
    }

    group = (ino - 1) / fs->inodes_per_group;
    index = (ino - 1) % fs->inodes_per_group;

    if (ext4_read_group_desc(fs, group, &desc))
    {
        return -EIO;
    }

    table_block = desc.inode_table_lo;
    block = table_block + (((uint64_t)index * fs->inode_size) / fs->block_size);
    offset = ((uint64_t)index * fs->inode_size) % fs->block_size;

    if (ext4_read_block(fs, block, ext4_block_buf_a))
    {
        return -EIO;
    }

    memcpy((int8_t *)inode, (int8_t *)(ext4_block_buf_a + offset), sizeof(*inode));
    return 0;
}

static int ext4_write_inode_raw(struct ext4_fs *fs, uint32_t ino, const struct ext4_inode_disk *inode)
{
    struct ext4_group_desc desc;
    uint32_t group;
    uint32_t index;
    uint64_t table_block;
    uint64_t block;
    uint32_t offset;

    group = (ino - 1) / fs->inodes_per_group;
    index = (ino - 1) % fs->inodes_per_group;

    if (ext4_read_group_desc(fs, group, &desc))
    {
        return -EIO;
    }

    table_block = desc.inode_table_lo;
    block = table_block + (((uint64_t)index * fs->inode_size) / fs->block_size);
    offset = ((uint64_t)index * fs->inode_size) % fs->block_size;

    if (ext4_read_block(fs, block, ext4_block_buf_a))
    {
        return -EIO;
    }

    memcpy((int8_t *)(ext4_block_buf_a + offset), (int8_t *)inode, sizeof(*inode));
    return ext4_write_block(fs, block, ext4_block_buf_a);
}

static int ext4_map_extent_block(const struct ext4_fs *fs, const struct ext4_inode_disk *inode,
                                 uint32_t logical, uint64_t *phys)
{
    struct ext4_extent_header *eh;
    struct ext4_extent_idx *idx;
    struct ext4_extent *ext;
    uint32_t i;
    uint32_t depth;
    uint8_t *node;

    if ((inode->flags & EXT4_EXTENTS_FL) == 0)
    {
        return -ENOTSUP;
    }

    eh = (struct ext4_extent_header *)inode->block;
    if (eh->magic != EXT4_EXT_MAGIC)
    {
        return -EIO;
    }

    node = (uint8_t *)inode->block;
    depth = eh->depth;

    while (depth)
    {
        eh = (struct ext4_extent_header *)node;
        idx = (struct ext4_extent_idx *)(node + sizeof(*eh));

        for (i = 0; i < eh->entries; i++)
        {
            if (i + 1 == eh->entries || logical < idx[i + 1].ei_block)
            {
                break;
            }
        }

        if (ext4_read_block(fs, ext4_extent_leaf(&idx[i]), ext4_block_buf_b))
        {
            return -EIO;
        }

        node = ext4_block_buf_b;
        eh = (struct ext4_extent_header *)node;
        if (eh->magic != EXT4_EXT_MAGIC)
        {
            return -EIO;
        }
        depth = eh->depth;
    }

    eh = (struct ext4_extent_header *)node;
    ext = (struct ext4_extent *)(node + sizeof(*eh));
    for (i = 0; i < eh->entries; i++)
    {
        uint32_t len = ext[i].ee_len & 0x7fff;
        if (logical >= ext[i].ee_block && logical < ext[i].ee_block + len)
        {
            *phys = ext4_extent_start(&ext[i]) + (logical - ext[i].ee_block);
            return 0;
        }
    }

    return -ENOENT;
}

static int ext4_fill_vfs_inode(struct ext4_fs *fs, uint32_t ino, struct vfs_inode *out)
{
    struct ext4_inode_disk raw_inode;
    int ret;

    ret = ext4_read_inode_raw(fs, ino, &raw_inode);
    if (ret)
    {
        return ret;
    }

    out->sb = &fs->sb;
    out->ops = &ext4_inode_ops;
    out->ino = ino;
    out->mode = raw_inode.mode;
    out->size = ext4_inode_size(&raw_inode);
    out->fs_private = fs;
    return 0;
}

static int ext4_lookup(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk dir_inode;
    uint64_t size;
    uint32_t logical;
    uint32_t block_count;
    size_t name_len;

    fs = (struct ext4_fs *)dir->fs_private;
    if (ext4_read_inode_raw(fs, dir->ino, &dir_inode))
    {
        return -EIO;
    }

    size = ext4_inode_size(&dir_inode);
    block_count = (size + fs->block_size - 1) / fs->block_size;
    name_len = strlen((int8_t *)name);

    for (logical = 0; logical < block_count; logical++)
    {
        uint64_t phys_block;
        uint32_t off;

        if (ext4_map_extent_block(fs, &dir_inode, logical, &phys_block))
        {
            continue;
        }

        if (ext4_read_block(fs, phys_block, ext4_block_buf_a))
        {
            return -EIO;
        }

        off = 0;
        while (off < fs->block_size)
        {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + off);

            if (de->rec_len == 0)
            {
                break;
            }

            if (de->inode && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0)
            {
                return ext4_fill_vfs_inode(fs, de->inode, out);
            }

            off += de->rec_len;
        }
    }

    return -ENOENT;
}

static bool ext4_dirent_is_skip(const struct ext4_dir_entry_2 *de)
{
    if (!de->inode || de->rec_len == 0 || de->name_len == 0)
    {
        return true;
    }

    if (de->name_len == 1 && de->name[0] == '.')
    {
        return true;
    }

    if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')
    {
        return true;
    }

    return false;
}

static int ext4_copy_dirent_name(struct vfs_dirent *out, const struct ext4_dir_entry_2 *de)
{
    size_t i;

    if (de->name_len > VFS_NAME_MAX)
    {
        return -ENAMETOOLONG;
    }

    for (i = 0; i < de->name_len; i++)
    {
        out->name[i] = (int8_t)de->name[i];
    }
    out->name[de->name_len] = '\0';
    return 0;
}

static int ext4_readdir(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk dir_inode;
    uint64_t size;
    uint32_t logical;
    uint32_t block_count;
    uint32_t seen;

    fs = (struct ext4_fs *)dir->fs_private;
    if (ext4_read_inode_raw(fs, dir->ino, &dir_inode))
    {
        return -EIO;
    }

    size = ext4_inode_size(&dir_inode);
    block_count = (size + fs->block_size - 1) / fs->block_size;
    seen = 0;

    for (logical = 0; logical < block_count; logical++)
    {
        uint64_t phys_block;
        uint32_t off;

        if (ext4_map_extent_block(fs, &dir_inode, logical, &phys_block))
        {
            continue;
        }

        if (ext4_read_block(fs, phys_block, ext4_block_buf_a))
        {
            return -EIO;
        }

        off = 0;
        while (off < fs->block_size)
        {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + off);
            struct vfs_inode child;
            int ret;

            if (de->rec_len == 0)
            {
                break;
            }

            if (ext4_dirent_is_skip(de))
            {
                off += de->rec_len;
                continue;
            }

            if (seen == index)
            {
                ret = ext4_fill_vfs_inode(fs, de->inode, &child);
                if (ret)
                {
                    return ret;
                }

                out->ino = child.ino;
                out->mode = child.mode;
                out->size = child.size;
                ret = ext4_copy_dirent_name(out, de);
                if (ret)
                {
                    return ret;
                }
                return 0;
            }

            seen++;
            off += de->rec_len;
        }
    }

    return -ENOENT;
}

static ssize_t ext4_read(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk raw_inode;
    uint8_t *dst;
    uint64_t end;
    size_t done;

    fs = (struct ext4_fs *)inode->fs_private;
    if (ext4_read_inode_raw(fs, inode->ino, &raw_inode))
    {
        return -EIO;
    }

    if (offset >= inode->size)
    {
        return 0;
    }

    end = offset + len;
    if (end > inode->size)
    {
        end = inode->size;
    }

    dst = (uint8_t *)buf;
    done = 0;

    while (offset < end)
    {
        uint32_t logical = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;
        uint32_t chunk = fs->block_size - block_off;
        uint64_t phys_block;

        if (chunk > end - offset)
        {
            chunk = end - offset;
        }

        if (ext4_map_extent_block(fs, &raw_inode, logical, &phys_block))
        {
            return done ? (ssize_t)done : -EIO;
        }

        if (ext4_read_block(fs, phys_block, ext4_block_buf_a))
        {
            return done ? (ssize_t)done : -EIO;
        }

        memcpy((int8_t *)(dst + done), (int8_t *)(ext4_block_buf_a + block_off), chunk);
        offset += chunk;
        done += chunk;
    }

    return done;
}

static ssize_t ext4_write(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk raw_inode;
    const uint8_t *src;
    size_t done;
    uint64_t end;

    fs = (struct ext4_fs *)inode->fs_private;
    if (ext4_read_inode_raw(fs, inode->ino, &raw_inode))
    {
        return -EIO;
    }

    if (offset > inode->size)
    {
        return -EINVAL;
    }

    src = (const uint8_t *)buf;
    done = 0;
    end = offset + len;

    while (done < len)
    {
        uint32_t logical = offset / fs->block_size;
        uint32_t block_off = offset % fs->block_size;
        uint32_t chunk = fs->block_size - block_off;
        uint64_t phys_block;

        if (chunk > len - done)
        {
            chunk = len - done;
        }

        if (ext4_map_extent_block(fs, &raw_inode, logical, &phys_block))
        {
            return done ? (ssize_t)done : -ENOSPC;
        }

        if (block_off || chunk != fs->block_size)
        {
            if (ext4_read_block(fs, phys_block, ext4_block_buf_a))
            {
                return done ? (ssize_t)done : -EIO;
            }
        }
        else
        {
            memset((int8_t *)ext4_block_buf_a, 0, fs->block_size);
        }

        memcpy((int8_t *)(ext4_block_buf_a + block_off), (int8_t *)(src + done), chunk);
        if (ext4_write_block(fs, phys_block, ext4_block_buf_a))
        {
            return done ? (ssize_t)done : -EIO;
        }

        offset += chunk;
        done += chunk;
    }

    if (end > inode->size)
    {
        ext4_set_inode_size(&raw_inode, end);
        if (ext4_write_inode_raw(fs, inode->ino, &raw_inode))
        {
            return done ? (ssize_t)done : -EIO;
        }
        inode->size = end;
    }

    return done;
}

int ext4_mount_root(void)
{
    struct gpt_entry entry;
    struct ext4_super_block *sb;
    struct vfs_inode root_inode;
    int ret;

    ret = virtio_blk_init();
    if (ret)
    {
        return ret;
    }
    printk("[ext4\tinit]: reading GPT entry 2\n");

    ret = ext4_read_gpt_entry(1, &entry);
    if (ret)
    {
        printk("[ext4\tinit]: failed to read GPT entry 2\n");
        return ret;
    }

    ext4_root_fs.part_lba_start = entry.first_lba;
    ext4_root_fs.part_lba_count = entry.last_lba - entry.first_lba + 1;
    printk("[ext4\tinit]: reading superblock\n");

    ret = virtio_blk_read(ext4_root_fs.part_lba_start + 2, ext4_block_buf_a, 2);
    if (ret)
    {
        printk("[ext4\tinit]: superblock read failed\n");
        return ret;
    }

    sb = (struct ext4_super_block *)ext4_block_buf_a;
    if (sb->magic != EXT4_SUPER_MAGIC)
    {
        printk("[ext4\tinit]: invalid superblock magic %#x\n", sb->magic);
        return -EINVAL;
    }

    ext4_root_fs.block_size = 1024U << sb->log_block_size;
    if (ext4_root_fs.block_size > sizeof(ext4_block_buf_a))
    {
        printk("[ext4\tinit]: unsupported block size %u\n", ext4_root_fs.block_size);
        return -ENOTSUP;
    }

    ext4_root_fs.inode_size = sb->inode_size;
    ext4_root_fs.inodes_per_group = sb->inodes_per_group;
    ext4_root_fs.blocks_per_group = sb->blocks_per_group;
    ext4_root_fs.group_desc_size = sb->desc_size ? sb->desc_size : 32;
    ext4_root_fs.gdt_block = (ext4_root_fs.block_size == 1024) ? 2 : 1;

    ext4_root_fs.sb.name = "ext4";
    ext4_root_fs.sb.fs_private = &ext4_root_fs;

    ret = ext4_fill_vfs_inode(&ext4_root_fs, EXT4_ROOT_INO, &root_inode);
    if (ret)
    {
        return ret;
    }

    ret = vfs_mount_root(&ext4_root_fs.sb, &root_inode);
    if (ret)
    {
        return ret;
    }

    printk("[ext4\tinit]: mounted ext4 at GPT partition 2, block_size=%u\n", ext4_root_fs.block_size);
    return 0;
}
