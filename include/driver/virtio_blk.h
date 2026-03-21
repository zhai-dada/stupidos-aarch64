#ifndef __VIRTIO_BLK_H__
#define __VIRTIO_BLK_H__

#include "asm/types.h"

#define VIRTIO_MMIO0_BASE       0x0A000000UL
#define VIRTIO_MMIO_STRIDE      0x00000200UL
#define VIRTIO_MMIO_COUNT       32
#define VIRTIO_MMIO_SIZE        (VIRTIO_MMIO_STRIDE * VIRTIO_MMIO_COUNT)
#define VIRTIO_MMIO_HIGH_BASE   0xFFFF900000000000UL

#define VIRTIO_BLK_SECTOR_SIZE  512

int virtio_blk_init(void);
int virtio_blk_read(uint64_t sector, void *buf, uint32_t sector_count);
int virtio_blk_write(uint64_t sector, const void *buf, uint32_t sector_count);
uint64_t virtio_blk_capacity_sectors(void);

#endif
