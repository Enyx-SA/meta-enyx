#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "enyx_h2f_loop.h"

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
    struct enyx_h2f_loop * h2f_loop;
    int minor;
};

struct enyx_h2f_drvdata {
    struct enyx_char_device device[MAX_CHAR_DEVICES];
    resource_size_t phys_addr;
    resource_size_t size;
};

static struct class * enyx_h2f_loop_class;

static int
enyx_create_h2f_loop_device(struct platform_device * pdev)
{
    struct enyx_h2f_drvdata * drvdata = platform_get_drvdata(pdev);
    int err;

    /* Reserve minor prior device creation */
    drvdata->device[0].minor = find_first_zero_bit(enyx_minor_used,
                                                   sizeof(enyx_minor_used));
    set_bit(drvdata->device[0].minor, enyx_minor_used);


    /* Create device */
    err = enyx_h2f_loop_init(&drvdata->device[0].h2f_loop,
                             drvdata->phys_addr, drvdata->size,
                             MAJOR(enyx_first_chrdev), drvdata->device[0].minor,
                             &pdev->dev, enyx_h2f_loop_class);
    if (err)
        goto err_h2f_loop_init;

    return 0;

err_h2f_loop_init:
    clear_bit(drvdata->device[0].minor, enyx_minor_used);
    return err;
}

static int
enyx_create_devices(struct platform_device * pdev)
{
    return enyx_create_h2f_loop_device(pdev);
}

static void
enyx_destroy_char_device(struct platform_device * pdev)
{
    struct enyx_h2f_drvdata * drvdata = platform_get_drvdata(pdev);

    enyx_h2f_loop_destroy(drvdata->device[0].h2f_loop);
    clear_bit(drvdata->device[0].minor, enyx_minor_used);
}

static void
enyx_destroy_devices(struct platform_device * pdev)
{
    enyx_destroy_char_device(pdev);
}

static int
h2f_remove(struct platform_device *pdev)
{
    struct enyx_h2f_drvdata * drvdata = platform_get_drvdata(pdev);

    dev_dbg(&pdev->dev, "Removing\n");

    enyx_destroy_devices(pdev);

	release_mem_region(drvdata->phys_addr, drvdata->size);

    kfree(drvdata);

	return 0;
}

static int
h2f_probe(struct platform_device *pdev)
{
    struct enyx_h2f_drvdata * drvdata;
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

    enyx_h2f_loop_class = class_create(THIS_MODULE, "h2f_loop");
    if (IS_ERR(enyx_h2f_loop_class)) {
        err = PTR_ERR(enyx_h2f_loop_class);
        pr_err("Can't create sysfs 'h2f_loop' class\n");
        goto err_class_create;
    }

	if ((err = platform_driver_register(&h2f_platform_driver)) < 0) {
        pr_err("Can't register h2f platform driver\n");
        goto err_h2f_platform_register_driver;
    }

    return 0;

err_h2f_platform_register_driver:
    class_destroy(enyx_h2f_loop_class);
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

    class_destroy(enyx_h2f_loop_class);

    unregister_chrdev_region(enyx_first_chrdev, MAX_CHAR_DEVICES);
}

module_exit(enyx_exit_module);

