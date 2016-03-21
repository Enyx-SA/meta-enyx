#ifndef ENYX_DEV_IO_SPACE_H
#define ENYX_DEV_IO_SPACE_H

#include <linux/types.h>
#include <linux/device.h>

struct enyx_io_space;

int
enyx_io_space_init(struct enyx_io_space ** io_space,
                   unsigned long io_base, size_t io_size,
                   int major, int minor,
                   struct device * parent,
                   struct class * device_class);

void
enyx_io_space_destroy(struct enyx_io_space * io_space);

#endif // ENYX_DEV_IO_SPACE_H

