#include "fs/fat32.h"
#include "fs/vfs.h"
#include "driver/virtio_blk.h"
#include "errno.h"
#include "lib/libmem.h"
#include "lib/libstr.h"
#include "mmu.h"
#include "printk.h"

#define GPT_SIGNATURE               "EFI PART"
#define GPT_ENTRY_SIZE_MIN          128

#define FAT32_PARTITION_INDEX       0
#define FAT32_ATTR_READ_ONLY        0x01
#define FAT32_ATTR_HIDDEN           0x02
#define FAT32_ATTR_SYSTEM           0x04
#define FAT32_ATTR_VOLUME_ID        0x08
#define FAT32_ATTR_DIRECTORY        0x10
#define FAT32_ATTR_ARCHIVE          0x20
#define FAT32_ATTR_LFN              0x0f

#define FAT32_EOC_MIN               0x0ffffff8U
#define FAT32_CLUSTER_MIN           2U
#define FAT32_MAX_CLUSTER_SIZE      4096U

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

struct fat32_bpb
{
    uint8_t jmp_boot[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} __attribute__((packed));

struct fat32_dirent
{
    uint8_t name[11];
    uint8_t attr;
    uint8_t ntres;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} __attribute__((packed));

struct fat32_fs
{
    struct vfs_superblock sb;
    uint64_t part_lba_start;
    uint64_t part_lba_count;
    uint32_t fat_start_lba;
    uint32_t first_data_sector;
    uint32_t total_sectors;
    uint32_t fat_size_sectors;
    uint32_t total_clusters;
    uint32_t root_cluster;
    uint32_t cluster_size;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint8_t num_fats;
};

static struct fat32_fs fat32_boot_fs;
static uint8_t fat32_sector_buf[VIRTIO_BLK_SECTOR_SIZE];
static uint8_t fat32_cluster_buf_a[FAT32_MAX_CLUSTER_SIZE];
static uint8_t fat32_cluster_buf_b[FAT32_MAX_CLUSTER_SIZE];

static int fat32_lookup(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out);
static int fat32_readdir(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out);
static ssize_t fat32_read(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len);
static ssize_t fat32_write(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len);

static struct vfs_inode_ops fat32_inode_ops;
static bool fat32_inode_ops_ready;

static void fat32_init_inode_ops(void)
{
    if (fat32_inode_ops_ready)
    {
        return;
    }

    /*
     * 文件系统回调在切到高地址内核后必须重绑定。
     * 否则静态初始化里留下的低地址函数指针会跳到未映射区域。
     */
    fat32_inode_ops.lookup = fat32_lookup;
    fat32_inode_ops.readdir = fat32_readdir;
    fat32_inode_ops.read = fat32_read;
    fat32_inode_ops.write = fat32_write;
    fat32_inode_ops_ready = true;
}

static inline int fat32_ascii_tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return ch - 'A' + 'a';
    }

    return ch;
}

static uint32_t fat32_dirent_first_cluster(const struct fat32_dirent *de)
{
    return ((uint32_t)de->fst_clus_hi << 16) | de->fst_clus_lo;
}

static bool fat32_cluster_is_eoc(uint32_t cluster)
{
    return cluster >= FAT32_EOC_MIN;
}

static int fat32_read_sector(uint64_t lba, void *buf)
{
    return virtio_blk_read(lba, buf, 1);
}

static int fat32_read_gpt_entry(uint32_t index, struct gpt_entry *entry)
{
    struct gpt_header *header;
    uint64_t entry_lba;
    uint32_t offset;

    if (fat32_read_sector(1, fat32_sector_buf))
    {
        return -EIO;
    }

    header = (struct gpt_header *)fat32_sector_buf;
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

    if (fat32_read_sector(entry_lba, fat32_sector_buf))
    {
        return -EIO;
    }

    memcpy((int8_t *)entry, (int8_t *)(fat32_sector_buf + offset), sizeof(*entry));
    return 0;
}

