#include "fs/ext4.h"
#include "fs/vfs.h"
#include "driver/virtio_blk.h"
#include "errno.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mmu.h"
#include "printk.h"

/*
 * 这是一个面向当前内核启动环境的简化 ext4 驱动：
 * 1. 从 GPT 第 2 分区挂载 ext4
 * 2. 支持 extent 树查块
 * 3. 支持目录 lookup/readdir、普通文件 read/write
 * 4. 支持最小 create/mkdir（不含 journal）
 *
 * 当前明确不支持：
 * - 完整 ext4 元数据校验和更新（metadata_csum）
 * - unlink / rename / 多级 extent 树修改
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
#define EXT4_DIR_REC_LEN(name_len)  (((uint16_t)(8 + (name_len)) + 3U) & ~3U)

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
static int ext4_create(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out);
static int ext4_mkdir(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out);
static int ext4_unlink(struct vfs_inode *dir, const int8_t *name, bool dir_only);
static int ext4_rename(struct vfs_inode *old_dir, const int8_t *old_name,
                       struct vfs_inode *new_dir, const int8_t *new_name);
static int ext4_truncate(struct vfs_inode *inode, uint64_t size);
static uint64_t ext4_desc_inode_table(const struct ext4_group_desc *desc);

static struct vfs_inode_ops ext4_inode_ops;
static bool ext4_inode_ops_ready;

static void ext4_init_inode_ops(void)
{
    if (ext4_inode_ops_ready)
    {
        return;
    }

    /*
     * 这里不能直接依赖静态初始化的函数指针。
     * 内核切到 KIMAGE_VADDR 后，必须把回调修正到高地址映射。
     */
    ext4_inode_ops.lookup = ext4_lookup;
    ext4_inode_ops.readdir = ext4_readdir;
    ext4_inode_ops.read = ext4_read;
    ext4_inode_ops.write = ext4_write;
    ext4_inode_ops.create = ext4_create;
    ext4_inode_ops.mkdir = ext4_mkdir;
    ext4_inode_ops.unlink = ext4_unlink;
    ext4_inode_ops.rename = ext4_rename;
    ext4_inode_ops.truncate = ext4_truncate;
    ext4_inode_ops_ready = true;
}

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

    table_block = ext4_desc_inode_table(&desc);
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

    table_block = ext4_desc_inode_table(&desc);
    block = table_block + (((uint64_t)index * fs->inode_size) / fs->block_size);
    offset = ((uint64_t)index * fs->inode_size) % fs->block_size;

    if (ext4_read_block(fs, block, ext4_block_buf_a))
    {
        return -EIO;
    }

    memcpy((int8_t *)(ext4_block_buf_a + offset), (int8_t *)inode, sizeof(*inode));
    return ext4_write_block(fs, block, ext4_block_buf_a);
}

static uint64_t ext4_blocks_count(const struct ext4_super_block *sb)
{
    return ((uint64_t)sb->blocks_count_hi << 32) | sb->blocks_count_lo;
}

static uint64_t ext4_free_blocks_count(const struct ext4_super_block *sb)
{
    return ((uint64_t)sb->free_blocks_count_hi << 32) | sb->free_blocks_count_lo;
}

static void ext4_set_free_blocks_count(struct ext4_super_block *sb, uint64_t value)
{
    sb->free_blocks_count_lo = (uint32_t)value;
    sb->free_blocks_count_hi = (uint32_t)(value >> 32);
}

static uint64_t ext4_desc_block_bitmap(const struct ext4_group_desc *desc)
{
    return ((uint64_t)desc->block_bitmap_hi << 32) | desc->block_bitmap_lo;
}

static uint64_t ext4_desc_inode_bitmap(const struct ext4_group_desc *desc)
{
    return ((uint64_t)desc->inode_bitmap_hi << 32) | desc->inode_bitmap_lo;
}

static uint64_t ext4_desc_inode_table(const struct ext4_group_desc *desc)
{
    return ((uint64_t)desc->inode_table_hi << 32) | desc->inode_table_lo;
}

static uint32_t ext4_desc_free_blocks(const struct ext4_group_desc *desc)
{
    return ((uint32_t)desc->free_blocks_count_hi << 16) | desc->free_blocks_count_lo;
}

static uint32_t ext4_desc_free_inodes(const struct ext4_group_desc *desc)
{
    return ((uint32_t)desc->free_inodes_count_hi << 16) | desc->free_inodes_count_lo;
}

static void ext4_desc_set_free_blocks(struct ext4_group_desc *desc, uint32_t value)
{
    desc->free_blocks_count_lo = (uint16_t)(value & 0xffffU);
    desc->free_blocks_count_hi = (uint16_t)(value >> 16);
}

static void ext4_desc_set_free_inodes(struct ext4_group_desc *desc, uint32_t value)
{
    desc->free_inodes_count_lo = (uint16_t)(value & 0xffffU);
    desc->free_inodes_count_hi = (uint16_t)(value >> 16);
}

static int ext4_write_group_desc(const struct ext4_fs *fs, uint32_t group, const struct ext4_group_desc *desc)
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
    memcpy((int8_t *)(ext4_block_buf_a + offset), (int8_t *)desc, sizeof(*desc));
    if (ext4_write_block(fs, block, ext4_block_buf_a))
    {
        return -EIO;
    }
    return 0;
}

