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

/**
 * Command list validator for VC4.
 *
 * The VC4 has no IOMMU between it and system memory.  So, a user with
 * access to execute command lists could escalate privilege by
 * overwriting system memory (drawing to it as a framebuffer) or
 * reading system memory it shouldn't (reading it as a texture, or
 * uniform data, or vertex data).
 *
 * This validates command lists to ensure that all accesses are within
 * the bounds of the GEM objects referenced.  It explicitly whitelists
 * packets, and looks at the offsets in any address fields to make
 * sure they're constrained within the BOs they reference.
 *
 * Note that because of the validation that's happening anyway, this
 * is where GEM relocation processing happens.
 */

#include "vc4_drv.h"
#include "vc4_packet.h"

#define VALIDATE_ARGS \
	struct exec_info *exec,				\
	void *validated,				\
	void *untrusted


/** Return the width in pixels of a 64-byte microtile. */
static uint32_t
utile_width(int cpp)
{
	switch (cpp) {
	case 1:
	case 2:
		return 8;
	case 4:
		return 4;
	case 8:
		return 2;
	default:
		DRM_ERROR("unknown cpp: %d\n", cpp);
		return 1;
	}
}

/** Return the height in pixels of a 64-byte microtile. */
static uint32_t
utile_height(int cpp)
{
	switch (cpp) {
	case 1:
		return 8;
	case 2:
	case 4:
	case 8:
		return 4;
	default:
		DRM_ERROR("unknown cpp: %d\n", cpp);
		return 1;
	}
}

/**
 * The texture unit decides what tiling format a particular miplevel is using
 * this function, so we lay out our miptrees accordingly.
 */
static bool
size_is_lt(uint32_t width, uint32_t height, int cpp)
{
	return (width <= 4 * utile_width(cpp) ||
		height <= 4 * utile_height(cpp));
}

static bool
vc4_use_bo(struct exec_info *exec,
	   uint32_t hindex,
	   enum vc4_bo_mode mode,
	   struct drm_gem_cma_object **obj)
{
	*obj = NULL;

	if (hindex >= exec->bo_count) {
		DRM_ERROR("BO index %d greater than BO count %d\n",
			  hindex, exec->bo_count);
		return false;
	}

	if (exec->bo[hindex].mode != mode) {
		if (exec->bo[hindex].mode == VC4_MODE_UNDECIDED) {
			exec->bo[hindex].mode = mode;
		} else {
			DRM_ERROR("BO index %d reused with mode %d vs %d\n",
				  hindex, exec->bo[hindex].mode, mode);
			return false;
		}
	}

	*obj = exec->bo[hindex].bo;
	return true;
}

static bool
vc4_use_handle(struct exec_info *exec,
	       uint32_t gem_handles_packet_index,
	       enum vc4_bo_mode mode,
	       struct drm_gem_cma_object **obj)
{
	return vc4_use_bo(exec, exec->bo_index[gem_handles_packet_index],
			  mode, obj);
}

static uint32_t
gl_shader_rec_size(uint32_t pointer_bits)
{
	uint32_t attribute_count = pointer_bits & 7;
	bool extended = pointer_bits & 8;

	if (attribute_count == 0)
		attribute_count = 8;

	if (extended)
		return 100 + attribute_count * 4;
	else
		return 36 + attribute_count * 8;
}

static bool
check_tex_size(struct exec_info *exec, struct drm_gem_cma_object *fbo,
	       uint32_t offset, uint8_t tiling_format,
	       uint32_t width, uint32_t height, uint8_t cpp)
{
	uint32_t aligned_width, aligned_height, stride, size;
	uint32_t utile_w = utile_width(cpp);
	uint32_t utile_h = utile_height(cpp);

	/* The values are limited by the packet/texture parameter bitfields,
	 * so we don't need to worry as much about integer overflow.
	 */
	BUG_ON(width > 65535);
	BUG_ON(height > 65535);

	switch (tiling_format) {
	case VC4_TILING_FORMAT_LINEAR:
		aligned_width = roundup(width, 16 / cpp);
		aligned_height = height;
		break;
	case VC4_TILING_FORMAT_T:
		aligned_width = roundup(width, utile_w * 8);
		aligned_height = roundup(height, utile_h * 8);
		break;
	case VC4_TILING_FORMAT_LT:
		aligned_width = roundup(width, utile_w);
		aligned_height = roundup(height, utile_h);
		break;
	default:
		DRM_ERROR("buffer tiling %d unsupported\n", tiling_format);
		return false;
	}

	stride = aligned_width * cpp;

	if (INT_MAX / stride < aligned_height) {
		DRM_ERROR("Overflow in fbo size (%dx%d -> %dx%d)\n",
			  width, height,
			  aligned_width, aligned_height);
		return false;
	}
	size = stride * aligned_height;

	if (size + offset < size ||
	    size + offset > fbo->base.size) {
		DRM_ERROR("Overflow in %dx%d (%dx%d) fbo size (%d + %d > %d)\n",
			  width, height,
			  aligned_width, aligned_height,
			  size, offset, fbo->base.size);
		return false;
	}

	return true;
}

