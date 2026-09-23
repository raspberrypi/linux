/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef __AMDGPU_DM_IRQ_H__
#define __AMDGPU_DM_IRQ_H__

#include "irq_types.h" /* DAL irq definitions */

/*
 * Display Manager IRQ-related interfaces (for use by DAL).
 */

/**
 * amdgpu_dm_irq_init - Initialize internal structures of 'amdgpu_dm_irq'.
 *
 * This function should be called exactly once - during DM initialization.
 *
 * Returns:
 *	0 - success
 *	non-zero - error
 */
int amdgpu_dm_irq_init(struct amdgpu_device *adev);

/**
 * amdgpu_dm_irq_fini - deallocate internal structures of 'amdgpu_dm_irq'.
 *
 * This function should be called exactly once - during DM destruction.
 *
 */
void amdgpu_dm_irq_fini(struct amdgpu_device *adev);

/**
 * amdgpu_dm_irq_register_interrupt - register irq handler for Display block.
 *
 * @adev: AMD DRM device
 * @int_params: parameters for the irq
 * @ih: pointer to the irq hander function
 * @handler_args: arguments which will be passed to ih
 *
 * Returns:
 * 	IRQ Handler Index on success.
 * 	NULL on failure.
 *
 * Cannot be called from an interrupt handler.
 */
void *amdgpu_dm_irq_register_interrupt(struct amdgpu_device *adev,
				       struct dc_interrupt_params *int_params,
				       void (*ih)(void *),
				       void *handler_args);

/**
 * amdgpu_dm_irq_unregister_interrupt - unregister handler which was registered
 *	by amdgpu_dm_irq_register_interrupt().
 *
 * @adev: AMD DRM device.
 * @ih_index: irq handler index which was returned by
 *	amdgpu_dm_irq_register_interrupt
 */
void amdgpu_dm_irq_unregister_interrupt(struct amdgpu_device *adev,
					enum dc_irq_source irq_source,
					void *ih_index);

/**
 * amdgpu_dm_irq_set - enable or disable a DC interrupt source.
 *
 * @adev: AMD DRM device
 * @src: DC interrupt source to toggle
 * @enable: true to enable the source, false to disable it
 *
 * DM-wide replacement for dc_interrupt_set(). As locking is DM's
 * responsibility, this is a thin wrapper serializes the underlying
 * read-modify-write against the other interrupt sources sharing HW control
 * registers with @src, so DM must never call dc_interrupt_set() directly.
 *
 * Returns: true if the source was toggled.
 */
bool amdgpu_dm_irq_set(struct amdgpu_device *adev, enum dc_irq_source src,
		       bool enable);

/**
 * amdgpu_dm_irq_ack - acknowledge a DC interrupt source.
 *
 * @adev: AMD DRM device
 * @src: DC interrupt source to acknowledge
 *
 * DM-wide replacement for dc_interrupt_ack(), serialized the same way as
 * amdgpu_dm_irq_set().
 */
void amdgpu_dm_irq_ack(struct amdgpu_device *adev, enum dc_irq_source src);

void amdgpu_dm_set_irq_funcs(struct amdgpu_device *adev);

void amdgpu_dm_outbox_init(struct amdgpu_device *adev);
void amdgpu_dm_hpd_init(struct amdgpu_device *adev);
void amdgpu_dm_hpd_fini(struct amdgpu_device *adev);

/**
 * amdgpu_dm_irq_suspend - disable ASIC interrupt during suspend.
 *
 */
void amdgpu_dm_irq_suspend(struct amdgpu_device *adev);

/**
 * amdgpu_dm_irq_resume_early - enable HPDRX ASIC interrupts during resume.
 * amdgpu_dm_irq_resume - enable ASIC interrupt during resume.
 *
 */
void amdgpu_dm_irq_resume_early(struct amdgpu_device *adev);
void amdgpu_dm_irq_resume_late(struct amdgpu_device *adev);

#endif /* __AMDGPU_DM_IRQ_H__ */
