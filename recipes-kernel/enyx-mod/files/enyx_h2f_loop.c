#include "enyx_h2f_loop.h"

#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/sysfs.h>

struct enyx_h2f_loop {
    struct semaphore lock;

    struct cdev cdev;

    struct device device;
    unsigned long io_base;
    size_t io_size;
};

static int
enyx_h2f_loop_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct enyx_h2f_loop * h2f_loop = filp->private_data;
    const unsigned long io_offset = vma->vm_pgoff << PAGE_SHIFT,
                        io_start = h2f_loop->io_base + io_offset,
                        vm_size = vma->vm_end - vma->vm_start;
    int err;

    if (down_interruptible(&h2f_loop->lock))
        return -ERESTARTSYS;

    dev_dbg(&h2f_loop->device, "Mmaping 0x%lx into 0x%lx (%lu)\n",
            io_start, vma->vm_start, vm_size);

    if ((h2f_loop->io_size - io_offset) < vm_size) {
        err = -EINVAL;
        goto err_invalid_arg;
    }

    err = io_remap_pfn_range(vma,
                             vma->vm_start,
                             io_start >> PAGE_SHIFT,
                             vm_size,
                             /* The memory is mapped as WC */
                             pgprot_writecombine(vma->vm_page_prot));

err_invalid_arg:
    up(&h2f_loop->lock);

    return err;
}

static int
enyx_h2f_loop_open(struct inode *inode, struct file *filp)
{
    struct enyx_h2f_loop * h2f_loop = container_of(inode->i_cdev,
                                                   struct enyx_h2f_loop,
                                                   cdev);
                    int err = 0;

    filp->private_data = h2f_loop;

    if (down_interruptible(&h2f_loop->lock))
        return -ERESTARTSYS;

    dev_dbg(&h2f_loop->device, "Open\n");

    up(&h2f_loop->lock);

    return err;
}

static int
enyx_h2f_loop_close(struct inode * inode, struct file *filp)
{
    struct enyx_h2f_loop * h2f_loop = filp->private_data;

    if (down_interruptible(&h2f_loop->lock))
        return -ERESTARTSYS;

    dev_dbg(&h2f_loop->device, "Close\n");

    up(&h2f_loop->lock);

    return 0;
}

static const struct file_operations enyx_h2f_loop_file_ops = {
    .owner          = THIS_MODULE,
    .open           = enyx_h2f_loop_open,
    .release        = enyx_h2f_loop_close,
    .mmap           = enyx_h2f_loop_mmap
};

static ssize_t
io_base_show(struct device * device,
             struct device_attribute * attr,
             char * buf)
{
    struct enyx_h2f_loop * h2f_loop = dev_get_drvdata(device);
    return scnprintf(buf, PAGE_SIZE, "0x%lx\n", h2f_loop->io_base);
}

static DEVICE_ATTR_RO(io_base);

static ssize_t
io_size_show(struct device * device,
             struct device_attribute * attr,
             char * buf)
{
    struct enyx_h2f_loop * h2f_loop = dev_get_drvdata(device);
    return scnprintf(buf, PAGE_SIZE, "%zu\n", h2f_loop->io_size);
}

static DEVICE_ATTR_RO(io_size);

static struct attribute * h2f_loop_attrs[] = {
    &dev_attr_io_base.attr,
    &dev_attr_io_size.attr,
    NULL,
};

static const struct attribute_group h2f_loop_group = {
    .attrs = h2f_loop_attrs,
};

static const struct attribute_group * h2f_loop_groups[] = {
    &h2f_loop_group,
    NULL
};

static void
enyx_h2f_loop_device_destroy(struct device * dev)
{
    struct enyx_h2f_loop * h2f_loop = dev_get_drvdata(dev);

    cdev_del(&h2f_loop->cdev);

    kfree(h2f_loop);
}

int
enyx_h2f_loop_init(struct enyx_h2f_loop ** h2f_loop_out,
                   unsigned long io_base, size_t io_size,
                   int major, int minor,
                   struct device * parent,
                   struct class * h2f_loop_class)
{
    struct enyx_h2f_loop * h2f_loop;
    int err;

    dev_dbg(parent, "Creating h2f_loop [mem %lx-%lx] as dev %d:%d\n",
            io_base, io_base + io_size, major, minor);

    h2f_loop = *h2f_loop_out = kzalloc(sizeof(*h2f_loop), GFP_KERNEL);
    if (!h2f_loop) {
        dev_err(parent, "Can't allocate dev h2f_loop struct\n");
        err = -ENOMEM;
        goto err_drvdata_alloc;
    }

    sema_init(&h2f_loop->lock, 1);
    cdev_init(&h2f_loop->cdev, &enyx_h2f_loop_file_ops);
    h2f_loop->cdev.owner = THIS_MODULE;
    h2f_loop->io_base = io_base;
    h2f_loop->io_size = io_size;

    /* The device is available after this call */
    err = cdev_add(&h2f_loop->cdev, MKDEV(major, minor), 1);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't add the child h2f_loop char device %d:%d\n",
                major, minor);
        goto err_cdev_add;
    }

    h2f_loop->device.devt = h2f_loop->cdev.dev;
    h2f_loop->device.class = h2f_loop_class;
    h2f_loop->device.parent = parent;
    h2f_loop->device.groups = h2f_loop_groups;
    h2f_loop->device.release = enyx_h2f_loop_device_destroy;
    dev_set_drvdata(&h2f_loop->device, h2f_loop);
    err = dev_set_name(&h2f_loop->device, "h2f_loop%d", minor);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't register the child h2f_loop device\n");
        goto err_device_set_name;
    }

    err = device_register(&h2f_loop->device);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't register the child h2f_loop device\n");
        goto err_device_register;
    }

    dev_info(&h2f_loop->device, "Created\n");

    return 0;

err_device_register:
    /* The device resources are managed by the kobj
       hence destroy it and exit */
    put_device(&h2f_loop->device);
    return err;
err_device_set_name:
    cdev_del(&h2f_loop->cdev);
err_cdev_add:
    kfree(h2f_loop);
err_drvdata_alloc:
    return err;
}

void
enyx_h2f_loop_destroy(struct enyx_h2f_loop * h2f_loop)
{
    device_unregister(&h2f_loop->device);
}