static int
validate_start_tile_binning(VALIDATE_ARGS)
{
	if (exec->found_start_tile_binning_packet) {
		DRM_ERROR("Duplicate VC4_PACKET_START_TILE_BINNING\n");
		return -EINVAL;
	}
	exec->found_start_tile_binning_packet = true;

	if (!exec->found_tile_binning_mode_config_packet) {
		DRM_ERROR("missing VC4_PACKET_TILE_BINNING_MODE_CONFIG\n");
		return -EINVAL;
	}

	return 0;
}

static int
validate_branch_to_sublist(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *target;
	uint32_t offset;

	if (!vc4_use_handle(exec, 0, VC4_MODE_TILE_ALLOC, &target))
		return -EINVAL;

	if (target != exec->tile_alloc_bo) {
		DRM_ERROR("Jumping to BOs other than tile alloc unsupported\n");
		return -EINVAL;
	}

	offset = *(uint32_t *)(untrusted + 0);
	if (offset % exec->tile_alloc_init_block_size ||
	    offset / exec->tile_alloc_init_block_size >
	    exec->bin_tiles_x * exec->bin_tiles_y) {
		DRM_ERROR("VC4_PACKET_BRANCH_TO_SUB_LIST must jump to initial "
			  "tile allocation space.\n");
		return -EINVAL;
	}

	*(uint32_t *)(validated + 0) = target->paddr + offset;

	return 0;
}

/**
 * validate_loadstore_tile_buffer_general() - Validation for
 * VC4_PACKET_LOAD_TILE_BUFFER_GENERAL and
 * VC4_PACKET_STORE_TILE_BUFFER_GENERAL.
 *
 * The two packets are nearly the same, except for the TLB-clearing management
 * bits not being present for loads.  Additionally, while stores are executed
 * immediately (using the current tile coordinates), loads are queued to be
 * executed when the tile coordinates packet occurs.
 *
 * Note that coordinates packets are validated to be within the declared
 * bin_x/y, which themselves are verified to match the rendering-configuration
 * FB width and height (which the hardware uses to clip loads and stores).
 */
static int
validate_loadstore_tile_buffer_general(VALIDATE_ARGS)
{
	uint32_t packet_b0 = *(uint8_t *)(untrusted + 0);
	uint32_t packet_b1 = *(uint8_t *)(untrusted + 1);
	struct drm_gem_cma_object *fbo;
	uint32_t buffer_type = packet_b0 & 0xf;
	uint32_t offset, cpp;

	switch (buffer_type) {
	case VC4_LOADSTORE_TILE_BUFFER_NONE:
		return 0;
	case VC4_LOADSTORE_TILE_BUFFER_COLOR:
		if ((packet_b1 & VC4_LOADSTORE_TILE_BUFFER_MASK) ==
		    VC4_LOADSTORE_TILE_BUFFER_RGBA8888) {
			cpp = 4;
		} else {
			cpp = 2;
		}
		break;

	case VC4_LOADSTORE_TILE_BUFFER_Z:
	case VC4_LOADSTORE_TILE_BUFFER_ZS:
		cpp = 4;
		break;

	default:
		DRM_ERROR("Load/store type %d unsupported\n", buffer_type);
		return -EINVAL;
	}

	if (!vc4_use_handle(exec, 0, VC4_MODE_RENDER, &fbo))
		return -EINVAL;

	offset = *(uint32_t *)(untrusted + 2);

	if (!check_tex_size(exec, fbo, offset,
			    ((packet_b0 &
			      VC4_LOADSTORE_TILE_BUFFER_FORMAT_MASK) >>
			     VC4_LOADSTORE_TILE_BUFFER_FORMAT_SHIFT),
			    exec->fb_width, exec->fb_height, cpp)) {
		return -EINVAL;
	}

	*(uint32_t *)(validated + 2) = offset + fbo->paddr;

	return 0;
}

static int
validate_indexed_prim_list(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *ib;
	uint32_t length = *(uint32_t *)(untrusted + 1);
	uint32_t offset = *(uint32_t *)(untrusted + 5);
	uint32_t max_index = *(uint32_t *)(untrusted + 9);
	uint32_t index_size = (*(uint8_t *)(untrusted + 0) >> 4) ? 2 : 1;
	struct vc4_shader_state *shader_state;

	/* Check overflow condition */
	if (exec->shader_state_count == 0) {
		DRM_ERROR("shader state must precede primitives\n");
		return -EINVAL;
	}
	shader_state = &exec->shader_state[exec->shader_state_count - 1];

	if (max_index > shader_state->max_index)
		shader_state->max_index = max_index;

	if (!vc4_use_handle(exec, 0, VC4_MODE_RENDER, &ib))
		return -EINVAL;

	if (offset > ib->base.size ||
	    (ib->base.size - offset) / index_size < length) {
		DRM_ERROR("IB access overflow (%d + %d*%d > %d)\n",
			  offset, length, index_size, ib->base.size);
		return -EINVAL;
	}

	*(uint32_t *)(validated + 5) = ib->paddr + offset;

	return 0;
}