static int ext4_read_superblock_raw(struct ext4_fs *fs, struct ext4_super_block *out)
{
    if (virtio_blk_read(fs->part_lba_start + 2, ext4_block_buf_a, 2))
    {
        return -EIO;
    }
    memcpy((int8_t *)out, (int8_t *)ext4_block_buf_a, sizeof(*out));
    return 0;
}

static int ext4_write_superblock_raw(struct ext4_fs *fs, const struct ext4_super_block *sb)
{
    if (virtio_blk_read(fs->part_lba_start + 2, ext4_block_buf_a, 2))
    {
        return -EIO;
    }
    memcpy((int8_t *)ext4_block_buf_a, (int8_t *)sb, sizeof(*sb));
    if (virtio_blk_write(fs->part_lba_start + 2, ext4_block_buf_a, 2))
    {
        return -EIO;
    }
    return 0;
}

static int ext4_alloc_inode(struct ext4_fs *fs, uint32_t *out_ino)
{
    struct ext4_super_block sb;
    uint64_t blocks_count;
    uint32_t group_count;
    uint32_t group;
    uint32_t first_ino;

    if (!out_ino)
    {
        return -EINVAL;
    }

    if (ext4_read_superblock_raw(fs, &sb))
    {
        return -EIO;
    }

    blocks_count = ext4_blocks_count(&sb);
    if (blocks_count == 0)
    {
        return -EIO;
    }
    group_count = (uint32_t)((blocks_count + fs->blocks_per_group - 1U) / fs->blocks_per_group);
    first_ino = sb.first_ino ? sb.first_ino : 11U;

    for (group = 0; group < group_count; group++)
    {
        struct ext4_group_desc desc;
        uint64_t bitmap_block;
        uint32_t free_inodes;
        uint32_t i;

        if (ext4_read_group_desc(fs, group, &desc))
        {
            return -EIO;
        }

        free_inodes = ext4_desc_free_inodes(&desc);
        if (free_inodes == 0)
        {
            continue;
        }

        bitmap_block = ext4_desc_inode_bitmap(&desc);
        if (!bitmap_block || ext4_read_block(fs, bitmap_block, ext4_block_buf_a))
        {
            return -EIO;
        }

        for (i = 0; i < fs->inodes_per_group; i++)
        {
            uint32_t ino;
            uint32_t byte_index;
            uint8_t bit;

            ino = group * fs->inodes_per_group + i + 1U;
            if (ino < first_ino)
            {
                continue;
            }
            if (ino > sb.inodes_count)
            {
                break;
            }

            byte_index = i >> 3;
            bit = (uint8_t)(1U << (i & 7U));
            if ((ext4_block_buf_a[byte_index] & bit) != 0)
            {
                continue;
            }

            ext4_block_buf_a[byte_index] |= bit;
            if (ext4_write_block(fs, bitmap_block, ext4_block_buf_a))
            {
                return -EIO;
            }

            ext4_desc_set_free_inodes(&desc, free_inodes - 1U);
            if (ext4_write_group_desc(fs, group, &desc))
            {
                return -EIO;
            }

            if (sb.free_inodes_count > 0)
            {
                sb.free_inodes_count--;
                if (ext4_write_superblock_raw(fs, &sb))
                {
                    return -EIO;
                }
            }

            *out_ino = ino;
            return 0;
        }
    }

    return -ENOSPC;
}

static int ext4_free_inode(struct ext4_fs *fs, uint32_t ino)
{
    struct ext4_super_block sb;
    struct ext4_group_desc desc;
    uint32_t group;
    uint32_t index;
    uint64_t bitmap_block;
    uint32_t byte_index;
    uint8_t bit;

    if (ino == 0)
    {
        return -EINVAL;
    }

    if (ext4_read_superblock_raw(fs, &sb))
    {
        return -EIO;
    }

    group = (ino - 1U) / fs->inodes_per_group;
    index = (ino - 1U) % fs->inodes_per_group;
    if (ext4_read_group_desc(fs, group, &desc))
    {
        return -EIO;
    }

    bitmap_block = ext4_desc_inode_bitmap(&desc);
    if (!bitmap_block || ext4_read_block(fs, bitmap_block, ext4_block_buf_a))
    {
        return -EIO;
    }

    byte_index = index >> 3;
    bit = (uint8_t)(1U << (index & 7U));
    if ((ext4_block_buf_a[byte_index] & bit) == 0)
    {
        return 0;
    }

    ext4_block_buf_a[byte_index] &= (uint8_t)~bit;
    if (ext4_write_block(fs, bitmap_block, ext4_block_buf_a))
    {
        return -EIO;
    }

    ext4_desc_set_free_inodes(&desc, ext4_desc_free_inodes(&desc) + 1U);
    if (ext4_write_group_desc(fs, group, &desc))
    {
        return -EIO;
    }

    sb.free_inodes_count++;
    if (ext4_write_superblock_raw(fs, &sb))
    {
        return -EIO;
    }

    return 0;
}

