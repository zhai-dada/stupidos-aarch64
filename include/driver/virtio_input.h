#ifndef __VIRTIO_INPUT_H__
#define __VIRTIO_INPUT_H__

#include "asm/types.h"

int virtio_input_init(void);
void virtio_input_poll(void);
uint32_t virtio_input_irq_count(void);

#endif