static int
validate_gl_array_primitive(VALIDATE_ARGS)
{
	uint32_t length = *(uint32_t *)(untrusted + 1);
	uint32_t base_index = *(uint32_t *)(untrusted + 5);
	uint32_t max_index;
	struct vc4_shader_state *shader_state;

	/* Check overflow condition */
	if (exec->shader_state_count == 0) {
		DRM_ERROR("shader state must precede primitives\n");
		return -EINVAL;
	}
	shader_state = &exec->shader_state[exec->shader_state_count - 1];

	if (length + base_index < length) {
		DRM_ERROR("primitive vertex count overflow\n");
		return -EINVAL;
	}
	max_index = length + base_index - 1;

	if (max_index > shader_state->max_index)
		shader_state->max_index = max_index;

	return 0;
}

static int
validate_gl_shader_state(VALIDATE_ARGS)
{
	uint32_t i = exec->shader_state_count++;

	if (i >= exec->shader_state_size) {
		DRM_ERROR("More requests for shader states than declared\n");
		return -EINVAL;
	}

	exec->shader_state[i].packet = VC4_PACKET_GL_SHADER_STATE;
	exec->shader_state[i].addr = *(uint32_t *)untrusted;
	exec->shader_state[i].max_index = 0;

	if (exec->shader_state[i].addr & ~0xf) {
		DRM_ERROR("high bits set in GL shader rec reference\n");
		return -EINVAL;
	}

	*(uint32_t *)validated = (exec->shader_rec_p +
				  exec->shader_state[i].addr);

	exec->shader_rec_p +=
		roundup(gl_shader_rec_size(exec->shader_state[i].addr), 16);

	return 0;
}

static int
validate_nv_shader_state(VALIDATE_ARGS)
{
	uint32_t i = exec->shader_state_count++;

	if (i >= exec->shader_state_size) {
		DRM_ERROR("More requests for shader states than declared\n");
		return -EINVAL;
	}

	exec->shader_state[i].packet = VC4_PACKET_NV_SHADER_STATE;
	exec->shader_state[i].addr = *(uint32_t *)untrusted;

	if (exec->shader_state[i].addr & 15) {
		DRM_ERROR("NV shader state address 0x%08x misaligned\n",
			  exec->shader_state[i].addr);
		return -EINVAL;
	}

	*(uint32_t *)validated = (exec->shader_state[i].addr +
				  exec->shader_rec_p);

	return 0;
}

