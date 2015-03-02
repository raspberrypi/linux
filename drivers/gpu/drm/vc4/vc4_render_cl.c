/*
 * Copyright © 2014-2015 Broadcom
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

/**
 * DOC: Render command list generation
 *
 * In the VC4 driver, render command list generation is performed by the
 * kernel instead of userspace.  We do this because validating a
 * user-submitted command list is hard to get right and has high CPU overhead,
 * while the number of valid configurations for render command lists is
 * actually fairly low.
 */

#include "uapi/drm/vc4_drm.h"
#include "vc4_drv.h"
#include "vc4_packet.h"

struct vc4_rcl_setup {
	struct drm_gem_cma_object *color_read;
	struct drm_gem_cma_object *color_ms_write;
	struct drm_gem_cma_object *zs_read;
	struct drm_gem_cma_object *zs_write;

	struct drm_gem_cma_object *rcl;
	u32 next_offset;
};

static inline void rcl_u8(struct vc4_rcl_setup *setup, u8 val)
{
	*(u8 *)(setup->rcl->vaddr + setup->next_offset) = val;
	setup->next_offset += 1;
}

static inline void rcl_u16(struct vc4_rcl_setup *setup, u16 val)
{
	*(u16 *)(setup->rcl->vaddr + setup->next_offset) = val;
	setup->next_offset += 2;
}

static inline void rcl_u32(struct vc4_rcl_setup *setup, u32 val)
{
	*(u32 *)(setup->rcl->vaddr + setup->next_offset) = val;
	setup->next_offset += 4;
}


/*
 * Emits a no-op STORE_TILE_BUFFER_GENERAL.
 *
 * If we emit a PACKET_TILE_COORDINATES, it must be followed by a store of
 * some sort before another load is triggered.
 */
static void vc4_store_before_load(struct vc4_rcl_setup *setup)
{
	rcl_u8(setup, VC4_PACKET_STORE_TILE_BUFFER_GENERAL);
	rcl_u16(setup,
		VC4_SET_FIELD(VC4_LOADSTORE_TILE_BUFFER_NONE,
			      VC4_LOADSTORE_TILE_BUFFER_BUFFER) |
		VC4_STORE_TILE_BUFFER_DISABLE_COLOR_CLEAR |
		VC4_STORE_TILE_BUFFER_DISABLE_ZS_CLEAR |
		VC4_STORE_TILE_BUFFER_DISABLE_VG_MASK_CLEAR);
	rcl_u32(setup, 0); /* no address, since we're in None mode */
}

/*
 * Emits a PACKET_TILE_COORDINATES if one isn't already pending.
 *
 * The tile coordinates packet triggers a pending load if there is one, are
 * used for clipping during rendering, and determine where loads/stores happen
 * relative to their base address.
 */
static void vc4_tile_coordinates(struct vc4_rcl_setup *setup,
				 uint32_t x, uint32_t y)
{
	rcl_u8(setup, VC4_PACKET_TILE_COORDINATES);
	rcl_u8(setup, x);
	rcl_u8(setup, y);
}