static int ext4_alloc_block(struct ext4_fs *fs, uint64_t *out_block)
{
    struct ext4_super_block sb;
    uint64_t blocks_count;
    uint32_t group_count;
    uint32_t group;
    uint64_t first_data_block;

    if (!out_block)
    {
        return -EINVAL;
    }

    if (ext4_read_superblock_raw(fs, &sb))
    {
        return -EIO;
    }

    blocks_count = ext4_blocks_count(&sb);
    if (blocks_count == 0)
    {
        return -EIO;
    }
    first_data_block = sb.first_data_block;
    group_count = (uint32_t)((blocks_count + fs->blocks_per_group - 1U) / fs->blocks_per_group);

    for (group = 0; group < group_count; group++)
    {
        struct ext4_group_desc desc;
        uint64_t bitmap_block;
        uint64_t group_first_block;
        uint32_t free_blocks;
        uint32_t i;

        if (ext4_read_group_desc(fs, group, &desc))
        {
            return -EIO;
        }

        free_blocks = ext4_desc_free_blocks(&desc);
        if (free_blocks == 0)
        {
            continue;
        }

        bitmap_block = ext4_desc_block_bitmap(&desc);
        if (!bitmap_block || ext4_read_block(fs, bitmap_block, ext4_block_buf_a))
        {
            return -EIO;
        }

        group_first_block = first_data_block + ((uint64_t)group * fs->blocks_per_group);
        for (i = 0; i < fs->blocks_per_group; i++)
        {
            uint64_t block;
            uint32_t byte_index;
            uint8_t bit;

            block = group_first_block + i;
            if (block >= blocks_count)
            {
                break;
            }

            byte_index = i >> 3;
            bit = (uint8_t)(1U << (i & 7U));
            if ((ext4_block_buf_a[byte_index] & bit) != 0)
            {
                continue;
            }

            ext4_block_buf_a[byte_index] |= bit;
            if (ext4_write_block(fs, bitmap_block, ext4_block_buf_a))
            {
                return -EIO;
            }

            ext4_desc_set_free_blocks(&desc, free_blocks - 1U);
            if (ext4_write_group_desc(fs, group, &desc))
            {
                return -EIO;
            }

            if (ext4_free_blocks_count(&sb) > 0)
            {
                ext4_set_free_blocks_count(&sb, ext4_free_blocks_count(&sb) - 1U);
                if (ext4_write_superblock_raw(fs, &sb))
                {
                    return -EIO;
                }
            }

            *out_block = block;
            return 0;
        }
    }

    return -ENOSPC;
}

static int ext4_free_block(struct ext4_fs *fs, uint64_t block)
{
    struct ext4_super_block sb;
    struct ext4_group_desc desc;
    uint64_t blocks_count;
    uint64_t first_data_block;
    uint32_t group;
    uint32_t index;
    uint64_t bitmap_block;
    uint32_t byte_index;
    uint8_t bit;

    if (ext4_read_superblock_raw(fs, &sb))
    {
        return -EIO;
    }

    blocks_count = ext4_blocks_count(&sb);
    first_data_block = sb.first_data_block;
    if (block < first_data_block || block >= blocks_count)
    {
        return -EINVAL;
    }

    group = (uint32_t)((block - first_data_block) / fs->blocks_per_group);
    index = (uint32_t)((block - first_data_block) % fs->blocks_per_group);

    if (ext4_read_group_desc(fs, group, &desc))
    {
        return -EIO;
    }
    bitmap_block = ext4_desc_block_bitmap(&desc);
    if (!bitmap_block || ext4_read_block(fs, bitmap_block, ext4_block_buf_a))
    {
        return -EIO;
    }

    byte_index = index >> 3;
    bit = (uint8_t)(1U << (index & 7U));
    if ((ext4_block_buf_a[byte_index] & bit) == 0)
    {
        return 0;
    }

    ext4_block_buf_a[byte_index] &= (uint8_t)~bit;
    if (ext4_write_block(fs, bitmap_block, ext4_block_buf_a))
    {
        return -EIO;
    }

    ext4_desc_set_free_blocks(&desc, ext4_desc_free_blocks(&desc) + 1U);
    if (ext4_write_group_desc(fs, group, &desc))
    {
        return -EIO;
    }

    ext4_set_free_blocks_count(&sb, ext4_free_blocks_count(&sb) + 1U);
    if (ext4_write_superblock_raw(fs, &sb))
    {
        return -EIO;
    }

    return 0;
}

static int ext4_inode_add_extent_block(const struct ext4_fs *fs, struct ext4_inode_disk *inode,
                                       uint32_t logical, uint64_t phys_block)
{
    struct ext4_extent_header *eh;
    struct ext4_extent *ext;
    uint16_t i;
    uint16_t insert_pos;

    if (!fs || !inode)
    {
        return -EINVAL;
    }

    inode->flags |= EXT4_EXTENTS_FL;
    eh = (struct ext4_extent_header *)inode->block;
    if (eh->magic != EXT4_EXT_MAGIC)
    {
        memset((int8_t *)inode->block, 0, sizeof(inode->block));
        eh = (struct ext4_extent_header *)inode->block;
        eh->magic = EXT4_EXT_MAGIC;
        eh->entries = 0;
        eh->depth = 0;
        eh->max = (uint16_t)((sizeof(inode->block) - sizeof(*eh)) / sizeof(struct ext4_extent));
        eh->generation = 0;
    }

    if (eh->depth != 0)
    {
        return -ENOTSUP;
    }

    ext = (struct ext4_extent *)((uint8_t *)inode->block + sizeof(*eh));
    for (i = 0; i < eh->entries; i++)
    {
        uint32_t len = ext[i].ee_len & 0x7fffU;
        if (logical >= ext[i].ee_block && logical < ext[i].ee_block + len)
        {
            return -EEXIST;
        }
    }

    /*
     * 关键路径（中文）：
     * 先尝试和已有 extent 做“连续块合并”，能有效减少 extent 条目消耗，
     * 对后续频繁 append 写文件（如 tcc 输出）很重要。
     */
    if (eh->entries > 0)
    {
        struct ext4_extent *last = &ext[eh->entries - 1];
        uint32_t last_len = last->ee_len & 0x7fffU;
        uint64_t last_phys_end = ext4_extent_start(last) + last_len;
        if (logical == last->ee_block + last_len &&
            phys_block == last_phys_end &&
            last_len < 0x7fffU)
        {
            last->ee_len = (uint16_t)(last_len + 1U);
            inode->blocks_lo += fs->block_size / 512U;
            return 0;
        }
    }

    if (eh->entries >= eh->max)
    {
        return -ENOSPC;
    }

    insert_pos = eh->entries;
    for (i = 0; i < eh->entries; i++)
    {
        if (logical < ext[i].ee_block)
        {
            insert_pos = i;
            break;
        }
    }

    for (i = eh->entries; i > insert_pos; i--)
    {
        ext[i] = ext[i - 1];
    }

    ext[insert_pos].ee_block = logical;
    ext[insert_pos].ee_len = 1;
    ext[insert_pos].ee_start_hi = (uint16_t)(phys_block >> 32);
    ext[insert_pos].ee_start_lo = (uint32_t)phys_block;
    eh->entries++;
    inode->blocks_lo += fs->block_size / 512U;
    return 0;
}