static int
validate_tile_binning_config(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *tile_allocation;
	struct drm_gem_cma_object *tile_state_data_array;
	uint8_t flags;
	uint32_t tile_allocation_size;

	if (!vc4_use_handle(exec, 0, VC4_MODE_TILE_ALLOC, &tile_allocation) ||
	    !vc4_use_handle(exec, 1, VC4_MODE_TSDA, &tile_state_data_array))
		return -EINVAL;

	if (exec->found_tile_binning_mode_config_packet) {
		DRM_ERROR("Duplicate VC4_PACKET_TILE_BINNING_MODE_CONFIG\n");
		return -EINVAL;
	}
	exec->found_tile_binning_mode_config_packet = true;

	exec->bin_tiles_x = *(uint8_t *)(untrusted + 12);
	exec->bin_tiles_y = *(uint8_t *)(untrusted + 13);
	flags = *(uint8_t *)(untrusted + 14);

	if (exec->bin_tiles_x == 0 ||
	    exec->bin_tiles_y == 0) {
		DRM_ERROR("Tile binning config of %dx%d too small\n",
			  exec->bin_tiles_x, exec->bin_tiles_y);
		return -EINVAL;
	}

	/* Our validation relies on the user not getting to set up their own
	 * tile state/tile allocation BO contents.
	 */
	if (!(flags & VC4_BIN_CONFIG_AUTO_INIT_TSDA)) {
		DRM_ERROR("binning config missing "
			  "VC4_BIN_CONFIG_AUTO_INIT_TSDA\n");
		return -EINVAL;
	}

	if (flags & (VC4_BIN_CONFIG_DB_NON_MS |
		     VC4_BIN_CONFIG_TILE_BUFFER_64BIT |
		     VC4_BIN_CONFIG_MS_MODE_4X)) {
		DRM_ERROR("unsupported bining config flags 0x%02x\n", flags);
		return -EINVAL;
	}

	if (*(uint32_t *)(untrusted + 0) != 0) {
		DRM_ERROR("tile allocation offset != 0 unsupported\n");
		return -EINVAL;
	}
	tile_allocation_size = *(uint32_t *)(untrusted + 4);
	if (tile_allocation_size > tile_allocation->base.size) {
		DRM_ERROR("tile allocation size %d > BO size %d\n",
			  tile_allocation_size, tile_allocation->base.size);
		return -EINVAL;
	}
	*(uint32_t *)validated = tile_allocation->paddr;
	exec->tile_alloc_bo = tile_allocation;

	exec->tile_alloc_init_block_size = 1 << (5 + ((flags >> 5) & 3));
	if (exec->bin_tiles_x * exec->bin_tiles_y *
	    exec->tile_alloc_init_block_size > tile_allocation_size) {
		DRM_ERROR("tile init exceeds tile alloc size (%d vs %d)\n",
			  exec->bin_tiles_x * exec->bin_tiles_y *
			  exec->tile_alloc_init_block_size,
			  tile_allocation_size);
		return -EINVAL;
	}
	if (*(uint32_t *)(untrusted + 8) != 0) {
		DRM_ERROR("TSDA offset != 0 unsupported\n");
		return -EINVAL;
	}
	if (exec->bin_tiles_x * exec->bin_tiles_y * 48 >
	    tile_state_data_array->base.size) {
		DRM_ERROR("TSDA of %db too small for %dx%d bin config\n",
			  tile_state_data_array->base.size,
			  exec->bin_tiles_x, exec->bin_tiles_y);
	}
	*(uint32_t *)(validated + 8) = tile_state_data_array->paddr;

	return 0;
}

static int
validate_tile_rendering_mode_config(VALIDATE_ARGS)
{
	struct drm_gem_cma_object *fbo;
	uint32_t flags, offset, cpp;

	if (exec->found_tile_rendering_mode_config_packet) {
		DRM_ERROR("Duplicate VC4_PACKET_TILE_RENDERING_MODE_CONFIG\n");
		return -EINVAL;
	}
	exec->found_tile_rendering_mode_config_packet = true;

	if (!vc4_use_handle(exec, 0, VC4_MODE_RENDER, &fbo))
		return -EINVAL;

	exec->fb_width = *(uint16_t *)(untrusted + 4);
	exec->fb_height = *(uint16_t *)(untrusted + 6);

	/* Make sure that the fb width/height matches the binning config -- we
	 * rely on being able to interchange these for various assertions.
	 * (Within a tile, loads and stores will be clipped to the
	 * width/height, but we allow load/storing to any binned tile).
	 */
	if (exec->fb_width <= (exec->bin_tiles_x - 1) * 64 ||
	    exec->fb_width > exec->bin_tiles_x * 64 ||
	    exec->fb_height <= (exec->bin_tiles_y - 1) * 64 ||
	    exec->fb_height > exec->bin_tiles_y * 64) {
		DRM_ERROR("bin config %dx%d doesn't match FB %dx%d\n",
			  exec->bin_tiles_x, exec->bin_tiles_y,
			  exec->fb_width, exec->fb_height);
		return -EINVAL;
	}

	flags = *(uint16_t *)(untrusted + 8);
	if ((flags & VC4_RENDER_CONFIG_FORMAT_MASK) ==
	    VC4_RENDER_CONFIG_FORMAT_RGBA8888) {
		cpp = 4;
	} else {
		cpp = 2;
	}

	offset = *(uint32_t *)untrusted;
	if (!check_tex_size(exec, fbo, offset,
			    ((flags &
			      VC4_RENDER_CONFIG_MEMORY_FORMAT_MASK) >>
			     VC4_RENDER_CONFIG_MEMORY_FORMAT_SHIFT),
			    exec->fb_width, exec->fb_height, cpp)) {
		return -EINVAL;
	}

	*(uint32_t *)validated = fbo->paddr + offset;

	return 0;
}

static int
validate_tile_coordinates(VALIDATE_ARGS)
{
	uint8_t tile_x = *(uint8_t *)(untrusted + 0);
	uint8_t tile_y = *(uint8_t *)(untrusted + 1);

	if (tile_x >= exec->bin_tiles_x ||
	    tile_y >= exec->bin_tiles_y) {
		DRM_ERROR("Tile coordinates %d,%d > bin config %d,%d\n",
			  tile_x,
			  tile_y,
			  exec->bin_tiles_x,
			  exec->bin_tiles_y);
		return -EINVAL;
	}

	return 0;
}

static int
validate_gem_handles(VALIDATE_ARGS)
{
	memcpy(exec->bo_index, untrusted, sizeof(exec->bo_index));
	return 0;
}

