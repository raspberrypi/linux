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
#define DRM_VC4_WAIT_SEQNO                        0x01
#define DRM_VC4_WAIT_BO                           0x02

#define DRM_IOCTL_VC4_SUBMIT_CL           DRM_IOWR( DRM_COMMAND_BASE + DRM_VC4_SUBMIT_CL, struct drm_vc4_submit_cl)
#define DRM_IOCTL_VC4_WAIT_SEQNO          DRM_IOWR( DRM_COMMAND_BASE + DRM_VC4_WAIT_SEQNO, struct drm_vc4_wait_seqno)
#define DRM_IOCTL_VC4_WAIT_BO             DRM_IOWR( DRM_COMMAND_BASE + DRM_VC4_WAIT_BO, struct drm_vc4_wait_bo)


/**
 * struct drm_vc4_submit_cl - ioctl argument for submitting commands to the 3D
 * engine.
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
	/* Pointer to the binner command list.
	 *
	 * This is the first set of commands executed, which runs the
	 * coordinate shader to determine where primitives land on the screen,
	 * then writes out the state updates and draw calls necessary per tile
	 * to the tile allocation BO.
	 */
	uint64_t bin_cl;

	/* Pointer to the render command list.
	 *
	 * The render command list contains a set of packets to load the
	 * current tile's state (reading from memory, or just clearing it)
	 * into the GPU, then call into the tile allocation BO to run the
	 * stored rendering for that tile, then store the tile's state back to
	 * memory.
	 */
	uint64_t render_cl;

	/* Pointer to the shader records.
	 *
	 * Shader records are the structures read by the hardware that contain
	 * pointers to uniforms, shaders, and vertex attributes.  The
	 * reference to the shader record has enough information to determine
	 * how many pointers are necessary (fixed number for shaders/uniforms,
	 * and an attribute count), so those BO indices into bo_handles are
	 * just stored as uint32_ts before each shader record passed in.
	 */
	uint64_t shader_rec;

	/* Pointer to uniform data and texture handles for the textures
	 * referenced by the shader.
	 *
	 * For each shader state record, there is a set of uniform data in the
	 * order referenced by the record (FS, VS, then CS).  Each set of
	 * uniform data has a uint32_t index into bo_handles per texture
	 * sample operation, in the order the QPU_W_TMUn_S writes appear in
	 * the program.  Following the texture BO handle indices is the actual
	 * uniform data.
	 *
	 * The individual uniform state blocks don't have sizes passed in,
	 * because the kernel has to determine the sizes anyway during shader
	 * code validation.
	 */
	uint64_t uniforms;
	uint64_t bo_handles;

	/* Size in bytes of the binner command list. */
	uint32_t bin_cl_size;
	/* Size in bytes of the render command list */
	uint32_t render_cl_size;
	/* Size in bytes of the set of shader records. */
	uint32_t shader_rec_size;
	/* Number of shader records.
	 *
	 * This could just be computed from the contents of shader_records and
	 * the address bits of references to them from the bin CL, but it
	 * keeps the kernel from having to resize some allocations it makes.
	 */
	uint32_t shader_rec_count;
	/* Size in bytes of the uniform state. */
	uint32_t uniforms_size;

	/* Number of BO handles passed in (size is that times 4). */
	uint32_t bo_handle_count;

	uint32_t flags;
	uint32_t pad;

	/* Returned value of the seqno of this render job (for the
	 * wait ioctl).
	 */
	uint64_t seqno;
};

/**
 * struct drm_vc4_wait_seqno - ioctl argument for waiting for
 * DRM_VC4_SUBMIT_CL completion using its returned seqno.
 *
 * timeout_ns is the timeout in nanoseconds, where "0" means "don't
 * block, just return the status."
 */
struct drm_vc4_wait_seqno {
	uint64_t seqno;
	uint64_t timeout_ns;
};

/**
 * struct drm_vc4_wait_bo - ioctl argument for waiting for
 * completion of the last DRM_VC4_SUBMIT_CL on a BO.
 *
 * This is useful for cases where multiple processes might be
 * rendering to a BO and you want to wait for all rendering to be
 * completed.
 */
struct drm_vc4_wait_bo {
	uint32_t handle;
	uint32_t pad;
	uint64_t timeout_ns;
};

#endif /* _UAPI_VC4_DRM_H_ */