static int ext4_init_new_inode(struct ext4_inode_disk *inode, uint16_t mode, uint16_t links)
{
    struct ext4_extent_header *eh;

    if (!inode)
    {
        return -EINVAL;
    }

    memset((int8_t *)inode, 0, sizeof(*inode));
    inode->mode = mode;
    inode->uid = 0;
    inode->gid = 0;
    inode->links_count = links;
    inode->flags = EXT4_EXTENTS_FL;
    inode->blocks_lo = 0;
    ext4_set_inode_size(inode, 0);

    eh = (struct ext4_extent_header *)inode->block;
    eh->magic = EXT4_EXT_MAGIC;
    eh->entries = 0;
    eh->max = (uint16_t)((sizeof(inode->block) - sizeof(*eh)) / sizeof(struct ext4_extent));
    eh->depth = 0;
    eh->generation = 0;
    return 0;
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

    ext4_init_inode_ops();
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
    out->nlink = raw_inode.links_count ? raw_inode.links_count : 1;
    out->uid = raw_inode.uid;
    out->gid = raw_inode.gid;
    out->blksize = fs->block_size;
    out->blocks = raw_inode.blocks_lo;
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
            int ret;

            if (de->rec_len == 0)
            {
                break;
            }

            if (index == 0)
            {
                int8_t dbg_name[VFS_NAME_MAX + 1];
                size_t dbg_i;
                size_t dbg_len;

                memset(dbg_name, 0, sizeof(dbg_name));
                dbg_len = de->name_len;
                if (dbg_len > VFS_NAME_MAX)
                {
                    dbg_len = VFS_NAME_MAX;
                }
                for (dbg_i = 0; dbg_i < dbg_len; dbg_i++)
                {
                    dbg_name[dbg_i] = (int8_t)de->name[dbg_i];
                }
                dbg_name[dbg_len] = '\0';
            }

            if (ext4_dirent_is_skip(de))
            {
                off += de->rec_len;
                continue;
            }

            if (seen == index)
            {
                /*
                 * 这里必须先把目录项名字拷贝到栈上。
                 * ext4_fill_vfs_inode() 会继续读 inode/block 元数据，
                 * 可能复用 ext4_block_buf_a，直接使用 de->name 会被覆盖。
                 */
                int8_t name[VFS_NAME_MAX + 1];
                size_t name_len;
                struct vfs_inode child;

                name_len = de->name_len;
                if (name_len > VFS_NAME_MAX)
                {
                    return -ENAMETOOLONG;
                }

                for (size_t i = 0; i < name_len; i++)
                {
                    name[i] = (int8_t)de->name[i];
                }
                name[name_len] = '\0';

                ret = ext4_fill_vfs_inode(fs, de->inode, &child);
                if (ret)
                {
                    return ret;
                }

                out->ino = child.ino;
                out->mode = child.mode;
                out->size = child.size;
                memcpy(out->name, name, name_len + 1);
                return 0;
            }

            seen++;
            off += de->rec_len;
        }
    }

    return -ENOENT;
}

static void ext4_dir_entry_fill(struct ext4_dir_entry_2 *de, uint32_t ino, uint16_t rec_len,
                                uint8_t file_type, const int8_t *name, uint8_t name_len)
{
    de->inode = ino;
    de->rec_len = rec_len;
    de->name_len = name_len;
    de->file_type = file_type;
    if (name_len)
    {
        memcpy(de->name, (int8_t *)name, name_len);
    }
}

struct ext4_dir_loc
{
    uint64_t phys_block;
    uint32_t off;
    uint32_t prev_off;
    uint16_t rec_len;
    uint32_t inode;
    uint8_t file_type;
    uint8_t name_len;
};

static uint8_t ext4_file_type_from_mode(uint16_t mode)
{
    if ((mode & VFS_S_IFMT) == VFS_S_IFDIR)
    {
        return EXT4_FT_DIR;
    }
    if ((mode & VFS_S_IFMT) == VFS_S_IFREG)
    {
        return EXT4_FT_REG_FILE;
    }
    return EXT4_FT_UNKNOWN;
}

