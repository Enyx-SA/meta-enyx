#ifndef ENYX_DEV_DMA_BUFFER_H
#define ENYX_DEV_DMA_BUFFER_H

#include <linux/types.h>
#include <linux/device.h>

struct enyx_dma_buffer;

int
enyx_dma_buffer_init(struct enyx_dma_buffer ** dma,
              size_t page_count,
              int major, int minor,
              struct device * parent,
              struct class * device_class);

void
enyx_dma_buffer_destroy(struct enyx_dma_buffer * dma);

#endif // ENYX_DEV_DMA_BUFFER_H

