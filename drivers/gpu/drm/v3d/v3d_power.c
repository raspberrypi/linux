// SPDX-License-Identifier: GPL-2.0+
/* Copyright (C) 2026 Raspberry Pi */

#include <linux/clk.h>
#include <linux/reset.h>

#include <drm/drm_print.h>

#include "v3d_drv.h"
#include "v3d_regs.h"

static void
v3d_resume_sms(struct v3d_dev *v3d)
{
	if (v3d->ver < V3D_GEN_71)
		return;

	V3D_SMS_WRITE(V3D_SMS_TEE_CS, V3D_SMS_CLEAR_POWER_OFF);

	if (wait_for((V3D_GET_FIELD(V3D_SMS_READ(V3D_SMS_TEE_CS),
				    V3D_SMS_STATE) == V3D_SMS_IDLE), 100)) {
		drm_err(&v3d->drm, "Failed to power up SMS\n");
	}

	v3d_reset_sms(v3d);
}

static void
v3d_suspend_sms(struct v3d_dev *v3d)
{
	if (v3d->ver < V3D_GEN_71)
		return;

	V3D_SMS_WRITE(V3D_SMS_TEE_CS, V3D_SMS_POWER_OFF);

	if (wait_for((V3D_GET_FIELD(V3D_SMS_READ(V3D_SMS_TEE_CS),
				    V3D_SMS_STATE) == V3D_SMS_POWER_OFF_STATE), 100)) {
		drm_err(&v3d->drm, "Failed to power off SMS\n");
	}
}

int v3d_power_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);

	v3d_irq_disable(v3d);
	v3d_suspend_sms(v3d);

	if (v3d->reset)
		reset_control_assert(v3d->reset);

	clk_disable_unprepare(v3d->clk);

	return 0;
}

int v3d_power_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);
	int ret;

	ret = clk_prepare_enable(v3d->clk);
	if (ret)
		return ret;

	if (v3d->reset) {
		ret = reset_control_deassert(v3d->reset);
		if (ret)
			goto clk_disable;
	}

	v3d_resume_sms(v3d);
	v3d_mmu_set_page_table(v3d);
	v3d_irq_enable(v3d);

	return 0;

clk_disable:
	clk_disable_unprepare(v3d->clk);
	return ret;
}