static uint64_t fat32_cluster_lba(const struct fat32_fs *fs, uint32_t cluster)
{
    return fs->part_lba_start + fs->first_data_sector +
           ((uint64_t)(cluster - FAT32_CLUSTER_MIN) * fs->sectors_per_cluster);
}

static int fat32_read_cluster(const struct fat32_fs *fs, uint32_t cluster, void *buf)
{
    if (cluster < FAT32_CLUSTER_MIN)
    {
        return -EINVAL;
    }

    return virtio_blk_read(fat32_cluster_lba(fs, cluster), buf, fs->sectors_per_cluster);
}

static int fat32_write_cluster(const struct fat32_fs *fs, uint32_t cluster, const void *buf)
{
    if (cluster < FAT32_CLUSTER_MIN)
    {
        return -EINVAL;
    }

    return virtio_blk_write(fat32_cluster_lba(fs, cluster), buf, fs->sectors_per_cluster);
}

static int fat32_next_cluster(const struct fat32_fs *fs, uint32_t cluster, uint32_t *next)
{
    uint32_t fat_offset;
    uint64_t fat_lba;
    uint32_t offset;

    fat_offset = cluster * 4;
    fat_lba = fs->part_lba_start + fs->fat_start_lba + (fat_offset / fs->bytes_per_sector);
    offset = fat_offset % fs->bytes_per_sector;

    if (fat32_read_sector(fat_lba, fat32_sector_buf))
    {
        return -EIO;
    }

    *next = (*(uint32_t *)(fat32_sector_buf + offset)) & 0x0fffffffU;
    return 0;
}

static int fat32_chain_nth_cluster(const struct fat32_fs *fs, uint32_t first_cluster,
                                   uint32_t index, uint32_t *cluster_out)
{
    uint32_t cluster;
    uint32_t step;

    if (first_cluster < FAT32_CLUSTER_MIN)
    {
        return -EINVAL;
    }

    cluster = first_cluster;
    for (step = 0; step < index; step++)
    {
        uint32_t next;

        if (fat32_next_cluster(fs, cluster, &next))
        {
            return -EIO;
        }

        if (fat32_cluster_is_eoc(next))
        {
            return -ENOSPC;
        }

        cluster = next;
    }

    *cluster_out = cluster;
    return 0;
}

static bool fat32_short_name_match(const uint8_t raw_name[11], const int8_t *name)
{
    int8_t formatted[13];
    size_t user_len;
    size_t i;
    size_t pos;

    user_len = strlen((int8_t *)name);
    pos = 0;

    for (i = 0; i < 8 && raw_name[i] != ' '; i++)
    {
        formatted[pos++] = (int8_t)fat32_ascii_tolower(raw_name[i]);
    }

    if (raw_name[8] != ' ')
    {
        formatted[pos++] = '.';
        for (i = 8; i < 11 && raw_name[i] != ' '; i++)
        {
            formatted[pos++] = (int8_t)fat32_ascii_tolower(raw_name[i]);
        }
    }

    if (pos != user_len)
    {
        return false;
    }

    for (i = 0; i < pos; i++)
    {
        if (fat32_ascii_tolower(formatted[i]) != fat32_ascii_tolower(name[i]))
        {
            return false;
        }
    }

    return true;
}

static void fat32_fill_root_inode(struct fat32_fs *fs, struct vfs_inode *out)
{
    fat32_init_inode_ops();
    memset((int8_t *)out, 0, sizeof(*out));
    out->sb = &fs->sb;
    out->ops = &fat32_inode_ops;
    out->ino = fs->root_cluster;
    out->mode = VFS_S_IFDIR;
    out->size = 0;
    out->fs_private = fs;
    out->private_data[0] = fs->root_cluster;
    out->private_data[1] = 0;
    out->private_data[2] = 0;
    out->private_data[3] = FAT32_ATTR_DIRECTORY;
}

static void fat32_fill_inode(struct fat32_fs *fs, const struct fat32_dirent *de,
                             uint32_t dir_cluster, uint32_t dir_offset,
                             struct vfs_inode *out)
{
    uint32_t first_cluster;