static void emit_tile(struct vc4_exec_info *exec,
		      struct vc4_rcl_setup *setup,
		      uint8_t x, uint8_t y, bool first, bool last)
{
	struct drm_vc4_submit_cl *args = exec->args;
	bool has_bin = args->bin_cl_size != 0;

	/* Note that the load doesn't actually occur until the
	 * tile coords packet is processed, and only one load
	 * may be outstanding at a time.
	 */
	if (setup->color_read) {
		rcl_u8(setup, VC4_PACKET_LOAD_TILE_BUFFER_GENERAL);
		rcl_u16(setup, args->color_read.bits);
		rcl_u32(setup,
			setup->color_read->paddr + args->color_read.offset);
	}

	if (setup->zs_read) {
		if (setup->color_read) {
			/* Exec previous load. */
			vc4_tile_coordinates(setup, x, y);
			vc4_store_before_load(setup);
		}

		rcl_u8(setup, VC4_PACKET_LOAD_TILE_BUFFER_GENERAL);
		rcl_u16(setup, args->zs_read.bits);
		rcl_u32(setup, setup->zs_read->paddr + args->zs_read.offset);
	}

	/* Clipping depends on tile coordinates having been
	 * emitted, so we always need one here.
	 */
	vc4_tile_coordinates(setup, x, y);

	/* Wait for the binner before jumping to the first
	 * tile's lists.
	 */
	if (first && has_bin)
		rcl_u8(setup, VC4_PACKET_WAIT_ON_SEMAPHORE);

	if (has_bin) {
		rcl_u8(setup, VC4_PACKET_BRANCH_TO_SUB_LIST);
		rcl_u32(setup, (exec->tile_bo->paddr +
				exec->tile_alloc_offset +
				(y * exec->bin_tiles_x + x) * 32));
	}

	if (setup->zs_write) {
		rcl_u8(setup, VC4_PACKET_STORE_TILE_BUFFER_GENERAL);
		rcl_u16(setup, args->zs_write.bits |
			(setup->color_ms_write ?
			 VC4_STORE_TILE_BUFFER_DISABLE_COLOR_CLEAR : 0));
		rcl_u32(setup,
			(setup->zs_write->paddr + args->zs_write.offset) |
			((last && !setup->color_ms_write) ?
			 VC4_LOADSTORE_TILE_BUFFER_EOF : 0));
	}

	if (setup->color_ms_write) {
		if (setup->zs_write) {
			/* Reset after previous store */
			vc4_tile_coordinates(setup, x, y);
		}

		if (last)
			rcl_u8(setup, VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF);
		else
			rcl_u8(setup, VC4_PACKET_STORE_MS_TILE_BUFFER);
	}
}

static int vc4_create_rcl_bo(struct drm_device *dev, struct vc4_exec_info *exec,
			     struct vc4_rcl_setup *setup)
{
	struct drm_vc4_submit_cl *args = exec->args;
	bool has_bin = args->bin_cl_size != 0;
	uint8_t min_x_tile = args->min_x_tile;
	uint8_t min_y_tile = args->min_y_tile;
	uint8_t max_x_tile = args->max_x_tile;
	uint8_t max_y_tile = args->max_y_tile;
	uint8_t xtiles = max_x_tile - min_x_tile + 1;
	uint8_t ytiles = max_y_tile - min_y_tile + 1;
	uint8_t x, y;
	uint32_t size, loop_body_size;

	size = VC4_PACKET_TILE_RENDERING_MODE_CONFIG_SIZE;
	loop_body_size = VC4_PACKET_TILE_COORDINATES_SIZE;

	if (args->flags & VC4_SUBMIT_CL_USE_CLEAR_COLOR) {
		size += VC4_PACKET_CLEAR_COLORS_SIZE +
			VC4_PACKET_TILE_COORDINATES_SIZE +
			VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE;
	}

	if (setup->color_read) {
		loop_body_size += (VC4_PACKET_LOAD_TILE_BUFFER_GENERAL_SIZE);
	}
	if (setup->zs_read) {
		if (setup->color_read) {
			loop_body_size += VC4_PACKET_TILE_COORDINATES_SIZE;
			loop_body_size += VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE;
		}
		loop_body_size += VC4_PACKET_LOAD_TILE_BUFFER_GENERAL_SIZE;
	}

	if (has_bin) {
		size += VC4_PACKET_WAIT_ON_SEMAPHORE_SIZE;
		loop_body_size += VC4_PACKET_BRANCH_TO_SUB_LIST_SIZE;
	}

	if (setup->zs_write)
		loop_body_size += VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE;
	if (setup->color_ms_write) {
		if (setup->zs_write)
			loop_body_size += VC4_PACKET_TILE_COORDINATES_SIZE;
		loop_body_size += VC4_PACKET_STORE_MS_TILE_BUFFER_SIZE;
	}
	size += xtiles * ytiles * loop_body_size;

	setup->rcl = &vc4_bo_create(dev, size)->base;
	if (!setup->rcl)
		return -ENOMEM;
	list_add_tail(&to_vc4_bo(&setup->rcl->base)->unref_head,
		      &exec->unref_list);

