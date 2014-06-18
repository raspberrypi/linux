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

struct drm_vc4_submit_cl {
	void __user *bin_cl;
	void __user *render_cl;
	void __user *shader_records;
	void __user *bo_handles;
	uint32_t bin_cl_len;
	uint32_t render_cl_len;
	uint32_t shader_record_len;
	uint32_t shader_record_count;
	uint32_t bo_handle_count;
};

#endif /* _UAPI_VC4_DRM_H_ */