    fat32_init_inode_ops();
    memset((int8_t *)out, 0, sizeof(*out));
    first_cluster = fat32_dirent_first_cluster(de);

    out->sb = &fs->sb;
    out->ops = &fat32_inode_ops;
    out->ino = first_cluster ? first_cluster : ((dir_cluster << 16) | dir_offset);
    out->mode = (de->attr & FAT32_ATTR_DIRECTORY) ? VFS_S_IFDIR : VFS_S_IFREG;
    out->size = de->file_size;
    out->fs_private = fs;
    out->private_data[0] = first_cluster;
    out->private_data[1] = dir_cluster;
    out->private_data[2] = dir_offset;
    out->private_data[3] = de->attr;
}

static int fat32_update_dirent_size(struct vfs_inode *inode, uint64_t size)
{
    struct fat32_fs *fs;
    struct fat32_dirent *de;
    uint32_t dir_cluster;
    uint32_t dir_offset;

    dir_cluster = (uint32_t)inode->private_data[1];
    dir_offset = (uint32_t)inode->private_data[2];
    if (!dir_cluster)
    {
        return -EINVAL;
    }

    fs = (struct fat32_fs *)inode->fs_private;
    if (fat32_read_cluster(fs, dir_cluster, fat32_cluster_buf_b))
    {
        return -EIO;
    }

    de = (struct fat32_dirent *)(fat32_cluster_buf_b + dir_offset);
    de->file_size = (uint32_t)size;

    if (fat32_write_cluster(fs, dir_cluster, fat32_cluster_buf_b))
    {
        return -EIO;
    }

    inode->size = size;
    return 0;
}

static int fat32_lookup(struct vfs_inode *dir, const int8_t *name, struct vfs_inode *out)
{
    struct fat32_fs *fs;
    uint32_t cluster;

    fs = (struct fat32_fs *)dir->fs_private;
    cluster = (uint32_t)dir->private_data[0];
    if (cluster < FAT32_CLUSTER_MIN)
    {
        return -ENOENT;
    }

    while (1)
    {
        uint32_t off;

        if (fat32_read_cluster(fs, cluster, fat32_cluster_buf_a))
        {
            return -EIO;
        }

        for (off = 0; off < fs->cluster_size; off += sizeof(struct fat32_dirent))
        {
            struct fat32_dirent *de = (struct fat32_dirent *)(fat32_cluster_buf_a + off);

            if (de->name[0] == 0x00)
            {
                return -ENOENT;
            }

            if (de->name[0] == 0xe5 || de->attr == FAT32_ATTR_LFN || (de->attr & FAT32_ATTR_VOLUME_ID))
            {
                continue;
            }

            if (fat32_short_name_match(de->name, name))
            {
                fat32_fill_inode(fs, de, cluster, off, out);
                return 0;
            }
        }

        if (fat32_next_cluster(fs, cluster, &cluster))
        {
            return -EIO;
        }

        if (fat32_cluster_is_eoc(cluster))
        {
            return -ENOENT;
        }
    }
}

static bool fat32_dirent_is_skip(const struct fat32_dirent *de)
{
    if (de->name[0] == 0x00 || de->name[0] == 0xe5)
    {
        return true;
    }

    if (de->attr == FAT32_ATTR_LFN || (de->attr & FAT32_ATTR_VOLUME_ID))
    {
        return true;
    }

    if (de->name[0] == '.')
    {
        return true;
    }

    return false;
}