static int ext4_dir_find_entry(struct ext4_fs *fs, const struct ext4_inode_disk *dir_inode,
                               const int8_t *name, struct ext4_dir_loc *out_loc)
{
    uint64_t size;
    uint32_t block_count;
    uint32_t logical;
    size_t name_len;

    if (!fs || !dir_inode || !name || !out_loc)
    {
        return -EINVAL;
    }

    name_len = strlen((int8_t *)name);
    if (name_len == 0 || name_len > VFS_NAME_MAX)
    {
        return -EINVAL;
    }

    size = ext4_inode_size(dir_inode);
    block_count = (uint32_t)((size + fs->block_size - 1U) / fs->block_size);
    for (logical = 0; logical < block_count; logical++)
    {
        uint64_t phys_block;
        uint32_t off;
        uint32_t prev_off;

        if (ext4_map_extent_block(fs, dir_inode, logical, &phys_block))
        {
            continue;
        }
        if (ext4_read_block(fs, phys_block, ext4_block_buf_a))
        {
            return -EIO;
        }

        off = 0;
        prev_off = 0xffffffffU;
        while (off < fs->block_size)
        {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + off);

            if (de->rec_len < 8U || off + de->rec_len > fs->block_size)
            {
                return -EIO;
            }

            if (de->inode &&
                de->name_len == name_len &&
                memcmp(de->name, (int8_t *)name, name_len) == 0)
            {
                out_loc->phys_block = phys_block;
                out_loc->off = off;
                out_loc->prev_off = prev_off;
                out_loc->rec_len = de->rec_len;
                out_loc->inode = de->inode;
                out_loc->file_type = de->file_type;
                out_loc->name_len = de->name_len;
                return 0;
            }

            prev_off = off;
            off += de->rec_len;
        }
    }

    return -ENOENT;
}

static int ext4_dir_remove_entry(struct ext4_fs *fs, const struct ext4_dir_loc *loc)
{
    struct ext4_dir_entry_2 *de;

    if (!fs || !loc)
    {
        return -EINVAL;
    }
    if (ext4_read_block(fs, loc->phys_block, ext4_block_buf_a))
    {
        return -EIO;
    }

    de = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + loc->off);
    if (de->inode != loc->inode)
    {
        return -EIO;
    }

    if (loc->prev_off != 0xffffffffU)
    {
        struct ext4_dir_entry_2 *prev = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + loc->prev_off);
        uint32_t sum = (uint32_t)prev->rec_len + (uint32_t)de->rec_len;
        if (sum > 0xffffU)
        {
            return -EIO;
        }
        prev->rec_len = (uint16_t)sum;
    }
    else
    {
        de->inode = 0;
        de->name_len = 0;
        de->file_type = EXT4_FT_UNKNOWN;
    }

    if (ext4_write_block(fs, loc->phys_block, ext4_block_buf_a))
    {
        return -EIO;
    }
    return 0;
}

static int ext4_dir_is_empty(struct ext4_fs *fs, const struct ext4_inode_disk *dir_inode, bool *out_empty)
{
    uint64_t size;
    uint32_t block_count;
    uint32_t logical;

    if (!fs || !dir_inode || !out_empty)
    {
        return -EINVAL;
    }
    *out_empty = true;

    size = ext4_inode_size(dir_inode);
    block_count = (uint32_t)((size + fs->block_size - 1U) / fs->block_size);
    for (logical = 0; logical < block_count; logical++)
    {
        uint64_t phys_block;
        uint32_t off;

        if (ext4_map_extent_block(fs, dir_inode, logical, &phys_block))
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
            if (de->rec_len < 8U || off + de->rec_len > fs->block_size)
            {
                return -EIO;
            }

            if (de->inode && !ext4_dirent_is_skip(de))
            {
                *out_empty = false;
                return 0;
            }
            off += de->rec_len;
        }
    }

    return 0;
}

static int ext4_free_inode_data_blocks(struct ext4_fs *fs, const struct ext4_inode_disk *inode)
{
    struct ext4_extent_header *eh;
    struct ext4_extent *ext;
    uint16_t i;

    if (!fs || !inode)
    {
        return -EINVAL;
    }

    if ((inode->flags & EXT4_EXTENTS_FL) == 0)
    {
        return 0;
    }
    eh = (struct ext4_extent_header *)inode->block;
    if (eh->magic != EXT4_EXT_MAGIC)
    {
        return 0;
    }
    if (eh->depth != 0)
    {
        /*
         * 兼容策略（中文）：
         * 深度>0 的 extent 树释放逻辑尚未实现，这里先保持“功能优先”：
         * unlink/rmdir 可以继续完成，但该 inode 的数据块可能延后回收。
         */
        return 0;
    }

    ext = (struct ext4_extent *)((uint8_t *)inode->block + sizeof(*eh));
    for (i = 0; i < eh->entries; i++)
    {
        uint32_t len = ext[i].ee_len & 0x7fffU;
        uint64_t start = ext4_extent_start(&ext[i]);
        uint32_t j;

        for (j = 0; j < len; j++)
        {
            int ret = ext4_free_block(fs, start + j);
            if (ret)
            {
                return ret;
            }
        }
    }

    return 0;
}

