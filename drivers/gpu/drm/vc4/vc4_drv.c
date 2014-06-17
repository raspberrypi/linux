/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>
#include <mach/vcio.h>

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "drm_gem_cma_helper.h"

#define DRIVER_NAME "vc4"
#define DRIVER_DESC "Broadcom VC4 graphics"
#define DRIVER_DATE "20140616"
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

static int
set_platform_qpu_enable(bool on)
{
	struct {
		u32 size;
		u32 response;
		u32 tag_id;
		u32 send_buffer_size;
		u32 send_data_size;
		u32 enable;
		u32 end_tag;
	} msg;
	int ret;

	msg.size = sizeof(msg);
	msg.response = 0;
	msg.end_tag = 0;

	msg.tag_id = 0x30012;
	msg.send_buffer_size = 4;
	msg.send_data_size = 4;
	msg.enable = on;

	ret = bcm_mailbox_property(&msg, sizeof(msg));

        if (ret == 0 && msg.response == 0x80000000) {
		DRM_DEBUG("QPU %s\n", on ? "enabled" : "disabled");
		return 0;
	} else {
		DRM_ERROR("Failed to %s QPU\n", on ? "enable" : "disable");
		return 1;
	}
}

static int
map_regs(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;
	void __iomem *regs[1];
	struct resource *mem[1];

	for (i = 0; i <= 0; i++) {
		mem[i] = platform_get_resource(dev->platformdev,
					       IORESOURCE_MEM, i);
		regs[i] = devm_ioremap_resource(dev->dev, mem[i]);
		if (IS_ERR(regs[i])) {
			DRM_ERROR("Failed to map registers\n");
			return PTR_ERR(  regs[i]);
		}
	}

	vc4->vc4_regs = regs[0];

	return 0;
}

static int
vc4_drm_load(struct drm_device *dev, unsigned long flags)
{
	struct vc4_dev *vc4;
	int ret;

	vc4 = kzalloc(sizeof(*vc4), GFP_KERNEL);
	if (!vc4)
		return -ENOMEM;

	ret = dma_set_coherent_mask(dev->dev, DMA_BIT_MASK(32));
	if (ret) {
		kfree(vc4);
		return ret;
	}

	dev_set_drvdata(dev->dev, dev);
	dev->dev_private = vc4;

	set_platform_qpu_enable(true);

	ret = map_regs(dev);
	if (ret)
		goto fail;

	if (VC4_READ(V3D_IDENT0) != VC4_EXPECTED_IDENT0) {
		DRM_ERROR("V3D_IDENT0 read 0x%08x instead of 0x%08x\n",
			  VC4_READ(V3D_IDENT0), VC4_EXPECTED_IDENT0);
		goto fail;
	}

	vc4_modeset_init(dev);

	return 0;

fail:
	dev->dev_private = NULL;
	kfree(vc4);
	return -ENOMEM;
}

static int vc4_drm_unload(struct drm_device *dev)
{
	drm_mode_config_cleanup(dev);

	set_platform_qpu_enable(false);

	kfree(dev->dev_private);
	dev->dev_private = NULL;

	return 0;
}

static const struct file_operations vc4_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = drm_gem_cma_mmap,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static const struct drm_ioctl_desc vc4_drm_ioctls[] = {
};

static struct drm_driver vc4_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME,
	.load = vc4_drm_load,
	.unload = vc4_drm_unload,
	/*
	.open = vc4_drm_open,
	.preclose = vc4_drm_preclose,
	.lastclose = vc4_drm_lastclose,
	*/

	/*
	.get_vblank_counter = vc4_drm_get_vblank_counter,
	.enable_vblank = vc4_drm_enable_vblank,
	.disable_vblank = vc4_drm_disable_vblank,
	*/

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = vc4_debugfs_init,
	.debugfs_cleanup = vc4_debugfs_cleanup,
#endif

	.gem_free_object = drm_gem_cma_free_object,
	.gem_vm_ops = &drm_gem_cma_vm_ops,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = drm_gem_prime_import,
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_get_sg_table	= drm_gem_cma_prime_get_sg_table,
	.gem_prime_import_sg_table = drm_gem_cma_prime_import_sg_table,
	.gem_prime_vmap		= drm_gem_cma_prime_vmap,
	.gem_prime_vunmap	= drm_gem_cma_prime_vunmap,
	.gem_prime_mmap		= drm_gem_cma_prime_mmap,

	.dumb_create = drm_gem_cma_dumb_create,
	.dumb_map_offset = drm_gem_dumb_map_offset,
	.dumb_destroy = drm_gem_dumb_destroy,

	.ioctls = vc4_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(vc4_drm_ioctls),
	.fops = &vc4_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static int
vc4_platform_drm_probe(struct platform_device *pdev)
{
	return drm_platform_init(&vc4_drm_driver, pdev);
}

static int
vc4_platform_drm_remove(struct platform_device *pdev)
{
	drm_put_dev(platform_get_drvdata(pdev));

	return 0;
}

static struct platform_driver vc4_platform_drm_driver = {
	.probe		= vc4_platform_drm_probe,
	.remove		= vc4_platform_drm_remove,
	.driver		= {
		.name	= "vc4-drm",
		.owner	= THIS_MODULE,
	},
};

module_platform_driver(vc4_platform_drm_driver);

MODULE_ALIAS("platform:vc4-drm");
MODULE_DESCRIPTION("Broadcom VC4 DRM Driver");
MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_LICENSE("GPL v2");