static int fat32_short_name_to_string(const uint8_t raw_name[11], int8_t *out, size_t out_len)
{
    size_t i;
    size_t pos;

    if (!out_len)
    {
        return -EINVAL;
    }

    pos = 0;
    for (i = 0; i < 8 && raw_name[i] != ' '; i++)
    {
        if (pos + 1 >= out_len)
        {
            return -ENAMETOOLONG;
        }
        out[pos++] = (int8_t)fat32_ascii_tolower(raw_name[i]);
    }

    if (raw_name[8] != ' ')
    {
        if (pos + 1 >= out_len)
        {
            return -ENAMETOOLONG;
        }
        out[pos++] = '.';
        for (i = 8; i < 11 && raw_name[i] != ' '; i++)
        {
            if (pos + 1 >= out_len)
            {
                return -ENAMETOOLONG;
            }
            out[pos++] = (int8_t)fat32_ascii_tolower(raw_name[i]);
        }
    }

    out[pos] = '\0';
    return 0;
}

static int fat32_readdir(struct vfs_inode *dir, uint32_t index, struct vfs_dirent *out)
{
    struct fat32_fs *fs;
    uint32_t cluster;
    uint32_t seen;

    fs = (struct fat32_fs *)dir->fs_private;
    cluster = (uint32_t)dir->private_data[0];
    if (cluster < FAT32_CLUSTER_MIN)
    {
        return -ENOENT;
    }

    seen = 0;
    while (1)
    {
        uint32_t off;

        if (fat32_read_cluster(fs, cluster, fat32_cluster_buf_a))
        {
            return -EIO;
        }

        for (off = 0; off < fs->cluster_size; off += sizeof(struct fat32_dirent))
        {
            struct fat32_dirent *de = (struct fat32_dirent *)(fat32_cluster_buf_a + off);
            struct vfs_inode child;
            int ret;

            if (de->name[0] == 0x00)
            {
                return -ENOENT;
            }

            if (fat32_dirent_is_skip(de))
            {
                continue;
            }

            if (seen == index)
            {
                ret = fat32_short_name_to_string(de->name, out->name, sizeof(out->name));
                if (ret)
                {
                    return ret;
                }

                fat32_fill_inode(fs, de, cluster, off, &child);
                out->ino = child.ino;
                out->mode = child.mode;
                out->size = child.size;
                return 0;
            }

            seen++;
        }

        if (fat32_next_cluster(fs, cluster, &cluster))
        {
            return -EIO;
        }

        if (fat32_cluster_is_eoc(cluster))
        {
            return -ENOENT;
        }
    }
}

static ssize_t fat32_read(struct vfs_inode *inode, uint64_t offset, void *buf, size_t len)
{
    struct fat32_fs *fs;
    uint8_t *dst;
    uint32_t first_cluster;
    uint64_t end;
    size_t done;

    fs = (struct fat32_fs *)inode->fs_private;
    first_cluster = (uint32_t)inode->private_data[0];

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
        uint32_t cluster_index;
        uint32_t cluster_off;
        uint32_t chunk;
        uint32_t cluster;

        cluster_index = offset / fs->cluster_size;
        cluster_off = offset % fs->cluster_size;
        chunk = fs->cluster_size - cluster_off;
        if (chunk > end - offset)
        {
            chunk = end - offset;
        }

        if (fat32_chain_nth_cluster(fs, first_cluster, cluster_index, &cluster))
        {
            return done ? (ssize_t)done : -EIO;
        }

        if (fat32_read_cluster(fs, cluster, fat32_cluster_buf_a))
        {
            return done ? (ssize_t)done : -EIO;
        }

        memcpy((int8_t *)(dst + done), (int8_t *)(fat32_cluster_buf_a + cluster_off), chunk);
        done += chunk;
        offset += chunk;
    }

    return done;
}

