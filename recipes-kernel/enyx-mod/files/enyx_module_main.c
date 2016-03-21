#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "enyx_io_space.h"
#include "enyx_dma_buffer.h"

#define MODULE_NAME module_name(THIS_MODULE)

MODULE_AUTHOR("David KELLER <david.keller@enyx.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Enyx drivers module");
MODULE_VERSION("1.0");

enum { FIRST_MINOR };
enum { MAX_CHAR_DEVICES = 64 };

static dev_t enyx_first_chrdev;

static DECLARE_BITMAP(enyx_minor_used, MAX_CHAR_DEVICES);

struct enyx_char_device {
    union {
        struct enyx_io_space * io_space;
        struct enyx_dma_buffer * dma;
    } ptr;
    int minor;
};

struct enyx_drvdata {
    struct enyx_char_device device[MAX_CHAR_DEVICES];
    resource_size_t phys_addr;
    resource_size_t size;
};

static struct class * enyx_fpga_device_class;

static int
enyx_create_io_space_device(struct platform_device * pdev)
{
    struct enyx_drvdata * drvdata = platform_get_drvdata(pdev);
    int err;

    /* Reserve minor prior device creation */
    drvdata->device[0].minor = find_first_zero_bit(enyx_minor_used,
                                                   sizeof(enyx_minor_used));
    set_bit(drvdata->device[0].minor, enyx_minor_used);


    /* Create device */
    err = enyx_io_space_init(&drvdata->device[0].ptr.io_space,
                             drvdata->phys_addr, drvdata->size,
                             MAJOR(enyx_first_chrdev), drvdata->device[0].minor,
                             &pdev->dev, enyx_fpga_device_class);
    if (err)
        goto err_io_space_init;

    return 0;

err_io_space_init:
    clear_bit(drvdata->device[0].minor, enyx_minor_used);
    return err;
}

static void
enyx_destroy_io_space_device(struct platform_device * pdev)
{
    struct enyx_drvdata * drvdata = platform_get_drvdata(pdev);

    enyx_io_space_destroy(drvdata->device[0].ptr.io_space);
    clear_bit(drvdata->device[0].minor, enyx_minor_used);
}

static int
enyx_create_dma_device(struct platform_device * pdev)
{
    struct enyx_drvdata * drvdata = platform_get_drvdata(pdev);
    int err;

    /* Reserve minor prior device creation */
    drvdata->device[1].minor = find_first_zero_bit(enyx_minor_used,
                                                   sizeof(enyx_minor_used));
    set_bit(drvdata->device[1].minor, enyx_minor_used);


    /* Create device */
    err = enyx_dma_buffer_init(&drvdata->device[1].ptr.dma,
                        PAGE_SIZE,
                        MAJOR(enyx_first_chrdev), drvdata->device[1].minor,
                        &pdev->dev, enyx_fpga_device_class);
    if (err)
        goto err_io_space_init;

    return 0;

err_io_space_init:
    clear_bit(drvdata->device[1].minor, enyx_minor_used);
    return err;
}

static void
enyx_destroy_dma_device(struct platform_device * pdev)
{
    struct enyx_drvdata * drvdata = platform_get_drvdata(pdev);

    enyx_dma_buffer_destroy(drvdata->device[1].ptr.dma);
    clear_bit(drvdata->device[1].minor, enyx_minor_used);
}

static int
enyx_create_devices(struct platform_device * pdev)
{
    int err;

    err = enyx_create_io_space_device(pdev);
    if (err)
        goto err_enyx_create_io_space_device;

    err = enyx_create_dma_device(pdev);
    if (err)
        goto err_enyx_create_dma_device;

err_enyx_create_dma_device:
    enyx_destroy_io_space_device(pdev);
err_enyx_create_io_space_device:
    return err;
}

static void
enyx_destroy_char_device(struct platform_device * pdev)
{
    enyx_destroy_dma_device(pdev);
    enyx_destroy_io_space_device(pdev);
}

static void
enyx_destroy_devices(struct platform_device * pdev)
{
    enyx_destroy_char_device(pdev);
}

static int
h2f_remove(struct platform_device *pdev)
{
    struct enyx_drvdata * drvdata = platform_get_drvdata(pdev);

    dev_dbg(&pdev->dev, "Removing\n");

    enyx_destroy_devices(pdev);

	release_mem_region(drvdata->phys_addr, drvdata->size);

    kfree(drvdata);

	return 0;
}

static int
h2f_probe(struct platform_device *pdev)
{
    struct enyx_drvdata * drvdata;
	struct resource *r;
	int err;

    dev_dbg(&pdev->dev, "Probing\n");

    drvdata = kzalloc(sizeof(*drvdata), GFP_KERNEL);
    if (! drvdata) {
        dev_err(&pdev->dev, "Can't allocate h2f drvdata\n");
        goto err_kzalloc;
    }
    platform_set_drvdata(pdev, drvdata);

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (! r) {
        err = -EINVAL;
		goto err_platform_get_resource;
    }

	drvdata->phys_addr = r->start;
	drvdata->size = resource_size(r);

	/* reserve our memory region */
	if (request_mem_region(drvdata->phys_addr, drvdata->size,
                           "h2f_region") == NULL) {
        err = -EINVAL;
        goto err_request_mem_region;
    }

    enyx_create_devices(pdev);

	return 0;

err_request_mem_region:
err_platform_get_resource:
    kfree(drvdata);
err_kzalloc:
	return err;
}

static struct of_device_id h2f_driver_dt_ids[] = {
	{ .compatible = "altr,bridge-15.0" },
//	{ .compatible = "altr,lol-1.0" },
	{ /* end of table */ }
};

MODULE_DEVICE_TABLE(of, h2f_driver_dt_ids);

static struct platform_driver h2f_platform_driver = {
	.probe = h2f_probe,
	.remove = h2f_remove,
	.driver = {
        .name = "h2f",
        .owner = THIS_MODULE,
        .of_match_table = h2f_driver_dt_ids,
    },
};

static int __init
enyx_init_module(void)
{
    int err;

    pr_info("%s <support@enyx.com>\n", MODULE_NAME);

    if ((err = alloc_chrdev_region(&enyx_first_chrdev,
                                   FIRST_MINOR,
                                   MAX_CHAR_DEVICES,
                                   MODULE_NAME)) < 0)
        goto err_alloc_chrdev_region;

    enyx_fpga_device_class = class_create(THIS_MODULE, "fpga_device");
    if (IS_ERR(enyx_fpga_device_class)) {
        err = PTR_ERR(enyx_fpga_device_class);
        pr_err("Can't create sysfs 'fpga_device' class\n");
        goto err_class_create;
    }

	if ((err = platform_driver_register(&h2f_platform_driver)) < 0) {
        pr_err("Can't register h2f platform driver\n");
        goto err_h2f_platform_register_driver;
    }

    return 0;

err_h2f_platform_register_driver:
    class_destroy(enyx_fpga_device_class);
err_class_create:
    unregister_chrdev_region(enyx_first_chrdev, MAX_CHAR_DEVICES);
err_alloc_chrdev_region:
    return err;
}

module_init(enyx_init_module);

static void __exit
enyx_exit_module(void)
{
	platform_driver_unregister(&h2f_platform_driver);

    class_destroy(enyx_fpga_device_class);

    unregister_chrdev_region(enyx_first_chrdev, MAX_CHAR_DEVICES);
}

module_exit(enyx_exit_module);