	rcl_u8(setup, VC4_PACKET_TILE_RENDERING_MODE_CONFIG);
	rcl_u32(setup,
		(setup->color_ms_write ?
		 (setup->color_ms_write->paddr +
		  args->color_ms_write.offset) :
		 0));
	rcl_u16(setup, args->width);
	rcl_u16(setup, args->height);
	rcl_u16(setup, args->color_ms_write.bits);

	/* The tile buffer gets cleared when the previous tile is stored.  If
	 * the clear values changed between frames, then the tile buffer has
	 * stale clear values in it, so we have to do a store in None mode (no
	 * writes) so that we trigger the tile buffer clear.
	 */
	if (args->flags & VC4_SUBMIT_CL_USE_CLEAR_COLOR) {
		rcl_u8(setup, VC4_PACKET_CLEAR_COLORS);
		rcl_u32(setup, args->clear_color[0]);
		rcl_u32(setup, args->clear_color[1]);
		rcl_u32(setup, args->clear_z);
		rcl_u8(setup, args->clear_s);

		vc4_tile_coordinates(setup, 0, 0);

		rcl_u8(setup, VC4_PACKET_STORE_TILE_BUFFER_GENERAL);
		rcl_u16(setup, VC4_LOADSTORE_TILE_BUFFER_NONE);
		rcl_u32(setup, 0); /* no address, since we're in None mode */
	}

	for (y = min_y_tile; y <= max_y_tile; y++) {
		for (x = min_x_tile; x <= max_x_tile; x++) {
			bool first = (x == min_x_tile && y == min_y_tile);
			bool last = (x == max_x_tile && y == max_y_tile);
			emit_tile(exec, setup, x, y, first, last);
		}
	}

	BUG_ON(setup->next_offset != size);
	exec->ct1ca = setup->rcl->paddr;
	exec->ct1ea = setup->rcl->paddr + setup->next_offset;

	return 0;
}

static int vc4_rcl_surface_setup(struct vc4_exec_info *exec,
				 struct drm_gem_cma_object **obj,
				 struct drm_vc4_submit_rcl_surface *surf)
{
	uint8_t tiling = VC4_GET_FIELD(surf->bits,
				       VC4_LOADSTORE_TILE_BUFFER_TILING);
	uint8_t buffer = VC4_GET_FIELD(surf->bits,
				       VC4_LOADSTORE_TILE_BUFFER_BUFFER);
	uint8_t format = VC4_GET_FIELD(surf->bits,
				       VC4_LOADSTORE_TILE_BUFFER_FORMAT);
	int cpp;

	if (surf->pad != 0) {
		DRM_ERROR("Padding unset\n");
		return -EINVAL;
	}

	if (surf->hindex == ~0)
		return 0;

	if (!vc4_use_bo(exec, surf->hindex, VC4_MODE_RENDER, obj))
		return -EINVAL;

	if (surf->bits & ~(VC4_LOADSTORE_TILE_BUFFER_TILING_MASK |
			   VC4_LOADSTORE_TILE_BUFFER_BUFFER_MASK |
			   VC4_LOADSTORE_TILE_BUFFER_FORMAT_MASK)) {
		DRM_ERROR("Unknown bits in load/store: 0x%04x\n",
			  surf->bits);
		return -EINVAL;
	}

	if (tiling > VC4_TILING_FORMAT_LT) {
		DRM_ERROR("Bad tiling format\n");
		return -EINVAL;
	}

	if (buffer == VC4_LOADSTORE_TILE_BUFFER_ZS) {
		if (format != 0) {
			DRM_ERROR("No color format should be set for ZS\n");
			return -EINVAL;
		}
		cpp = 4;
	} else if (buffer == VC4_LOADSTORE_TILE_BUFFER_COLOR) {
		switch (format) {
		case VC4_LOADSTORE_TILE_BUFFER_BGR565:
		case VC4_LOADSTORE_TILE_BUFFER_BGR565_DITHER:
			cpp = 2;
			break;
		case VC4_LOADSTORE_TILE_BUFFER_RGBA8888:
			cpp = 4;
			break;
		default:
			DRM_ERROR("Bad tile buffer format\n");
			return -EINVAL;
		}
	} else {
		DRM_ERROR("Bad load/store buffer %d.\n", buffer);
		return -EINVAL;
	}