static const struct cmd_info {
	bool bin;
	bool render;
	uint16_t len;
	const char *name;
	int (*func)(struct exec_info *exec, void *validated, void *untrusted);
} cmd_info[] = {
	[VC4_PACKET_HALT] = { 1, 1, 1, "halt", NULL },
	[VC4_PACKET_NOP] = { 1, 1, 1, "nop", NULL },
	[VC4_PACKET_FLUSH] = { 1, 1, 1, "flush", NULL },
	[VC4_PACKET_FLUSH_ALL] = { 1, 0, 1, "flush all state", NULL },
	[VC4_PACKET_START_TILE_BINNING] = { 1, 0, 1, "start tile binning", validate_start_tile_binning },
	[VC4_PACKET_INCREMENT_SEMAPHORE] = { 1, 0, 1, "increment semaphore", NULL },
	[VC4_PACKET_WAIT_ON_SEMAPHORE] = { 1, 1, 1, "wait on semaphore", NULL },
	/* BRANCH_TO_SUB_LIST is actually supported in the binner as well, but
	 * we only use it from the render CL in order to jump into the tile
	 * allocation BO.
	 */
	[VC4_PACKET_BRANCH_TO_SUB_LIST] = { 0, 1, 5, "branch to sublist", validate_branch_to_sublist },
	[VC4_PACKET_STORE_MS_TILE_BUFFER] = { 0, 1, 1, "store MS resolved tile color buffer", NULL },
	[VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF] = { 0, 1, 1, "store MS resolved tile color buffer and EOF", NULL },

	[VC4_PACKET_STORE_TILE_BUFFER_GENERAL] = { 0, 1, 7, "Store Tile Buffer General", validate_loadstore_tile_buffer_general },
	[VC4_PACKET_LOAD_TILE_BUFFER_GENERAL] = { 0, 1, 7, "Load Tile Buffer General", validate_loadstore_tile_buffer_general },

	[VC4_PACKET_GL_INDEXED_PRIMITIVE] = { 1, 1, 14, "Indexed Primitive List", validate_indexed_prim_list },

	[VC4_PACKET_GL_ARRAY_PRIMITIVE] = { 1, 1, 10, "Vertex Array Primitives", validate_gl_array_primitive },

	/* This is only used by clipped primitives (packets 48 and 49), which
	 * we don't support parsing yet.
	 */
	[VC4_PACKET_PRIMITIVE_LIST_FORMAT] = { 1, 1, 2, "primitive list format", NULL },

	[VC4_PACKET_GL_SHADER_STATE] = { 1, 1, 5, "GL Shader State", validate_gl_shader_state },
	[VC4_PACKET_NV_SHADER_STATE] = { 1, 1, 5, "NV Shader State", validate_nv_shader_state },

	[VC4_PACKET_CONFIGURATION_BITS] = { 1, 1, 4, "configuration bits", NULL },
	[VC4_PACKET_FLAT_SHADE_FLAGS] = { 1, 1, 5, "flat shade flags", NULL },
	[VC4_PACKET_POINT_SIZE] = { 1, 1, 5, "point size", NULL },
	[VC4_PACKET_LINE_WIDTH] = { 1, 1, 5, "line width", NULL },
	[VC4_PACKET_RHT_X_BOUNDARY] = { 1, 1, 3, "RHT X boundary", NULL },
	[VC4_PACKET_DEPTH_OFFSET] = { 1, 1, 5, "Depth Offset", NULL },
	[VC4_PACKET_CLIP_WINDOW] = { 1, 1, 9, "Clip Window", NULL },
	[VC4_PACKET_VIEWPORT_OFFSET] = { 1, 1, 5, "Viewport Offset", NULL },
	[VC4_PACKET_CLIPPER_XY_SCALING] = { 1, 1, 9, "Clipper XY Scaling", NULL },
	/* Note: The docs say this was also 105, but it was 106 in the
	 * initial userland code drop.
	 */
	[VC4_PACKET_CLIPPER_Z_SCALING] = { 1, 1, 9, "Clipper Z Scale and Offset", NULL },

	[VC4_PACKET_TILE_BINNING_MODE_CONFIG] = { 1, 0, 16, "tile binning configuration", validate_tile_binning_config },

	[VC4_PACKET_TILE_RENDERING_MODE_CONFIG] = { 0, 1, 11, "tile rendering mode configuration", validate_tile_rendering_mode_config},

	[VC4_PACKET_CLEAR_COLORS] = { 0, 1, 14, "Clear Colors", NULL },

	[VC4_PACKET_TILE_COORDINATES] = { 0, 1, 3, "Tile Coordinates", validate_tile_coordinates },

	[VC4_PACKET_GEM_HANDLES] = { 1, 1, 9, "GEM handles", validate_gem_handles },
};