static ssize_t fat32_write(struct vfs_inode *inode, uint64_t offset, const void *buf, size_t len)
{
    struct fat32_fs *fs;
    const uint8_t *src;
    uint32_t first_cluster;
    uint64_t end;
    size_t done;

    fs = (struct fat32_fs *)inode->fs_private;
    first_cluster = (uint32_t)inode->private_data[0];

    if ((inode->private_data[3] & FAT32_ATTR_DIRECTORY) != 0)
    {
        return -EISDIR;
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
        uint32_t cluster_index;
        uint32_t cluster_off;
        uint32_t chunk;
        uint32_t cluster;
        int ret;

        cluster_index = offset / fs->cluster_size;
        cluster_off = offset % fs->cluster_size;
        chunk = fs->cluster_size - cluster_off;
        if (chunk > len - done)
        {
            chunk = len - done;
        }

        ret = fat32_chain_nth_cluster(fs, first_cluster, cluster_index, &cluster);
        if (ret)
        {
            return done ? (ssize_t)done : ret;
        }

        if (cluster_off || chunk != fs->cluster_size)
        {
            if (fat32_read_cluster(fs, cluster, fat32_cluster_buf_a))
            {
                return done ? (ssize_t)done : -EIO;
            }
        }

        memcpy((int8_t *)(fat32_cluster_buf_a + cluster_off), (int8_t *)(src + done), chunk);
        if (fat32_write_cluster(fs, cluster, fat32_cluster_buf_a))
        {
            return done ? (ssize_t)done : -EIO;
        }

        offset += chunk;
        done += chunk;
    }

    if (end > inode->size)
    {
        if (fat32_update_dirent_size(inode, end))
        {
            return done ? (ssize_t)done : -EIO;
        }
    }

    return done;
}

int fat32_mount(const int8_t *path)
{
    struct gpt_entry entry;
    struct fat32_bpb *bpb;
    struct vfs_inode root_inode;
    uint32_t data_sectors;
    int ret;

    printk("[fat32\tinit]: reading GPT entry 1\n");
    ret = fat32_read_gpt_entry(FAT32_PARTITION_INDEX, &entry);
    if (ret)
    {
        return ret;
    }

    fat32_boot_fs.part_lba_start = entry.first_lba;
    fat32_boot_fs.part_lba_count = entry.last_lba - entry.first_lba + 1;

    if (fat32_read_sector(fat32_boot_fs.part_lba_start, fat32_sector_buf))
    {
        return -EIO;
    }

    bpb = (struct fat32_bpb *)fat32_sector_buf;
    if (bpb->bytes_per_sector != VIRTIO_BLK_SECTOR_SIZE)
    {
        printk("[fat32\tinit]: unsupported sector size %u\n", bpb->bytes_per_sector);
        return -ENOTSUP;
    }

    fat32_boot_fs.bytes_per_sector = bpb->bytes_per_sector;
    fat32_boot_fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fat32_boot_fs.cluster_size = bpb->bytes_per_sector * bpb->sectors_per_cluster;
    if (fat32_boot_fs.cluster_size > FAT32_MAX_CLUSTER_SIZE)
    {
        printk("[fat32\tinit]: cluster size %u too large\n", fat32_boot_fs.cluster_size);
        return -ENOTSUP;
    }

    fat32_boot_fs.total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    fat32_boot_fs.fat_size_sectors = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    fat32_boot_fs.root_cluster = bpb->root_cluster;
    fat32_boot_fs.num_fats = bpb->num_fats;
    fat32_boot_fs.fat_start_lba = bpb->reserved_sector_count;
    fat32_boot_fs.first_data_sector = bpb->reserved_sector_count + ((uint32_t)bpb->num_fats * fat32_boot_fs.fat_size_sectors);
    data_sectors = fat32_boot_fs.total_sectors - fat32_boot_fs.first_data_sector;
    fat32_boot_fs.total_clusters = data_sectors / bpb->sectors_per_cluster;

    fat32_boot_fs.sb.name = "fat32";
    fat32_boot_fs.sb.fs_private = &fat32_boot_fs;

    fat32_fill_root_inode(&fat32_boot_fs, &root_inode);
    ret = vfs_mount(path, &fat32_boot_fs.sb, &root_inode);
    if (ret)
    {
        return ret;
    }

    printk("[fat32\tinit]: mounted FAT32 at %s, cluster_size=%u root_cluster=%u\n",
           path, fat32_boot_fs.cluster_size, fat32_boot_fs.root_cluster);
    return 0;
}
