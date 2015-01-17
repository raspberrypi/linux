/*
 * Copyright (C) 2014-2015 Broadcom
 * Copyright (C) 2013 Red Hat
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/component.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "uapi/drm/vc4_drm.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

#define DRIVER_NAME "vc4"
#define DRIVER_DESC "Broadcom VC4 graphics"
#define DRIVER_DATE "20140616"
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

/*
 * Helper function for mapping the regs on a platform device.
 *
 * We assume only one register range per device, so we use index 0.
 */
void __iomem *
vc4_ioremap_regs(struct platform_device *dev, int index)
{
	struct resource *res;
	void __iomem *map;

	res = platform_get_resource(dev, IORESOURCE_MEM, index);
	map = devm_ioremap_resource(&dev->dev, res);
	if (IS_ERR(map)) {
		int ret = PTR_ERR(map);
		DRM_ERROR("Failed to map registers: %d\n", ret);
		return map;
	}

	return map;
}

static int
vc4_drm_load(struct drm_device *dev, unsigned long flags)
{
	struct platform_device *firmware_pdev;
	struct vc4_dev *vc4;
	int ret;

	vc4 = devm_kzalloc(dev->dev, sizeof(*vc4), GFP_KERNEL);
	if (!vc4)
		return -ENOMEM;

	vc4->firmware_node = of_parse_phandle(dev->dev->of_node, "firmware", 0);
	if (!vc4->firmware_node) {
		DRM_ERROR("Failed to parse firmware node.\n");
		return -EINVAL;
	}
	firmware_pdev = of_find_device_by_node(vc4->firmware_node);
	if (!platform_get_drvdata(firmware_pdev)) {
		DRM_DEBUG("firmware device not probed yet.\n");
		return -EPROBE_DEFER;
	}

	dev_set_drvdata(dev->dev, dev);
	vc4->dev = dev;
	dev->dev_private = vc4;

	drm_mode_config_init(dev);

	ret = component_bind_all(dev->dev, dev);
	if (ret)
		return ret;

	vc4_gem_init(dev);

	vc4_kms_load(dev);

	return 0;
}

static int vc4_drm_unload(struct drm_device *dev)
{
	drm_mode_config_cleanup(dev);

	component_unbind_all(dev->dev, dev);

	return 0;
}

static const struct file_operations vc4_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = vc4_mmap,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static const struct drm_ioctl_desc vc4_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VC4_SUBMIT_CL, vc4_submit_cl_ioctl, 0),
	DRM_IOCTL_DEF_DRV(VC4_WAIT_SEQNO, vc4_wait_seqno_ioctl, 0),
	DRM_IOCTL_DEF_DRV(VC4_WAIT_BO, vc4_wait_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(VC4_CREATE_BO, vc4_create_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(VC4_MMAP_BO, vc4_mmap_bo_ioctl, 0),
};


static struct drm_driver vc4_drm_driver = {
	.driver_features = (DRIVER_MODESET |
			    DRIVER_GEM |
			    DRIVER_HAVE_IRQ |
			    DRIVER_PRIME),
	.load = vc4_drm_load,
	.unload = vc4_drm_unload,
	.set_busid = drm_platform_set_busid,

	.irq_handler = vc4_irq,
	.irq_preinstall = vc4_irq_preinstall,
	.irq_postinstall = vc4_irq_postinstall,
	.irq_uninstall = vc4_irq_uninstall,

	.enable_vblank = vc4_enable_vblank,
	.disable_vblank = vc4_disable_vblank,
	.get_vblank_counter = drm_vblank_count,

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = vc4_debugfs_init,
	.debugfs_cleanup = vc4_debugfs_cleanup,
#endif

	.gem_free_object = vc4_free_object,
	.gem_vm_ops = &vc4_vm_ops,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = vc4_prime_import,
	.gem_prime_export = vc4_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,

	.dumb_create = vc4_dumb_create,
	.dumb_map_offset = drm_gem_cma_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.ioctls = vc4_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(vc4_drm_ioctls),
	.fops = &vc4_drm_fops,

	.gem_obj_size = sizeof(struct vc4_bo),

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static int vc4_drm_bind(struct device *dev)
{
	return drm_platform_init(&vc4_drm_driver, to_platform_device(dev));
}

static void vc4_drm_unbind(struct device *dev)
{
	drm_put_dev(platform_get_drvdata(to_platform_device(dev)));
}

static const struct component_master_ops vc4_drm_ops = {
	.bind = vc4_drm_bind,
	.unbind = vc4_drm_unbind,
};

/* NOTE: the CONFIG_OF case duplicates the same code as exynos or imx
 * (or probably any other).. so probably some room for some helpers
 */
static int compare_of(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static int add_components(struct device *dev, struct component_match **matchptr,
		const char *name)
{
	struct device_node *np = dev->of_node;
	unsigned i;

	for (i = 0; ; i++) {
		struct device_node *node;

		node = of_parse_phandle(np, name, i);
		if (!node)
			break;

		component_match_add(dev, matchptr, compare_of, node);
	}

	return 0;
}

static int
vc4_platform_drm_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;

	add_components(&pdev->dev, &match, "gpus");
	add_components(&pdev->dev, &match, "crtcs");
	add_components(&pdev->dev, &match, "encoders");
	add_components(&pdev->dev, &match, "hvss");

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	return component_master_add_with_match(&pdev->dev, &vc4_drm_ops, match);
}

static int
vc4_platform_drm_remove(struct platform_device *pdev)
{
	drm_put_dev(platform_get_drvdata(pdev));

	return 0;
}

static const struct of_device_id vc4_of_match[] = {
	{ .compatible = "brcm,vc4", },
	{},
};
MODULE_DEVICE_TABLE(of, vc4_of_match);

static struct platform_driver vc4_platform_driver = {
	.probe		= vc4_platform_drm_probe,
	.remove		= vc4_platform_drm_remove,
	.driver		= {
		.name	= "vc4-drm",
		.owner	= THIS_MODULE,
		.of_match_table = vc4_of_match,
	},
};

static int __init vc4_drm_register(void)
{
	vc4_v3d_register();
	vc4_hdmi_register();
	vc4_crtc_register();
	vc4_hvs_register();
	return platform_driver_register(&vc4_platform_driver);
}

static void __exit vc4_drm_unregister(void)
{
	platform_driver_unregister(&vc4_platform_driver);
	vc4_hvs_unregister();
	vc4_crtc_unregister();
	vc4_hdmi_unregister();
	vc4_v3d_unregister();
}

module_init(vc4_drm_register);
module_exit(vc4_drm_unregister);

MODULE_ALIAS("platform:vc4-drm");
MODULE_DESCRIPTION("Broadcom VC4 DRM Driver");
MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_LICENSE("GPL v2");