int
vc4_validate_cl(struct drm_device *dev,
		void *validated,
		void *unvalidated,
		uint32_t len,
		bool is_bin,
		struct exec_info *exec)
{
	uint32_t dst_offset = 0;
	uint32_t src_offset = 0;

	while (src_offset < len) {
		void *dst_pkt = validated + dst_offset;
		void *src_pkt = unvalidated + src_offset;
		u8 cmd = *(uint8_t *)src_pkt;
		const struct cmd_info *info;

		if (cmd > ARRAY_SIZE(cmd_info)) {
			DRM_ERROR("0x%08x: packet %d out of bounds\n",
				  src_offset, cmd);
			return -EINVAL;
		}

		info = &cmd_info[cmd];
		if (!info->name) {
			DRM_ERROR("0x%08x: packet %d invalid\n",
				  src_offset, cmd);
			return -EINVAL;
		}

#if 0
		DRM_INFO("0x%08x: packet %d (%s) size %d processing...\n",
			 src_offset, cmd, info->name, info->len);
#endif

		if ((is_bin && !info->bin) ||
		    (!is_bin && !info->render)) {
			DRM_ERROR("0x%08x: packet %d (%s) invalid for %s\n",
				  src_offset, cmd, info->name,
				  is_bin ? "binner" : "render");
			return -EINVAL;
		}

		if (src_offset + info->len > len) {
			DRM_ERROR("0x%08x: packet %d (%s) length 0x%08x "
				  "exceeds bounds (0x%08x)\n",
				  src_offset, cmd, info->name, info->len,
				  src_offset + len);
			return -EINVAL;
		}

		if (cmd != VC4_PACKET_GEM_HANDLES)
			memcpy(dst_pkt, src_pkt, info->len);

		if (info->func && info->func(exec,
					     dst_pkt + 1,
					     src_pkt + 1)) {
			DRM_ERROR("0x%08x: packet %d (%s) failed to "
				  "validate\n",
				  src_offset, cmd, info->name);
			return -EINVAL;
		}

		src_offset += info->len;
		/* GEM handle loading doesn't produce HW packets. */
		if (cmd != VC4_PACKET_GEM_HANDLES)
			dst_offset += info->len;

		/* When the CL hits halt, it'll stop reading anything else. */
		if (cmd == VC4_PACKET_HALT)
			break;
	}

	if (is_bin) {
		exec->ct0ea = exec->ct0ca + dst_offset;

		if (!exec->found_start_tile_binning_packet) {
			DRM_ERROR("Bin CL missing VC4_PACKET_START_TILE_BINNING\n");
			return -EINVAL;
		}
	} else {
		if (!exec->found_tile_rendering_mode_config_packet) {
			DRM_ERROR("Render CL missing VC4_PACKET_TILE_RENDERING_MODE_CONFIG\n");
			return -EINVAL;
		}
		exec->ct1ea = exec->ct1ca + dst_offset;
	}

	return 0;
}

static bool
reloc_tex(struct exec_info *exec,
	  void *uniform_data_u,
	  struct vc4_texture_sample_info *sample,
	  uint32_t texture_handle_index)

