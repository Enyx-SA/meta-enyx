#include "enyx_io_space.h"

#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/sysfs.h>

struct enyx_io_space {
    struct semaphore lock;

    struct cdev cdev;

    struct device device;
    unsigned long io_base;
    size_t io_size;
};

static int
enyx_io_space_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct enyx_io_space * io_space = filp->private_data;
    const unsigned long io_offset = vma->vm_pgoff << PAGE_SHIFT,
                        io_start = io_space->io_base + io_offset,
                        vm_size = vma->vm_end - vma->vm_start;
    int err;

    if (down_interruptible(&io_space->lock))
        return -ERESTARTSYS;

    dev_dbg(&io_space->device, "Mmaping 0x%lx into 0x%lx (%lu)\n",
            io_start, vma->vm_start, vm_size);

    if ((io_space->io_size - io_offset) < vm_size) {
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
    up(&io_space->lock);

    return err;
}

static int
enyx_io_space_open(struct inode *inode, struct file *filp)
{
    struct enyx_io_space * io_space = container_of(inode->i_cdev,
                                                   struct enyx_io_space,
                                                   cdev);
    int err = 0;

    filp->private_data = io_space;

    if (down_interruptible(&io_space->lock))
        return -ERESTARTSYS;

    dev_dbg(&io_space->device, "Open\n");

    up(&io_space->lock);

    return err;
}

static int
enyx_io_space_close(struct inode * inode, struct file *filp)
{
    struct enyx_io_space * io_space = filp->private_data;

    if (down_interruptible(&io_space->lock))
        return -ERESTARTSYS;

    dev_dbg(&io_space->device, "Close\n");

    up(&io_space->lock);

    return 0;
}

static const struct file_operations enyx_io_space_file_ops = {
    .owner          = THIS_MODULE,
    .open           = enyx_io_space_open,
    .release        = enyx_io_space_close,
    .mmap           = enyx_io_space_mmap
};

static ssize_t
io_base_show(struct device * device,
             struct device_attribute * attr,
             char * buf)
{
    struct enyx_io_space * io_space = dev_get_drvdata(device);
    return scnprintf(buf, PAGE_SIZE, "0x%lx\n", io_space->io_base);
}

static DEVICE_ATTR_RO(io_base);

static ssize_t
io_size_show(struct device * device,
             struct device_attribute * attr,
             char * buf)
{
    struct enyx_io_space * io_space = dev_get_drvdata(device);
    return scnprintf(buf, PAGE_SIZE, "%zu\n", io_space->io_size);
}

static DEVICE_ATTR_RO(io_size);

static struct attribute * io_space_attrs[] = {
    &dev_attr_io_base.attr,
    &dev_attr_io_size.attr,
    NULL,
};

static const struct attribute_group io_space_group = {
    .attrs = io_space_attrs,
};

static const struct attribute_group * io_space_groups[] = {
    &io_space_group,
    NULL
};

static void
enyx_io_space_device_destroy(struct device * dev)
{
    struct enyx_io_space * io_space = dev_get_drvdata(dev);

    cdev_del(&io_space->cdev);

    kfree(io_space);
}

int
enyx_io_space_init(struct enyx_io_space ** io_space_out,
                   unsigned long io_base, size_t io_size,
                   int major, int minor,
                   struct device * parent,
                   struct class * device_class)
{
    struct enyx_io_space * io_space;
    int err;

    dev_dbg(parent, "Creating io_space [mem %lx-%lx] as dev %d:%d\n",
            io_base, io_base + io_size, major, minor);

    io_space = *io_space_out = kzalloc(sizeof(*io_space), GFP_KERNEL);
    if (!io_space) {
        dev_err(parent, "Can't allocate dev io_space struct\n");
        err = -ENOMEM;
        goto err_drvdata_alloc;
    }

    sema_init(&io_space->lock, 1);
    cdev_init(&io_space->cdev, &enyx_io_space_file_ops);
    io_space->cdev.owner = THIS_MODULE;
    io_space->io_base = io_base;
    io_space->io_size = io_size;

    /* The device is available after this call */
    err = cdev_add(&io_space->cdev, MKDEV(major, minor), 1);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't add the child io_space char device %d:%d\n",
                major, minor);
        goto err_cdev_add;
    }

    io_space->device.devt = io_space->cdev.dev;
    io_space->device.class = device_class;
    io_space->device.parent = parent;
    io_space->device.groups = io_space_groups;
    io_space->device.release = enyx_io_space_device_destroy;
    dev_set_drvdata(&io_space->device, io_space);
    err = dev_set_name(&io_space->device, "io_space%d", minor);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't register the child io_space device\n");
        goto err_device_set_name;
    }

    err = device_register(&io_space->device);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't register the child io_space device\n");
        goto err_device_register;
    }

    dev_info(&io_space->device, "Created\n");

    return 0;

err_device_register:
    /* The device resources are managed by the kobj
       hence destroy it and exit */
    put_device(&io_space->device);
    return err;
err_device_set_name:
    cdev_del(&io_space->cdev);
err_cdev_add:
    kfree(io_space);
err_drvdata_alloc:
    return err;
}

void
enyx_io_space_destroy(struct enyx_io_space * io_space)
{
    device_unregister(&io_space->device);
}

