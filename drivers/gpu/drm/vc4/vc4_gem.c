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

#include "uapi/drm/vc4_drm.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

static void
thread_reset(struct drm_device *dev)
{
	DRM_INFO("Resetting threads\n");
	VC4_WRITE(V3D_CT0CS, V3D_CTRSTA);
	VC4_WRITE(V3D_CT1CS, V3D_CTRSTA);
	barrier();
}

static void
submit_cl(struct drm_device *dev, uint32_t thread, uint32_t start, uint32_t end)
{
	/* Stop any existing thread and set state to "stopped at halt" */
	VC4_WRITE(V3D_CTNCS(thread), V3D_CTRUN);
	barrier();

	VC4_WRITE(V3D_CTNCA(thread), start);
	barrier();

	/* Set the end address of the control list.  Writing this
	 * register is what starts the job.
	 */
	VC4_WRITE(V3D_CTNEA(thread), end);
	barrier();
}

static bool
thread_stopped(struct drm_device *dev, uint32_t thread)
{
	barrier();
	return !(VC4_READ(V3D_CTNCS(thread)) & V3D_CTRUN);
}

static int
wait_for_bin_thread(struct drm_device *dev)
{
	int i;

	for (i = 0; i < 1000000; i++) {
		if (thread_stopped(dev, 0)) {
			if (VC4_READ(V3D_PCS) & V3D_BMOOM) {
				/* XXX */
				DRM_ERROR("binner oom and stopped\n");
				return -EINVAL;
			}
			return 0;
		}

		if (VC4_READ(V3D_PCS) & V3D_BMOOM) {
			/* XXX */
			DRM_ERROR("binner oom\n");
			return -EINVAL;
		}
	}

	DRM_ERROR("timeout waiting for bin thread idle\n");
	return -EINVAL;
}

static int
wait_for_idle(struct drm_device *dev)
{
	int i;

	for (i = 0; i < 1000000; i++) {
		if (VC4_READ(V3D_PCS) == 0)
			return 0;
	}

	DRM_ERROR("timeout waiting for idle\n");
	return -EINVAL;
}

/*
static int
wait_for_render_thread(struct drm_device *dev, u32 initial_rfc)
{
	int i;

	for (i = 0; i < 1000000; i++) {
		if ((VC4_READ(V3D_RFC) & 0xff) == ((initial_rfc + 1) & 0xff))
			return 0;
	}

	DRM_ERROR("timeout waiting for render thread idle: "
		  "0x%08x start vs 0x%08x end\n",
		  initial_rfc, VC4_READ(V3D_RFC));
	return -EINVAL;
}
*/

static int
vc4_submit(struct drm_device *dev, struct drm_vc4_submit_cl *args)
{
	uint32_t initial_bfc, initial_rfc;
	uint32_t ct0ca = args->ct0ca, ct0ea = args->ct0ea; /* XXX */
	uint32_t ct1ca = args->ct1ca, ct1ea = args->ct1ea; /* XXX */
	int ret;

	/* flushes caches */
	VC4_WRITE(V3D_L2CACTL, 1 << 2);
	barrier();

	/* Disable the binner's pre-loaded overflow memory address */
	VC4_WRITE(V3D_BPOA, 0);
	VC4_WRITE(V3D_BPOS, 0);

	initial_bfc = VC4_READ(V3D_BFC);
	initial_rfc = VC4_READ(V3D_RFC);

	submit_cl(dev, 0, ct0ca, ct0ea);

	ret = wait_for_bin_thread(dev);
	if (ret)
		return ret;

	ret = wait_for_idle(dev);
	if (ret)
		return ret;

	WARN_ON(!thread_stopped(dev, 0));
	if (VC4_READ(V3D_CTNCS(0)) & V3D_CTERR) {
		DRM_ERROR("thread 0 stopped with error\n");
		return -EINVAL;
	}

	submit_cl(dev, 1, ct1ca, ct1ea);

	/* XXX: this errored out.  but wait_for_idle() seems like enough.
	ret = wait_for_render_thread(dev, initial_rfc);
	if (ret)
		return ret;
	*/
	ret = wait_for_idle(dev);
	if (ret)
		return ret;

	DRM_INFO("BFC 0x%02x -> 0x%02x\n",
		 initial_bfc, VC4_READ(V3D_BFC));

	return 0;
}

/**
 * Submits a command list to the VC4.
 *
 * This is what is called batchbuffer emitting on other hardware.
 */
int
vc4_submit_cl_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file_priv)
{
	struct drm_vc4_submit_cl *args = data;
	int ret;

	mutex_lock(&dev->struct_mutex);
	ret = vc4_submit(dev, args);
	if (ret)
		thread_reset(dev);
	mutex_unlock(&dev->struct_mutex);

	return ret;
}