{
	struct drm_gem_cma_object *tex;
	uint32_t p0 = *(uint32_t *)(uniform_data_u + sample->p_offset[0]);
	uint32_t p1 = *(uint32_t *)(uniform_data_u + sample->p_offset[1]);
	uint32_t *validated_p0 = exec->uniforms_v + sample->p_offset[0];
	uint32_t offset = p0 & ~0xfff;
	uint32_t miplevels = (p0 & 15);
	uint32_t width = (p1 >> 8) & 2047;
	uint32_t height = (p1 >> 20) & 2047;
	uint32_t cpp, tiling_format, utile_w, utile_h;
	uint32_t i;
	enum vc4_texture_data_type type;

	if (width == 0)
		width = 2048;
	if (height == 0)
		height = 2048;

	if (p0 & (1 << 9)) {
		DRM_ERROR("Cube maps unsupported\n");
		return false;
	}

	type = ((p0 >> 4) & 15) | ((p1 >> 31) << 4);

	switch (type) {
	case VC4_TEXTURE_TYPE_RGBA8888:
	case VC4_TEXTURE_TYPE_RGBX8888:
	case VC4_TEXTURE_TYPE_RGBA32R:
		cpp = 4;
		break;
	case VC4_TEXTURE_TYPE_RGBA4444:
	case VC4_TEXTURE_TYPE_RGBA5551:
	case VC4_TEXTURE_TYPE_RGB565:
	case VC4_TEXTURE_TYPE_LUMALPHA:
	case VC4_TEXTURE_TYPE_S16F:
	case VC4_TEXTURE_TYPE_S16:
		cpp = 2;
		break;
	case VC4_TEXTURE_TYPE_LUMINANCE:
	case VC4_TEXTURE_TYPE_ALPHA:
	case VC4_TEXTURE_TYPE_S8:
		cpp = 1;
		break;
	case VC4_TEXTURE_TYPE_ETC1:
	case VC4_TEXTURE_TYPE_BW1:
	case VC4_TEXTURE_TYPE_A4:
	case VC4_TEXTURE_TYPE_A1:
	case VC4_TEXTURE_TYPE_RGBA64:
	case VC4_TEXTURE_TYPE_YUV422R:
	default:
		DRM_ERROR("Texture format %d unsupported\n", type);
		return false;
	}
	utile_w = utile_width(cpp);
	utile_h = utile_height(cpp);

	if (type == VC4_TEXTURE_TYPE_RGBA32R) {
		tiling_format = VC4_TILING_FORMAT_LINEAR;
	} else {
		if (size_is_lt(width, height, cpp))
			tiling_format = VC4_TILING_FORMAT_LT;
		else
			tiling_format = VC4_TILING_FORMAT_T;
	}

	if (!vc4_use_bo(exec, texture_handle_index, VC4_MODE_RENDER, &tex))
		return false;

	if (!check_tex_size(exec, tex, offset, tiling_format,
			    width, height, cpp)) {
		return false;
	}

	/* The mipmap levels are stored before the base of the texture.  Make
	 * sure there is actually space in the BO.
	 */
	for (i = 1; i <= miplevels; i++) {
		uint32_t level_width = max(width >> i, 1u);
		uint32_t level_height = max(height >> i, 1u);
		uint32_t aligned_width, aligned_height;
		uint32_t level_size;

		/* Once the levels get small enough, they drop from T to LT. */
		if (tiling_format == VC4_TILING_FORMAT_T &&
		    size_is_lt(level_width, level_height, cpp)) {
			tiling_format = VC4_TILING_FORMAT_LT;
		}

		switch (tiling_format) {
		case VC4_TILING_FORMAT_T:
			aligned_width = roundup(level_width, utile_w * 8);
			aligned_height = roundup(level_height, utile_h * 8);
			break;
		case VC4_TILING_FORMAT_LT:
			aligned_width = roundup(level_width, utile_w);
			aligned_height = roundup(level_height, utile_h);
			break;
		default:
			aligned_width = roundup(level_width, 16 / cpp);
			aligned_height = height;
			break;
		}

		level_size = aligned_width * cpp * aligned_height;

		if (offset < level_size) {
			DRM_ERROR("Level %d (%dx%d -> %dx%d) size %db "
				  "overflowed buffer bounds (offset %d)\n",
				  i, level_width, level_height,
				  aligned_width, aligned_height,
				  level_size, offset);
			return false;
		}

		offset -= level_size;
	}

	*validated_p0 = tex->paddr + p0;

	return true;
}

static int
validate_shader_rec(struct drm_device *dev,
		    struct exec_info *exec,
		    struct vc4_shader_state *state)
{
	uint32_t *src_handles;
	void *pkt_u, *pkt_v;
	enum shader_rec_reloc_type {
		RELOC_CODE,
		RELOC_VBO,
	};
	struct shader_rec_reloc {
		enum shader_rec_reloc_type type;
		uint32_t offset;
	};
	static const struct shader_rec_reloc gl_relocs[] = {
		{ RELOC_CODE, 4 },  /* fs */
		{ RELOC_CODE, 16 }, /* vs */
		{ RELOC_CODE, 28 }, /* cs */
	};
	static const struct shader_rec_reloc nv_relocs[] = {
		{ RELOC_CODE, 4 }, /* fs */
		{ RELOC_VBO, 12 }
	};
	const struct shader_rec_reloc *relocs;
	struct drm_gem_cma_object *bo[ARRAY_SIZE(gl_relocs) + 8];
	uint32_t nr_attributes = 0, nr_fixed_relocs, nr_relocs, packet_size;
	int i;
	struct vc4_validated_shader_info *validated_shader = NULL;

	if (state->packet == VC4_PACKET_NV_SHADER_STATE) {
		relocs = nv_relocs;
		nr_fixed_relocs = ARRAY_SIZE(nv_relocs);

		packet_size = 16;
	} else {
		relocs = gl_relocs;
		nr_fixed_relocs = ARRAY_SIZE(gl_relocs);

		nr_attributes = state->addr & 0x7;
		if (nr_attributes == 0)
			nr_attributes = 8;
		packet_size = gl_shader_rec_size(state->addr);
	}
	nr_relocs = nr_fixed_relocs + nr_attributes;

	if (nr_relocs * 4 > exec->shader_rec_size) {
		DRM_ERROR("overflowed shader recs reading %d handles "
			  "from %d bytes left\n",
			  nr_relocs, exec->shader_rec_size);
		return -EINVAL;
	}
	src_handles = exec->shader_rec_u;
	exec->shader_rec_u += nr_relocs * 4;
	exec->shader_rec_size -= nr_relocs * 4;

