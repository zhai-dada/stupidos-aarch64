#ifndef __VIRTIO_NET_H__
#define __VIRTIO_NET_H__

#include "asm/types.h"

int virtio_net_init(void);
void virtio_net_poll(void);
uint32_t virtio_net_irq_count(void);

#endif
