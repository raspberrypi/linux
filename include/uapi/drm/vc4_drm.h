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

#ifndef _UAPI_VC4_DRM_H_
#define _UAPI_VC4_DRM_H_

#include <drm/drm.h>

#define DRM_VC4_SUBMIT_CL                         0x00

#define DRM_IOCTL_VC4_SUBMIT_CL	   DRM_IOWR( DRM_COMMAND_BASE + DRM_VC4_SUBMIT_CL, struct drm_vc4_submit_cl)

/**
 * Structure for submitting commands to the 3D engine.
 *
 * Drivers typically use GPU BOs to store batchbuffers / command lists and
 * their associated state.  However, because the VC4 lacks an MMU, we have to
 * do validation of memory accesses by the GPU commands.  If we were to store
 * our commands in BOs, we'd need to do uncached readback from them to do the
 * validation process, which is too expensive.  Instead, userspace accumulates
 * commands and associated state in plain memory, then the kernel copies the
 * data to its own address space, and then validates and stores it in a GPU
 * BO.
 */
struct drm_vc4_submit_cl {
	/**
	 * Pointer to the binner command list.
	 *
	 * This is the first set of commands executed, which runs the
	 * coordinate shader to determine where primitives land on the screen,
	 * then writes out the state updates and draw calls necessary per tile
	 * to the tile allocation BO.
	 */
	void __user *bin_cl;

	/**
	 * Pointer to the render command list.
	 *
	 * The render command list contains a set of packets to load the
	 * current tile's state (reading from memory, or just clearing it)
	 * into the GPU, then call into the tile allocation BO to run the
	 * stored rendering for that tile, then store the tile's state back to
	 * memory.
	 */
	void __user *render_cl;

	/** Pointer to the shader records.
	 *
	 * Shader records are the structures read by the hardware that contain
	 * pointers to uniforms, shaders, and vertex attributes.  The
	 * reference to the shader record has enough information to determine
	 * how many pointers are necessary (fixed number for shaders/uniforms,
	 * and an attribute count), so those BO indices into bo_handles are
	 * just stored as uint32_ts before each shader record passed in.
	 */
	void __user *shader_records;
	void __user *bo_handles;

	/** Size in bytes of the binner command list. */
	uint32_t bin_cl_len;
	/** Size in bytes of the render command list */
	uint32_t render_cl_len;
	/** Size in bytes of the list of shader records. */
	uint32_t shader_record_len;
	/**
	 * Number of shader records.
	 *
	 * This could just be computed from the contents of shader_records,
	 * but it keeps the kernel from having to resize various allocations
	 * it makes.
	 */
	uint32_t shader_record_count;

	/** Number of BO handles passed in (size is that times 4). */
	uint32_t bo_handle_count;
};

#endif /* _UAPI_VC4_DRM_H_ */