	if (packet_size > exec->shader_rec_size) {
		DRM_ERROR("overflowed shader recs copying %db packet "
			  "from %d bytes left\n",
			  packet_size, exec->shader_rec_size);
		return -EINVAL;
	}
	pkt_u = exec->shader_rec_u;
	pkt_v = exec->shader_rec_v;
	memcpy(pkt_v, pkt_u, packet_size);
	exec->shader_rec_u += packet_size;
	/* Shader recs have to be aligned to 16 bytes (due to the attribute
	 * flags being in the low bytes), so round the next validated shader
	 * rec address up.  This should be safe, since we've got so many
	 * relocations in a shader rec packet.
	 */
	BUG_ON(roundup(packet_size, 16) - packet_size > nr_relocs * 4);
	exec->shader_rec_v += roundup(packet_size, 16);
	exec->shader_rec_size -= packet_size;

	for (i = 0; i < nr_relocs; i++) {
		enum vc4_bo_mode mode;

		if (i < nr_fixed_relocs && relocs[i].type == RELOC_CODE)
			mode = VC4_MODE_SHADER;
		else
			mode = VC4_MODE_RENDER;

		if (!vc4_use_bo(exec, src_handles[i], mode, &bo[i])) {
			return false;
		}
	}

	for (i = 0; i < nr_fixed_relocs; i++) {
		uint32_t o = relocs[i].offset;
		uint32_t src_offset = *(uint32_t *)(pkt_u + o);
		uint32_t *texture_handles_u;
		void *uniform_data_u;
		uint32_t tex;

		*(uint32_t *)(pkt_v + o) = bo[i]->paddr + src_offset;

		switch (relocs[i].type) {
		case RELOC_CODE:
			kfree(validated_shader);
			validated_shader = vc4_validate_shader(bo[i],
							       src_offset);
			if (!validated_shader)
				goto fail;

			if (validated_shader->uniforms_src_size >
			    exec->uniforms_size) {
				DRM_ERROR("Uniforms src buffer overflow\n");
				goto fail;
			}

			texture_handles_u = exec->uniforms_u;
			uniform_data_u = (texture_handles_u +
					  validated_shader->num_texture_samples);

			memcpy(exec->uniforms_v, uniform_data_u,
			       validated_shader->uniforms_size);

			for (tex = 0;
			     tex < validated_shader->num_texture_samples;
			     tex++) {
				if (!reloc_tex(exec,
					       uniform_data_u,
					       &validated_shader->texture_samples[tex],
					       texture_handles_u[tex])) {
					goto fail;
				}
			}

			*(uint32_t *)(pkt_v + o + 4) = exec->uniforms_p;

			exec->uniforms_u += validated_shader->uniforms_src_size;
			exec->uniforms_v += validated_shader->uniforms_size;
			exec->uniforms_p += validated_shader->uniforms_size;

			break;

		case RELOC_VBO:
			break;
		}
	}

	for (i = 0; i < nr_attributes; i++) {
		struct drm_gem_cma_object *vbo = bo[nr_fixed_relocs + i];
		uint32_t o = 36 + i * 8;
		uint32_t offset = *(uint32_t *)(pkt_u + o + 0);
		uint32_t attr_size = *(uint8_t *)(pkt_u + o + 4) + 1;
		uint32_t stride = *(uint8_t *)(pkt_u + o + 5);
		uint32_t max_index;

		if (state->addr & 0x8)
			stride |= (*(uint32_t *)(pkt_u + 100 + i * 4)) & ~0xff;

		if (vbo->base.size < offset ||
		    vbo->base.size - offset < attr_size) {
			DRM_ERROR("BO offset overflow (%d + %d > %d)\n",
				  offset, attr_size, vbo->base.size);
			return -EINVAL;
		}

		if (stride != 0) {
			max_index = ((vbo->base.size - offset - attr_size) /
				     stride);
			if (state->max_index > max_index) {
				DRM_ERROR("primitives use index %d out of supplied %d\n",
					  state->max_index, max_index);
				return -EINVAL;
			}
		}

		*(uint32_t *)(pkt_v + o) = vbo->paddr + offset;
	}

	kfree(validated_shader);

	return 0;

fail:
	kfree(validated_shader);
	return -EINVAL;
}

int
vc4_validate_shader_recs(struct drm_device *dev,
			 struct exec_info *exec)
{
	uint32_t i;
	int ret = 0;

	for (i = 0; i < exec->shader_state_count; i++) {
		ret = validate_shader_rec(dev, exec, &exec->shader_state[i]);
		if (ret)
			return ret;
	}

	return ret;
}