	if (surf->offset & 0xf) {
		DRM_ERROR("load/store buffer must be 16b aligned.\n");
		return -EINVAL;
	}

	if (!vc4_check_tex_size(exec, *obj, surf->offset, tiling,
				exec->args->width, exec->args->height, cpp)) {
		return -EINVAL;
	}

	return 0;
}

static int
vc4_rcl_ms_surface_setup(struct vc4_exec_info *exec,
			 struct drm_gem_cma_object **obj,
			 struct drm_vc4_submit_rcl_surface *surf)
{
	uint8_t tiling = VC4_GET_FIELD(surf->bits,
				       VC4_RENDER_CONFIG_MEMORY_FORMAT);
	uint8_t format = VC4_GET_FIELD(surf->bits,
				       VC4_RENDER_CONFIG_FORMAT);
	int cpp;

	if (surf->pad != 0) {
		DRM_ERROR("Padding unset\n");
		return -EINVAL;
	}

	if (surf->bits & ~(VC4_RENDER_CONFIG_MEMORY_FORMAT_MASK |
			   VC4_RENDER_CONFIG_FORMAT_MASK)) {
		DRM_ERROR("Unknown bits in render config: 0x%04x\n",
			  surf->bits);
		return -EINVAL;
	}

	if (surf->hindex == ~0)
		return 0;

	if (!vc4_use_bo(exec, surf->hindex, VC4_MODE_RENDER, obj))
		return -EINVAL;

	if (tiling > VC4_TILING_FORMAT_LT) {
		DRM_ERROR("Bad tiling format\n");
		return -EINVAL;
	}

	switch (format) {
	case VC4_RENDER_CONFIG_FORMAT_BGR565_DITHERED:
	case VC4_RENDER_CONFIG_FORMAT_BGR565:
		cpp = 2;
		break;
	case VC4_RENDER_CONFIG_FORMAT_RGBA8888:
		cpp = 4;
		break;
	default:
		DRM_ERROR("Bad tile buffer format\n");
		return -EINVAL;
	}

	if (!vc4_check_tex_size(exec, *obj, surf->offset, tiling,
				exec->args->width, exec->args->height, cpp)) {
		return -EINVAL;
	}

	return 0;
}

int vc4_get_rcl(struct drm_device *dev, struct vc4_exec_info *exec)
{
	struct vc4_rcl_setup setup = {0};
	struct drm_vc4_submit_cl *args = exec->args;
	bool has_bin = args->bin_cl_size != 0;
	int ret;

	if (args->min_x_tile > args->max_x_tile ||
	    args->min_y_tile > args->max_y_tile) {
		DRM_ERROR("Bad render tile set (%d,%d)-(%d,%d)\n",
			  args->min_x_tile, args->min_y_tile,
			  args->max_x_tile, args->max_y_tile);
		return -EINVAL;
	}

	if (has_bin &&
	    (args->max_x_tile > exec->bin_tiles_x ||
	     args->max_y_tile > exec->bin_tiles_y)) {
		DRM_ERROR("Render tiles (%d,%d) outside of bin config (%d,%d)\n",
			  args->max_x_tile, args->max_y_tile,
			  exec->bin_tiles_x, exec->bin_tiles_y);
		return -EINVAL;
	}

	ret = vc4_rcl_surface_setup(exec, &setup.color_read, &args->color_read);
	if (ret)
		return ret;

	ret = vc4_rcl_ms_surface_setup(exec, &setup.color_ms_write,
				       &args->color_ms_write);
	if (ret)
		return ret;

	ret = vc4_rcl_surface_setup(exec, &setup.zs_read, &args->zs_read);
	if (ret)
		return ret;

	ret = vc4_rcl_surface_setup(exec, &setup.zs_write, &args->zs_write);
	if (ret)
		return ret;

	/* We shouldn't even have the job submitted to us if there's no
	 * surface to write out.
	 */
	if (!setup.color_ms_write && !setup.zs_write) {
		DRM_ERROR("RCL requires color or Z/S write\n");
		return -EINVAL;
	}

	return vc4_create_rcl_bo(dev, exec, &setup);
}