static int ext4_dir_try_add_in_block(struct ext4_fs *fs, uint64_t phys_block,
                                     uint32_t child_ino, const int8_t *name, uint8_t name_len, uint8_t file_type)
{
    uint16_t need;
    uint32_t off;

    if (ext4_read_block(fs, phys_block, ext4_block_buf_a))
    {
        return -EIO;
    }

    need = EXT4_DIR_REC_LEN(name_len);
    off = 0;
    while (off < fs->block_size)
    {
        struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + off);
        uint16_t rec_len;

        rec_len = de->rec_len;
        if (rec_len < 8U || off + rec_len > fs->block_size)
        {
            return -EIO;
        }

        if (de->inode == 0)
        {
            if (rec_len >= need)
            {
                uint16_t remain = rec_len - need;
                if (remain >= 8U)
                {
                    struct ext4_dir_entry_2 *next = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + off + need);
                    memset((int8_t *)next, 0, remain);
                    next->rec_len = remain;
                    rec_len = need;
                }
                ext4_dir_entry_fill(de, child_ino, rec_len, file_type, name, name_len);
                if (ext4_write_block(fs, phys_block, ext4_block_buf_a))
                {
                    return -EIO;
                }
                return 0;
            }
        }
        else
        {
            uint16_t used = EXT4_DIR_REC_LEN(de->name_len);
            if (rec_len >= used && rec_len - used >= need)
            {
                struct ext4_dir_entry_2 *new_de = (struct ext4_dir_entry_2 *)(ext4_block_buf_a + off + used);
                uint16_t new_rec_len = rec_len - used;
                de->rec_len = used;
                ext4_dir_entry_fill(new_de, child_ino, new_rec_len, file_type, name, name_len);
                if (ext4_write_block(fs, phys_block, ext4_block_buf_a))
                {
                    return -EIO;
                }
                return 0;
            }
        }

        off += rec_len;
    }

    return -ENOSPC;
}

