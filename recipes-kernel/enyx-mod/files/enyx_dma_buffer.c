#include "enyx_dma_buffer.h"

#include <linux/semaphore.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/dma-mapping.h>

struct enyx_dma_buffer {
    struct semaphore lock;

    struct cdev cdev;

    struct device device;
    dma_addr_t phys_addr;
    void * virt_addr;
    size_t size;
};

static int
enyx_dma_buffer_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct enyx_dma_buffer * dma = filp->private_data;
    const unsigned long vm_start = vma->vm_start,
                        vm_size = vma->vm_end - vma->vm_start;
    unsigned long pfn = virt_to_pfn(dma->virt_addr);
    unsigned long offset;
    int err;

    if (down_interruptible(&dma->lock))
        return -ERESTARTSYS;

    dev_dbg(&dma->device, "Mmaping %lu Bi\n", vm_size);

    if (vm_size > dma->size || vm_size % PAGE_SIZE != 0) {
        err = -EINVAL;
        goto err_invalid_arg;
    }

    for (offset = 0; offset != vm_size; offset += PAGE_SIZE)
        vm_insert_pfn(vma, vm_start + offset, pfn++);

err_invalid_arg:
    up(&dma->lock);

    return err;
}

static int
enyx_dma_buffer_open(struct inode *inode, struct file *filp)
{
    struct enyx_dma_buffer * dma = container_of(inode->i_cdev,
                                                   struct enyx_dma_buffer,
                                                   cdev);
                    int err = 0;

    filp->private_data = dma;

    if (down_interruptible(&dma->lock))
        return -ERESTARTSYS;

    dev_dbg(&dma->device, "Open\n");

    up(&dma->lock);

    return err;
}

static int
enyx_dma_buffer_close(struct inode * inode, struct file *filp)
{
    struct enyx_dma_buffer * dma = filp->private_data;

    if (down_interruptible(&dma->lock))
        return -ERESTARTSYS;

    dev_dbg(&dma->device, "Close\n");

    up(&dma->lock);

    return 0;
}

static const struct file_operations enyx_dma_buffer_file_ops = {
    .owner          = THIS_MODULE,
    .open           = enyx_dma_buffer_open,
    .release        = enyx_dma_buffer_close,
    .mmap           = enyx_dma_buffer_mmap
};

static ssize_t
phys_addr_show(struct device * device,
               struct device_attribute * attr,
               char * buf)
{
    struct enyx_dma_buffer * dma = dev_get_drvdata(device);
    return scnprintf(buf, PAGE_SIZE, "%pad\n", &dma->phys_addr);
}

static DEVICE_ATTR_RO(phys_addr);

static ssize_t
size_show(struct device * device,
          struct device_attribute * attr,
          char * buf)
{
    struct enyx_dma_buffer * dma = dev_get_drvdata(device);
    return scnprintf(buf, PAGE_SIZE, "%zu\n", dma->size);
}

static DEVICE_ATTR_RO(size);

static struct attribute * dma_attrs[] = {
    &dev_attr_phys_addr.attr,
    &dev_attr_size.attr,
    NULL,
};

static const struct attribute_group dma_group = {
    .attrs = dma_attrs,
};

static const struct attribute_group * dma_groups[] = {
    &dma_group,
    NULL
};

static void
enyx_dma_buffer_device_destroy(struct device * dev)
{
    struct enyx_dma_buffer * dma = dev_get_drvdata(dev);

    cdev_del(&dma->cdev);

    dma_unmap_single(dev->parent, dma->phys_addr, dma->size, DMA_FROM_DEVICE);
    kfree(dma->virt_addr);

    kfree(dma);
}

int
enyx_dma_buffer_init(struct enyx_dma_buffer ** dma_out,
              size_t page_count,
              int major, int minor,
              struct device * parent,
              struct class * device_class)
{
    struct enyx_dma_buffer * dma;
    int err;

    dev_dbg(parent, "Creating dma as dev %d:%d\n",
            major, minor);

    dma = *dma_out = kzalloc(sizeof(*dma), GFP_KERNEL);
    if (!dma) {
        dev_err(parent, "Can't allocate dev dma struct\n");
        err = -ENOMEM;
        goto err_drvdata_alloc;
    }

    sema_init(&dma->lock, 1);
    cdev_init(&dma->cdev, &enyx_dma_buffer_file_ops);
    dma->cdev.owner = THIS_MODULE;
    dma->size = page_count * PAGE_SIZE;

    dma->virt_addr = kmalloc(dma->size, GFP_KERNEL | GFP_DMA);
    if (! dma->virt_addr) {
        dev_err(parent, "Can't allocate DMA buffer\n");
        err = -ENOMEM;
        goto err_dma_buffer_alloc;
    }

    dma->phys_addr = dma_map_single(parent,
                                    dma->virt_addr,
                                    dma->size, DMA_FROM_DEVICE);
    err = dma_mapping_error(parent, dma->phys_addr);
    if (err) {
        dev_err(parent, "Can't create DMA mapping\n");
        goto err_dma_map_single;
    }

    /* The device is available after this call */
    err = cdev_add(&dma->cdev, MKDEV(major, minor), 1);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't add the child dma char device %d:%d\n",
                major, minor);
        goto err_cdev_add;
    }

    dma->device.devt = dma->cdev.dev;
    dma->device.class = device_class;
    dma->device.parent = parent;
    dma->device.groups = dma_groups;
    dma->device.release = enyx_dma_buffer_device_destroy;
    dev_set_drvdata(&dma->device, dma);
    err = dev_set_name(&dma->device, "dma%d", minor);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't register the child dma device\n");
        goto err_device_set_name;
    }

    err = device_register(&dma->device);
    if (IS_ERR_VALUE(err)) {
        dev_err(parent, "Can't register the child dma device\n");
        goto err_device_register;
    }

    dev_info(&dma->device, "Created\n");

    return 0;

err_device_register:
    /* The device resources are managed by the kobj
       hence destroy it and exit */
    put_device(&dma->device);
    return err;
err_device_set_name:
    cdev_del(&dma->cdev);
err_cdev_add:
    dma_unmap_single(parent, dma->phys_addr, dma->size, DMA_FROM_DEVICE);
err_dma_map_single:
    kfree(dma->virt_addr);
err_dma_buffer_alloc:
    kfree(dma);
err_drvdata_alloc:
    return err;
}

void
enyx_dma_buffer_destroy(struct enyx_dma_buffer * dma)
{
    device_unregister(&dma->device);
}

