#ifndef ENYX_DEV_H2F_LOOP_H
#define ENYX_DEV_H2F_LOOP_H

#include <linux/types.h>
#include <linux/device.h>

struct enyx_h2f_loop;

int
enyx_h2f_loop_init(struct enyx_h2f_loop ** h2f_loop,
                   unsigned long io_base, size_t io_size,
                   int major, int minor,
                   struct device * parent,
                   struct class * h2f_loop_class);

void
enyx_h2f_loop_destroy(struct enyx_h2f_loop * h2f_loop);

#endif // ENYX_DEV_H2F_LOOP_H