static int ext4_dir_add_entry(struct ext4_fs *fs, uint32_t dir_ino, struct ext4_inode_disk *dir_inode,
                              uint32_t child_ino, const int8_t *name, uint8_t file_type)
{
    uint64_t size;
    uint32_t block_count;
    uint32_t logical;
    size_t raw_name_len;
    uint8_t name_len;

    if (!fs || !dir_inode || !name)
    {
        return -EINVAL;
    }

    raw_name_len = strlen((int8_t *)name);
    if (raw_name_len == 0 || raw_name_len > VFS_NAME_MAX)
    {
        return -EINVAL;
    }
    if (raw_name_len > 255U)
    {
        return -ENAMETOOLONG;
    }
    name_len = (uint8_t)raw_name_len;

    size = ext4_inode_size(dir_inode);
    block_count = (uint32_t)((size + fs->block_size - 1U) / fs->block_size);
    for (logical = 0; logical < block_count; logical++)
    {
        uint64_t phys_block;
        int ret;

        ret = ext4_map_extent_block(fs, dir_inode, logical, &phys_block);
        if (ret)
        {
            continue;
        }

        ret = ext4_dir_try_add_in_block(fs, phys_block, child_ino, name, name_len, file_type);
        if (ret == 0)
        {
            return 0;
        }
        if (ret != -ENOSPC)
        {
            return ret;
        }
    }

    /*
     * 目录无空洞时按 ext4 语义增长一个新块：
     * 1) 分配数据块；
     * 2) 在 inode extent 中挂上 logical->physical 映射；
     * 3) 回写 inode，再写目录项数据。
     */
    {
        uint64_t new_block;
        uint32_t new_logical;
        int ret;

        ret = ext4_alloc_block(fs, &new_block);
        if (ret)
        {
            return ret;
        }

        memset((int8_t *)ext4_block_buf_a, 0, fs->block_size);
        ext4_dir_entry_fill((struct ext4_dir_entry_2 *)ext4_block_buf_a,
                            child_ino, (uint16_t)fs->block_size, file_type, name, name_len);
        if (ext4_write_block(fs, new_block, ext4_block_buf_a))
        {
            ext4_free_block(fs, new_block);
            return -EIO;
        }

        new_logical = block_count;
        ret = ext4_inode_add_extent_block(fs, dir_inode, new_logical, new_block);
        if (ret)
        {
            ext4_free_block(fs, new_block);
            return ret;
        }

        ext4_set_inode_size(dir_inode, size + fs->block_size);
        if (ext4_write_inode_raw(fs, dir_ino, dir_inode))
        {
            return -EIO;
        }
    }

    return 0;
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
            uint64_t new_block;
            int alloc_ret;

            /*
             * 关键修复（中文）：
             * 旧实现只能覆盖“已有映射块”，对新建文件第一次写入会直接 ENOSPC。
             * 这里在缺块时即时分配 block 并追加 extent，让 O_CREAT + write 真正可用。
             */
            alloc_ret = ext4_alloc_block(fs, &new_block);
            if (alloc_ret)
            {
                return done ? (ssize_t)done : alloc_ret;
            }
            alloc_ret = ext4_inode_add_extent_block(fs, &raw_inode, logical, new_block);
            if (alloc_ret)
            {
                ext4_free_block(fs, new_block);
                return done ? (ssize_t)done : alloc_ret;
            }
            if (ext4_write_inode_raw(fs, inode->ino, &raw_inode))
            {
                ext4_free_block(fs, new_block);
                return done ? (ssize_t)done : -EIO;
            }
            phys_block = new_block;
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

static int ext4_create(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk parent_raw;
    struct ext4_inode_disk child_raw;
    struct vfs_inode exists;
    uint32_t ino;
    int ret;

    if (!dir || !name || name[0] == '\0')
    {
        return -EINVAL;
    }

    if (strlen((int8_t *)name) > VFS_NAME_MAX)
    {
        return -ENAMETOOLONG;
    }

    fs = (struct ext4_fs *)dir->fs_private;
    ret = ext4_lookup(dir, name, &exists);
    if (ret == 0)
    {
        return -EEXIST;
    }
    if (ret != -ENOENT)
    {
        return ret;
    }

    if (ext4_read_inode_raw(fs, dir->ino, &parent_raw))
    {
        return -EIO;
    }
    if ((parent_raw.mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    ret = ext4_alloc_inode(fs, &ino);
    if (ret)
    {
        return ret;
    }

    ext4_init_new_inode(&child_raw, (uint16_t)(VFS_S_IFREG | (mode & 0777U)), 1U);
    if (ext4_write_inode_raw(fs, ino, &child_raw))
    {
        ext4_free_inode(fs, ino);
        return -EIO;
    }

    ret = ext4_dir_add_entry(fs, dir->ino, &parent_raw, ino, name, EXT4_FT_REG_FILE);
    if (ret)
    {
        ext4_free_inode(fs, ino);
        return ret;
    }

    if (out)
    {
        return ext4_fill_vfs_inode(fs, ino, out);
    }
    return 0;
}

static int ext4_mkdir(struct vfs_inode *dir, const int8_t *name, uint16_t mode, struct vfs_inode *out)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk parent_raw;
    struct ext4_inode_disk child_raw;
    struct vfs_inode exists;
    uint32_t ino;
    uint64_t block;
    int ret;

    if (!dir || !name || name[0] == '\0')
    {
        return -EINVAL;
    }
    if (strlen((int8_t *)name) > VFS_NAME_MAX)
    {
        return -ENAMETOOLONG;
    }

    fs = (struct ext4_fs *)dir->fs_private;
    ret = ext4_lookup(dir, name, &exists);
    if (ret == 0)
    {
        return -EEXIST;
    }
    if (ret != -ENOENT)
    {
        return ret;
    }

    if (ext4_read_inode_raw(fs, dir->ino, &parent_raw))
    {
        return -EIO;
    }
    if ((parent_raw.mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    ret = ext4_alloc_inode(fs, &ino);
    if (ret)
    {
        return ret;
    }

    ext4_init_new_inode(&child_raw, (uint16_t)(VFS_S_IFDIR | (mode & 0777U)), 2U);
    ret = ext4_alloc_block(fs, &block);
    if (ret)
    {
        ext4_free_inode(fs, ino);
        return ret;
    }

    ret = ext4_inode_add_extent_block(fs, &child_raw, 0, block);
    if (ret)
    {
        ext4_free_block(fs, block);
        ext4_free_inode(fs, ino);
        return ret;
    }
    ext4_set_inode_size(&child_raw, fs->block_size);

    memset((int8_t *)ext4_block_buf_a, 0, fs->block_size);
    ext4_dir_entry_fill((struct ext4_dir_entry_2 *)ext4_block_buf_a, ino, EXT4_DIR_REC_LEN(1), EXT4_FT_DIR, (const int8_t *)".", 1);
    ext4_dir_entry_fill((struct ext4_dir_entry_2 *)(ext4_block_buf_a + EXT4_DIR_REC_LEN(1)),
                        dir->ino, (uint16_t)(fs->block_size - EXT4_DIR_REC_LEN(1)), EXT4_FT_DIR, (const int8_t *)"..", 2);
    if (ext4_write_block(fs, block, ext4_block_buf_a))
    {
        ext4_free_block(fs, block);
        ext4_free_inode(fs, ino);
        return -EIO;
    }

    if (ext4_write_inode_raw(fs, ino, &child_raw))
    {
        ext4_free_block(fs, block);
        ext4_free_inode(fs, ino);
        return -EIO;
    }

    ret = ext4_dir_add_entry(fs, dir->ino, &parent_raw, ino, name, EXT4_FT_DIR);
    if (ret)
    {
        ext4_free_block(fs, block);
        ext4_free_inode(fs, ino);
        return ret;
    }

    if (parent_raw.links_count < 0xffffU)
    {
        parent_raw.links_count++;
        if (ext4_write_inode_raw(fs, dir->ino, &parent_raw))
        {
            return -EIO;
        }
    }

    if (out)
    {
        return ext4_fill_vfs_inode(fs, ino, out);
    }
    return 0;
}

static int ext4_unlink(struct vfs_inode *dir, const int8_t *name, bool dir_only)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk parent_raw;
    struct ext4_inode_disk child_raw;
    struct ext4_dir_loc loc;
    struct ext4_inode_disk cleared;
    uint16_t type;
    bool empty;
    int ret;

    if (!dir || !name || name[0] == '\0')
    {
        return -EINVAL;
    }
    if (strlen((int8_t *)name) > VFS_NAME_MAX)
    {
        return -ENAMETOOLONG;
    }

    fs = (struct ext4_fs *)dir->fs_private;
    if (ext4_read_inode_raw(fs, dir->ino, &parent_raw))
    {
        return -EIO;
    }
    if ((parent_raw.mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    ret = ext4_dir_find_entry(fs, &parent_raw, name, &loc);
    if (ret)
    {
        return ret;
    }
    if (ext4_read_inode_raw(fs, loc.inode, &child_raw))
    {
        return -EIO;
    }

    type = child_raw.mode & VFS_S_IFMT;
    if (dir_only)
    {
        if (type != VFS_S_IFDIR)
        {
            return -ENOTDIR;
        }
        ret = ext4_dir_is_empty(fs, &child_raw, &empty);
        if (ret)
        {
            return ret;
        }
        if (!empty)
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

    ret = ext4_dir_remove_entry(fs, &loc);
    if (ret)
    {
        return ret;
    }

    if (dir_only && parent_raw.links_count > 0)
    {
        parent_raw.links_count--;
        if (ext4_write_inode_raw(fs, dir->ino, &parent_raw))
        {
            return -EIO;
        }
    }

    ret = ext4_free_inode_data_blocks(fs, &child_raw);
    if (ret)
    {
        return ret;
    }

    memset((int8_t *)&cleared, 0, sizeof(cleared));
    if (ext4_write_inode_raw(fs, loc.inode, &cleared))
    {
        return -EIO;
    }

    return ext4_free_inode(fs, loc.inode);
}

static int ext4_rename(struct vfs_inode *old_dir, const int8_t *old_name,
                       struct vfs_inode *new_dir, const int8_t *new_name)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk old_parent_raw;
    struct ext4_inode_disk new_parent_raw;
    struct ext4_inode_disk child_raw;
    struct ext4_dir_loc old_loc;
    struct ext4_dir_loc new_loc;
    int ret;
    uint8_t file_type;

    if (!old_dir || !new_dir || !old_name || !new_name ||
        old_name[0] == '\0' || new_name[0] == '\0')
    {
        return -EINVAL;
    }
    if (strlen((int8_t *)old_name) > VFS_NAME_MAX || strlen((int8_t *)new_name) > VFS_NAME_MAX)
    {
        return -ENAMETOOLONG;
    }

    if (old_dir->sb != new_dir->sb)
    {
        return -EXDEV;
    }
    if (strcmp((int8_t *)old_name, (int8_t *)new_name) == 0 && old_dir->ino == new_dir->ino)
    {
        return 0;
    }

    fs = (struct ext4_fs *)old_dir->fs_private;
    if (ext4_read_inode_raw(fs, old_dir->ino, &old_parent_raw) ||
        ext4_read_inode_raw(fs, new_dir->ino, &new_parent_raw))
    {
        return -EIO;
    }
    if ((old_parent_raw.mode & VFS_S_IFMT) != VFS_S_IFDIR ||
        (new_parent_raw.mode & VFS_S_IFMT) != VFS_S_IFDIR)
    {
        return -ENOTDIR;
    }

    ret = ext4_dir_find_entry(fs, &old_parent_raw, old_name, &old_loc);
    if (ret)
    {
        return ret;
    }
    ret = ext4_dir_find_entry(fs, &new_parent_raw, new_name, &new_loc);
    if (ret == 0)
    {
        return -EEXIST;
    }
    if (ret != -ENOENT)
    {
        return ret;
    }

    if (ext4_read_inode_raw(fs, old_loc.inode, &child_raw))
    {
        return -EIO;
    }

    if ((child_raw.mode & VFS_S_IFMT) == VFS_S_IFDIR && old_dir->ino != new_dir->ino)
    {
        /*
         * 先保证稳定语义：目录跨父目录移动需要维护 ".." 和 link 计数，
         * 当前这条路径先显式拒绝，避免生成不一致目录结构。
         */
        return -EXDEV;
    }

    file_type = old_loc.file_type;
    if (file_type == EXT4_FT_UNKNOWN)
    {
        file_type = ext4_file_type_from_mode(child_raw.mode);
    }

    ret = ext4_dir_add_entry(fs, new_dir->ino, &new_parent_raw, old_loc.inode, new_name, file_type);
    if (ret)
    {
        return ret;
    }

    if (ext4_read_inode_raw(fs, old_dir->ino, &old_parent_raw))
    {
        return -EIO;
    }
    ret = ext4_dir_find_entry(fs, &old_parent_raw, old_name, &old_loc);
    if (ret)
    {
        return ret;
    }
    ret = ext4_dir_remove_entry(fs, &old_loc);
    if (ret)
    {
        /*
         * 尝试回滚新目录项，避免 rename 失败后出现“双名指向”。
         */
        if (ext4_read_inode_raw(fs, new_dir->ino, &new_parent_raw) == 0 &&
            ext4_dir_find_entry(fs, &new_parent_raw, new_name, &new_loc) == 0)
        {
            (void)ext4_dir_remove_entry(fs, &new_loc);
        }
        return ret;
    }

    return 0;
}

static int ext4_truncate(struct vfs_inode *inode, uint64_t size)
{
    struct ext4_fs *fs;
    struct ext4_inode_disk raw_inode;

    if (!inode)
    {
        return -EINVAL;
    }

    fs = (struct ext4_fs *)inode->fs_private;
    if (ext4_read_inode_raw(fs, inode->ino, &raw_inode))
    {
        return -EIO;
    }
    if ((raw_inode.mode & VFS_S_IFMT) != VFS_S_IFREG)
    {
        return -EISDIR;
    }

    /*
     * 最小 truncate 语义（中文）：
     * 目前仅保证 size<=原大小时元数据可更新，不释放块，避免把“根分区可写”目标拖慢。
     * 后续可继续补真实块回收。
     */
    if (size > ext4_inode_size(&raw_inode))
    {
        return -ENOTSUP;
    }

    ext4_set_inode_size(&raw_inode, size);
    if (ext4_write_inode_raw(fs, inode->ino, &raw_inode))
    {
        return -EIO;
    }
    inode->size = size;
    return 0;
}

int ext4_mount_root(void)
{
    struct gpt_entry entry;
    struct ext4_super_block *sb;
    struct vfs_inode root_inode;
    int ret;

    ext4_init_inode_ops();
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
