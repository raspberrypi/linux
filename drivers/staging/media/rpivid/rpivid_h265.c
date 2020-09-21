// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Raspberry Pi HEVC driver
 *
 * Copyright (C) 2020 Raspberry Pi (Trading) Ltd
 *
 * Based on the Cedrus VPU driver, that is:
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 */

#include <linux/delay.h>
#include <linux/types.h>

#include <media/videobuf2-dma-contig.h>

#include "rpivid.h"
#include "rpivid_hw.h"

#define DEBUG_TRACE_P1_CMD 0
#define DEBUG_TRACE_EXECUTION 0

#if DEBUG_TRACE_EXECUTION
#define xtrace_in(dev_, de_)\
	v4l2_info(&(dev_)->v4l2_dev, "%s[%d]: in\n",   __func__,\
		  (de_) == NULL ? -1 : (de_)->decode_order)
#define xtrace_ok(dev_, de_)\
	v4l2_info(&(dev_)->v4l2_dev, "%s[%d]: ok\n",   __func__,\
		  (de_) == NULL ? -1 : (de_)->decode_order)
#define xtrace_fin(dev_, de_)\
	v4l2_info(&(dev_)->v4l2_dev, "%s[%d]: finish\n", __func__,\
		  (de_) == NULL ? -1 : (de_)->decode_order)
#define xtrace_fail(dev_, de_)\
	v4l2_info(&(dev_)->v4l2_dev, "%s[%d]: FAIL\n", __func__,\
		  (de_) == NULL ? -1 : (de_)->decode_order)
#else
#define xtrace_in(dev_, de_)
#define xtrace_ok(dev_, de_)
#define xtrace_fin(dev_, de_)
#define xtrace_fail(dev_, de_)
#endif

enum hevc_slice_type {
	HEVC_SLICE_B = 0,
	HEVC_SLICE_P = 1,
	HEVC_SLICE_I = 2,
};

enum hevc_layer { L0 = 0, L1 = 1 };

static int gptr_alloc(struct rpivid_dev *const dev, struct rpivid_gptr *gptr,
		      size_t size, unsigned long attrs)
{
	gptr->size = size;
	gptr->attrs = attrs;
	gptr->addr = 0;
	gptr->ptr = dma_alloc_attrs(dev->dev, gptr->size, &gptr->addr,
				    GFP_KERNEL, gptr->attrs);
	return !gptr->ptr ? -ENOMEM : 0;
}

static void gptr_free(struct rpivid_dev *const dev,
		      struct rpivid_gptr *const gptr)
{
	if (gptr->ptr)
		dma_free_attrs(dev->dev, gptr->size, gptr->ptr, gptr->addr,
			       gptr->attrs);
	gptr->size = 0;
	gptr->ptr = NULL;
	gptr->addr = 0;
	gptr->attrs = 0;
}

/* Realloc but do not copy */
static int gptr_realloc_new(struct rpivid_dev * const dev,
			    struct rpivid_gptr * const gptr, size_t size)
{
	if (size == gptr->size)
		return 0;

	if (gptr->ptr)
		dma_free_attrs(dev->dev, gptr->size, gptr->ptr,
			       gptr->addr, gptr->attrs);

	gptr->addr = 0;
	gptr->size = size;
	gptr->ptr = dma_alloc_attrs(dev->dev, gptr->size,
				    &gptr->addr, GFP_KERNEL, gptr->attrs);
	return gptr->ptr ? 0 : -ENOMEM;
}

/* floor(log2(x)) */
static unsigned int log2_size(size_t x)
{
	unsigned int n = 0;

	if (x & ~0xffff) {
		n += 16;
		x >>= 16;
	}
	if (x & ~0xff) {
		n += 8;
		x >>= 8;
	}
	if (x & ~0xf) {
		n += 4;
		x >>= 4;
	}
	if (x & ~3) {
		n += 2;
		x >>= 2;
	}
	return (x & ~1) ? n + 1 : n;
}

static size_t round_up_size(const size_t x)
{
	/* Admit no size < 256 */
	const unsigned int n = x < 256 ? 8 : log2_size(x) - 1;

	return x >= (3 << n) ? 4 << n : (3 << n);
}

static size_t next_size(const size_t x)
{
	return round_up_size(x + 1);
}

#define NUM_SCALING_FACTORS 4064 /* Not a typo = 0xbe0 + 0x400 */

#define AXI_BASE64 0

#define PROB_BACKUP ((20 << 12) + (20 << 6) + (0 << 0))
#define PROB_RELOAD ((20 << 12) + (20 << 0) + (0 << 6))

#define HEVC_MAX_REFS V4L2_HEVC_DPB_ENTRIES_NUM_MAX

//////////////////////////////////////////////////////////////////////////////

struct rpi_cmd {
	u32 addr;
	u32 data;
} __packed;

struct rpivid_q_aux {
	unsigned int refcount;
	unsigned int q_index;
	struct rpivid_q_aux *next;
	struct rpivid_gptr col;
};

//////////////////////////////////////////////////////////////////////////////

enum rpivid_decode_state {
	RPIVID_DECODE_SLICE_START,
	RPIVID_DECODE_SLICE_CONTINUE,
	RPIVID_DECODE_ERROR_CONTINUE,
	RPIVID_DECODE_ERROR_DONE,
	RPIVID_DECODE_PHASE1,
	RPIVID_DECODE_END,
};

struct rpivid_dec_env {
	struct rpivid_ctx *ctx;
	struct rpivid_dec_env *next;

	enum rpivid_decode_state state;
	unsigned int decode_order;
	int p1_status;		/* P1 status - what to realloc */

	struct rpivid_dec_env *phase_wait_q_next;

	struct rpi_cmd *cmd_fifo;
	unsigned int cmd_len, cmd_max;
	unsigned int num_slice_msgs;
	unsigned int pic_width_in_ctbs_y;
	unsigned int pic_height_in_ctbs_y;
	unsigned int dpbno_col;
	u32 reg_slicestart;
	int collocated_from_l0_flag;
	unsigned int wpp_entry_x;
	unsigned int wpp_entry_y;

	u32 rpi_config2;
	u32 rpi_framesize;
	u32 rpi_currpoc;

	struct vb2_v4l2_buffer *frame_buf; // Detached dest buffer
	unsigned int frame_c_offset;
	unsigned int frame_stride;
	dma_addr_t frame_addr;
	dma_addr_t ref_addrs[16];
	struct rpivid_q_aux *frame_aux;
	struct rpivid_q_aux *col_aux;

	dma_addr_t pu_base_vc;
	dma_addr_t coeff_base_vc;
	u32 pu_stride;
	u32 coeff_stride;

	struct rpivid_gptr *bit_copy_gptr;
	size_t bit_copy_len;
	struct rpivid_gptr *cmd_copy_gptr;

	u16 slice_msgs[2 * HEVC_MAX_REFS * 8 + 3];
	u8 scaling_factors[NUM_SCALING_FACTORS];

	struct rpivid_hw_irq_ent irq_ent;
};

#define member_size(type, member) sizeof(((type *)0)->member)

struct rpivid_dec_state {
	struct v4l2_ctrl_hevc_sps sps;
	struct v4l2_ctrl_hevc_pps pps;

	// Helper vars & tables derived from sps/pps
	unsigned int log2_ctb_size; /* log2 width of a CTB */
	unsigned int ctb_width; /* Width in CTBs */
	unsigned int ctb_height; /* Height in CTBs */
	unsigned int ctb_size; /* Pic area in CTBs */
	unsigned int num_tile_columns;
	unsigned int num_tile_rows;
	u8 column_width[member_size(struct v4l2_ctrl_hevc_pps,
				    column_width_minus1)];
	u8 row_height[member_size(struct v4l2_ctrl_hevc_pps,
				  row_height_minus1)];

	int *col_bd;
	int *row_bd;
	int *ctb_addr_rs_to_ts;
	int *ctb_addr_ts_to_rs;
	int *tile_id;

	// Aux starage for DPB
	// Hold refs
	struct rpivid_q_aux *ref_aux[HEVC_MAX_REFS];
	struct rpivid_q_aux *frame_aux;

	// Slice vars
	unsigned int slice_idx;
	bool frame_end;
	bool slice_temporal_mvp;  /* Slice flag but constant for frame */

	// Temp vars per run - don't actually need to persist
	u8 *src_buf;
	dma_addr_t src_addr;
	const struct v4l2_ctrl_hevc_slice_params *sh;
	unsigned int nb_refs[2];
	unsigned int slice_qp;
	unsigned int max_num_merge_cand; // 0 if I-slice
	bool dependent_slice_segment_flag;
};

static inline int clip_int(const int x, const int lo, const int hi)
{
	return x < lo ? lo : x > hi ? hi : x;
}

//////////////////////////////////////////////////////////////////////////////
// Phase 1 command and bit FIFOs

#if DEBUG_TRACE_P1_CMD
static int p1_z;
#endif

// ???? u16 addr - put in u32
static int p1_apb_write(struct rpivid_dec_env *const de, const u16 addr,
			const u32 data)
{
	if (de->cmd_len == de->cmd_max)
		de->cmd_fifo =
			krealloc(de->cmd_fifo,
				 (de->cmd_max *= 2) * sizeof(struct rpi_cmd),
				 GFP_KERNEL);
	de->cmd_fifo[de->cmd_len].addr = addr;
	de->cmd_fifo[de->cmd_len].data = data;

#if DEBUG_TRACE_P1_CMD
	if (++p1_z < 256) {
		v4l2_info(&de->ctx->dev->v4l2_dev, "[%02x] %x %x\n",
			  de->cmd_len, addr, data);
	}
#endif

	return de->cmd_len++;
}

static int ctb_to_tile(unsigned int ctb, unsigned int *bd, int num)
{
	int i;

	for (i = 1; ctb >= bd[i]; i++)
		; // bd[] has num+1 elements; bd[0]=0;
	return i - 1;
}

static int ctb_to_slice_w_h(unsigned int ctb, int ctb_size, int width,
			    unsigned int *bd, int num)
{
	if (ctb < bd[num - 1])
		return ctb_size;
	else if (width % ctb_size)
		return width % ctb_size;
	else
		return ctb_size;
}

static void aux_q_free(struct rpivid_ctx *const ctx,
		       struct rpivid_q_aux *const aq)
{
	struct rpivid_dev *const dev = ctx->dev;

	gptr_free(dev, &aq->col);
	kfree(aq);
}

static struct rpivid_q_aux *aux_q_alloc(struct rpivid_ctx *const ctx)
{
	struct rpivid_dev *const dev = ctx->dev;
	struct rpivid_q_aux *const aq = kzalloc(sizeof(*aq), GFP_KERNEL);

	if (!aq)
		return NULL;

	aq->refcount = 1;
	if (gptr_alloc(dev, &aq->col, ctx->colmv_picsize,
		       DMA_ATTR_FORCE_CONTIGUOUS | DMA_ATTR_NO_KERNEL_MAPPING))
		goto fail;

	return aq;

fail:
	kfree(aq);
	return NULL;
}

static struct rpivid_q_aux *aux_q_new(struct rpivid_ctx *const ctx,
				      const unsigned int q_index)
{
	struct rpivid_q_aux *aq;
	unsigned long lockflags;

	spin_lock_irqsave(&ctx->aux_lock, lockflags);
	aq = ctx->aux_free;
	if (aq) {
		ctx->aux_free = aq->next;
		aq->next = NULL;
		aq->refcount = 1;
	}
	spin_unlock_irqrestore(&ctx->aux_lock, lockflags);

	if (!aq) {
		aq = aux_q_alloc(ctx);
		if (!aq)
			return NULL;
	}

	aq->q_index = q_index;
	ctx->aux_ents[q_index] = aq;
	return aq;
}

static struct rpivid_q_aux *aux_q_ref(struct rpivid_ctx *const ctx,
				      struct rpivid_q_aux *const aq)
{
	if (aq) {
		unsigned long lockflags;

		spin_lock_irqsave(&ctx->aux_lock, lockflags);

		++aq->refcount;

		spin_unlock_irqrestore(&ctx->aux_lock, lockflags);
	}
	return aq;
}

static void aux_q_release(struct rpivid_ctx *const ctx,
			  struct rpivid_q_aux **const paq)
{
	struct rpivid_q_aux *const aq = *paq;
	*paq = NULL;

	if (aq) {
		unsigned long lockflags;

		spin_lock_irqsave(&ctx->aux_lock, lockflags);

		if (--aq->refcount == 0) {
			aq->next = ctx->aux_free;
			ctx->aux_free = aq;
			ctx->aux_ents[aq->q_index] = NULL;
		}

		spin_unlock_irqrestore(&ctx->aux_lock, lockflags);
	}
}

static void aux_q_init(struct rpivid_ctx *const ctx)
{
	spin_lock_init(&ctx->aux_lock);
	ctx->aux_free = NULL;
}

static void aux_q_uninit(struct rpivid_ctx *const ctx)
{
	struct rpivid_q_aux *aq;

	ctx->colmv_picsize = 0;
	ctx->colmv_stride = 0;
	while ((aq = ctx->aux_free) != NULL) {
		ctx->aux_free = aq->next;
		aux_q_free(ctx, aq);
	}
}

//////////////////////////////////////////////////////////////////////////////

/*
 * Initialisation process for context variables (CABAC init)
 * see H.265 9.3.2.2
 *
 * N.B. If comparing with FFmpeg note that this h/w uses slightly different
 * offsets to FFmpegs array
 */

/* Actual number of values */
#define RPI_PROB_VALS 154U
/* Rounded up as we copy words */
#define RPI_PROB_ARRAY_SIZE ((154 + 3) & ~3)

/* Initialiser values - see tables H.265 9-4 through 9-42 */
static const u8 prob_init[3][156] = {
	{
		153, 200, 139, 141, 157, 154, 154, 154, 154, 154, 184, 154, 154,
		154, 184, 63,  154, 154, 154, 154, 154, 154, 154, 154, 154, 154,
		154, 154, 154, 153, 138, 138, 111, 141, 94,  138, 182, 154, 154,
		154, 140, 92,  137, 138, 140, 152, 138, 139, 153, 74,  149, 92,
		139, 107, 122, 152, 140, 179, 166, 182, 140, 227, 122, 197, 110,
		110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
		79,  108, 123, 63,  110, 110, 124, 125, 140, 153, 125, 127, 140,
		109, 111, 143, 127, 111, 79,  108, 123, 63,  91,  171, 134, 141,
		138, 153, 136, 167, 152, 152, 139, 139, 111, 111, 125, 110, 110,
		94,  124, 108, 124, 107, 125, 141, 179, 153, 125, 107, 125, 141,
		179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182, 182,
		152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111, 0,   0,
	},
	{
		153, 185, 107, 139, 126, 197, 185, 201, 154, 149, 154, 139, 154,
		154, 154, 152, 110, 122, 95,  79,  63,  31,  31,  153, 153, 168,
		140, 198, 79,  124, 138, 94,  153, 111, 149, 107, 167, 154, 154,
		154, 154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 137, 169, 194, 166, 167, 154, 167, 137, 182, 125,
		110, 94,  110, 95,  79,  125, 111, 110, 78,  110, 111, 111, 95,
		94,  108, 123, 108, 125, 110, 94,  110, 95,  79,  125, 111, 110,
		78,  110, 111, 111, 95,  94,  108, 123, 108, 121, 140, 61,  154,
		107, 167, 91,  122, 107, 167, 139, 139, 155, 154, 139, 153, 139,
		123, 123, 63,  153, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 123, 123,
		107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0,
	},
	{
		153, 160, 107, 139, 126, 197, 185, 201, 154, 134, 154, 139, 154,
		154, 183, 152, 154, 137, 95,  79,  63,  31,  31,  153, 153, 168,
		169, 198, 79,  224, 167, 122, 153, 111, 149, 92,  167, 154, 154,
		154, 154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 122, 169, 208, 166, 167, 154, 152, 167, 182, 125,
		110, 124, 110, 95,  94,  125, 111, 111, 79,  125, 126, 111, 111,
		79,  108, 123, 93,  125, 110, 124, 110, 95,  94,  125, 111, 111,
		79,  125, 126, 111, 111, 79,  108, 123, 93,  121, 140, 61,  154,
		107, 167, 91,  107, 107, 167, 139, 139, 170, 154, 139, 153, 139,
		123, 123, 63,  124, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 138, 138,
		122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0,
	},
};

static void write_prob(struct rpivid_dec_env *const de,
		       const struct rpivid_dec_state *const s)
{
	u8 dst[RPI_PROB_ARRAY_SIZE];

	const unsigned int init_type =
		((s->sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) != 0 &&
		 s->sh->slice_type != HEVC_SLICE_I) ?
			s->sh->slice_type + 1 :
			2 - s->sh->slice_type;
	const u8 *p = prob_init[init_type];
	const int q = clip_int(s->slice_qp, 0, 51);
	unsigned int i;

	for (i = 0; i < RPI_PROB_VALS; i++) {
		int init_value = p[i];
		int m = (init_value >> 4) * 5 - 45;
		int n = ((init_value & 15) << 3) - 16;
		int pre = 2 * (((m * q) >> 4) + n) - 127;

		pre ^= pre >> 31;
		if (pre > 124)
			pre = 124 + (pre & 1);
		dst[i] = pre;
	}
	for (i = RPI_PROB_VALS; i != RPI_PROB_ARRAY_SIZE; ++i)
		dst[i] = 0;

	for (i = 0; i < RPI_PROB_ARRAY_SIZE; i += 4)
		p1_apb_write(de, 0x1000 + i,
			     dst[i] + (dst[i + 1] << 8) + (dst[i + 2] << 16) +
				     (dst[i + 3] << 24));
}

static void write_scaling_factors(struct rpivid_dec_env *const de)
{
	int i;
	const u8 *p = (u8 *)de->scaling_factors;

	for (i = 0; i < NUM_SCALING_FACTORS; i += 4, p += 4)
		p1_apb_write(de, 0x2000 + i,
			     p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24));
}

static inline __u32 dma_to_axi_addr(dma_addr_t a)
{
	return (__u32)(a >> 6);
}

static void write_bitstream(struct rpivid_dec_env *const de,
			    const struct rpivid_dec_state *const s)
{
	// Note that FFmpeg removes emulation prevention bytes, so this is
	// matched in the configuration here.
	// Whether that is the correct behaviour or not is not clear in the
	// spec.
	const int rpi_use_emu = 1;
	unsigned int offset = s->sh->data_bit_offset / 8 + 1;
	const unsigned int len = (s->sh->bit_size + 7) / 8 - offset;
	dma_addr_t addr;

	if (s->src_addr != 0) {
		addr = s->src_addr + offset;
	} else {
		memcpy(de->bit_copy_gptr->ptr + de->bit_copy_len,
		       s->src_buf + offset, len);
		addr = de->bit_copy_gptr->addr + de->bit_copy_len;
		de->bit_copy_len += (len + 63) & ~63;
	}
	offset = addr & 63;

	p1_apb_write(de, RPI_BFBASE, dma_to_axi_addr(addr));
	p1_apb_write(de, RPI_BFNUM, len);
	p1_apb_write(de, RPI_BFCONTROL, offset + (1 << 7)); // Stop
	p1_apb_write(de, RPI_BFCONTROL, offset + (rpi_use_emu << 6));
}

//////////////////////////////////////////////////////////////////////////////

static void write_slice(struct rpivid_dec_env *const de,
			const struct rpivid_dec_state *const s,
			const unsigned int slice_w,
			const unsigned int slice_h)
{
	u32 u32 = (s->sh->slice_type << 12) +
		  (((s->sh->flags &
		     V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) != 0)
		   << 14) +
		  (((s->sh->flags &
		     V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA) != 0)
		   << 15) +
		  (slice_w << 17) + (slice_h << 24);

	u32 |= (s->max_num_merge_cand << 0) + (s->nb_refs[L0] << 4) +
	       (s->nb_refs[L1] << 8);

	if (s->sh->slice_type == HEVC_SLICE_B)
		u32 |= ((s->sh->flags &
			 V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO) != 0)
		       << 16;
	p1_apb_write(de, RPI_SLICE, u32);
}

//////////////////////////////////////////////////////////////////////////////
// Tiles mode

static void new_entry_point(struct rpivid_dec_env *const de,
			    const struct rpivid_dec_state *const s,
			    const int do_bte,
			    const int reset_qp_y, const int ctb_addr_ts)
{
	int ctb_col = s->ctb_addr_ts_to_rs[ctb_addr_ts] %
							de->pic_width_in_ctbs_y;
	int ctb_row = s->ctb_addr_ts_to_rs[ctb_addr_ts] /
							de->pic_width_in_ctbs_y;

	int tile_x = ctb_to_tile(ctb_col, s->col_bd, s->num_tile_columns);
	int tile_y = ctb_to_tile(ctb_row, s->row_bd, s->num_tile_rows);

	int endx = s->col_bd[tile_x + 1] - 1;
	int endy = s->row_bd[tile_y + 1] - 1;

	u8 slice_w = ctb_to_slice_w_h(ctb_col, 1 << s->log2_ctb_size,
				      s->sps.pic_width_in_luma_samples,
				      s->col_bd, s->num_tile_columns);
	u8 slice_h = ctb_to_slice_w_h(ctb_row, 1 << s->log2_ctb_size,
				      s->sps.pic_height_in_luma_samples,
				      s->row_bd, s->num_tile_rows);

	p1_apb_write(de, RPI_TILESTART,
		     s->col_bd[tile_x] + (s->row_bd[tile_y] << 16));
	p1_apb_write(de, RPI_TILEEND, endx + (endy << 16));

	if (do_bte)
		p1_apb_write(de, RPI_BEGINTILEEND, endx + (endy << 16));

	write_slice(de, s, slice_w, slice_h);

	if (reset_qp_y) {
		unsigned int sps_qp_bd_offset =
			6 * s->sps.bit_depth_luma_minus8;

		p1_apb_write(de, RPI_QP, sps_qp_bd_offset + s->slice_qp);
	}

	p1_apb_write(de, RPI_MODE,
		     (0xFFFF << 0) + (0x0 << 16) +
			     ((tile_x == s->num_tile_columns - 1) << 17) +
			     ((tile_y == s->num_tile_rows - 1) << 18));

	p1_apb_write(de, RPI_CONTROL, (ctb_col << 0) + (ctb_row << 16));
}

//////////////////////////////////////////////////////////////////////////////

static void new_slice_segment(struct rpivid_dec_env *const de,
			      const struct rpivid_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_sps *const sps = &s->sps;
	const struct v4l2_ctrl_hevc_pps *const pps = &s->pps;

	p1_apb_write(de,
		     RPI_SPS0,
		     ((sps->log2_min_luma_coding_block_size_minus3 + 3) << 0) |
		     (s->log2_ctb_size << 4) |
		     ((sps->log2_min_luma_transform_block_size_minus2 + 2)
							<< 8) |
		     ((sps->log2_min_luma_transform_block_size_minus2 + 2 +
		       sps->log2_diff_max_min_luma_transform_block_size)
						<< 12) |
		     ((sps->bit_depth_luma_minus8 + 8) << 16) |
		     ((sps->bit_depth_chroma_minus8 + 8) << 20) |
		     (sps->max_transform_hierarchy_depth_intra << 24) |
		     (sps->max_transform_hierarchy_depth_inter << 28));

	p1_apb_write(de,
		     RPI_SPS1,
		     ((sps->pcm_sample_bit_depth_luma_minus1 + 1) << 0) |
		     ((sps->pcm_sample_bit_depth_chroma_minus1 + 1) << 4) |
		     ((sps->log2_min_pcm_luma_coding_block_size_minus3 + 3)
						<< 8) |
		     ((sps->log2_min_pcm_luma_coding_block_size_minus3 + 3 +
		       sps->log2_diff_max_min_pcm_luma_coding_block_size)
						<< 12) |
		     (((sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE) ?
				0 : sps->chroma_format_idc) << 16) |
		     ((!!(sps->flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED)) << 18) |
		     ((!!(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED)) << 19) |
		     ((!!(sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED))
						<< 20) |
		     ((!!(sps->flags &
			   V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED))
						<< 21));

	p1_apb_write(de,
		     RPI_PPS,
		     ((s->log2_ctb_size - pps->diff_cu_qp_delta_depth) << 0) |
		     ((!!(pps->flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED))
						 << 4) |
		     ((!!(pps->flags &
				V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED))
						 << 5) |
		     ((!!(pps->flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED))
						 << 6) |
		     ((!!(pps->flags &
				V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED))
						<< 7) |
		     (((pps->pps_cb_qp_offset + s->sh->slice_cb_qp_offset) & 255)
						<< 8) |
		     (((pps->pps_cr_qp_offset + s->sh->slice_cr_qp_offset) & 255)
						<< 16) |
		     ((!!(pps->flags &
				V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED))
						<< 24));

	if ((sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED) != 0)
		write_scaling_factors(de);

	if (!s->dependent_slice_segment_flag) {
		int ctb_col = s->sh->slice_segment_addr %
							de->pic_width_in_ctbs_y;
		int ctb_row = s->sh->slice_segment_addr /
							de->pic_width_in_ctbs_y;

		de->reg_slicestart = (ctb_col << 0) + (ctb_row << 16);
	}

	p1_apb_write(de, RPI_SLICESTART, de->reg_slicestart);
}

//////////////////////////////////////////////////////////////////////////////
// Slice messages

static void msg_slice(struct rpivid_dec_env *const de, const u16 msg)
{
	de->slice_msgs[de->num_slice_msgs++] = msg;
}

static void program_slicecmds(struct rpivid_dec_env *const de,
			      const int sliceid)
{
	int i;

	p1_apb_write(de, RPI_SLICECMDS, de->num_slice_msgs + (sliceid << 8));

	for (i = 0; i < de->num_slice_msgs; i++)
		p1_apb_write(de, 0x4000 + 4 * i, de->slice_msgs[i] & 0xffff);
}

// NoBackwardPredictionFlag 8.3.5
// Simply checks POCs
static int has_backward(const struct v4l2_hevc_dpb_entry *const dpb,
			const __u8 *const idx, const unsigned int n,
			const unsigned int cur_poc)
{
	unsigned int i;

	for (i = 0; i < n; ++i) {
		// Compare mod 2^16
		// We only get u16 pocs & 8.3.1 says
		// "The bitstream shall not contain data that result in values
		//  of DiffPicOrderCnt( picA, picB ) used in the decoding
		//  process that are not in the range of −2^15 to 2^15 − 1,
		//  inclusive."
		if (((cur_poc - dpb[idx[i]].pic_order_cnt[0]) & 0x8000) != 0)
			return 0;
	}
	return 1;
}

static void pre_slice_decode(struct rpivid_dec_env *const de,
			     const struct rpivid_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_slice_params *const sh = s->sh;
	int weighted_pred_flag, idx;
	u16 cmd_slice;
	unsigned int collocated_from_l0_flag;

	de->num_slice_msgs = 0;

	cmd_slice = 0;
	if (sh->slice_type == HEVC_SLICE_I)
		cmd_slice = 1;
	if (sh->slice_type == HEVC_SLICE_P)
		cmd_slice = 2;
	if (sh->slice_type == HEVC_SLICE_B)
		cmd_slice = 3;

	cmd_slice |= (s->nb_refs[L0] << 2) | (s->nb_refs[L1] << 6) |
		     (s->max_num_merge_cand << 11);

	collocated_from_l0_flag =
		!s->slice_temporal_mvp ||
		sh->slice_type != HEVC_SLICE_B ||
		(sh->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0);
	cmd_slice |= collocated_from_l0_flag << 14;

	if (sh->slice_type == HEVC_SLICE_P || sh->slice_type == HEVC_SLICE_B) {
		// Flag to say all reference pictures are from the past
		const int no_backward_pred_flag =
			has_backward(sh->dpb, sh->ref_idx_l0, s->nb_refs[L0],
				     sh->slice_pic_order_cnt) &&
			has_backward(sh->dpb, sh->ref_idx_l1, s->nb_refs[L1],
				     sh->slice_pic_order_cnt);
		cmd_slice |= no_backward_pred_flag << 10;
		msg_slice(de, cmd_slice);

		if (s->slice_temporal_mvp) {
			const __u8 *const rpl = collocated_from_l0_flag ?
						sh->ref_idx_l0 : sh->ref_idx_l1;
			de->dpbno_col = rpl[sh->collocated_ref_idx];
			//v4l2_info(&de->ctx->dev->v4l2_dev,
			//	    "L0=%d col_ref_idx=%d,
			//          dpb_no=%d\n", collocated_from_l0_flag,
			//          sh->collocated_ref_idx, de->dpbno_col);
		}

		// Write reference picture descriptions
		weighted_pred_flag =
			sh->slice_type == HEVC_SLICE_P ?
				!!(s->pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) :
				!!(s->pps.flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED);

		for (idx = 0; idx < s->nb_refs[L0]; ++idx) {
			unsigned int dpb_no = sh->ref_idx_l0[idx];
			//v4l2_info(&de->ctx->dev->v4l2_dev,
			//	  "L0[%d]=dpb[%d]\n", idx, dpb_no);

			msg_slice(de,
				  dpb_no |
				  (sh->dpb[dpb_no].rps ==
					V4L2_HEVC_DPB_ENTRY_RPS_LT_CURR ?
						 (1 << 4) : 0) |
				  (weighted_pred_flag ? (3 << 5) : 0));
			msg_slice(de, sh->dpb[dpb_no].pic_order_cnt[0]);

			if (weighted_pred_flag) {
				const struct v4l2_hevc_pred_weight_table
					*const w = &sh->pred_weight_table;
				const int luma_weight_denom =
					(1 << w->luma_log2_weight_denom);
				const unsigned int chroma_log2_weight_denom =
					(w->luma_log2_weight_denom +
					 w->delta_chroma_log2_weight_denom);
				const int chroma_weight_denom =
					(1 << chroma_log2_weight_denom);

				msg_slice(de,
					  w->luma_log2_weight_denom |
					  (((w->delta_luma_weight_l0[idx] +
					     luma_weight_denom) & 0x1ff)
						 << 3));
				msg_slice(de, w->luma_offset_l0[idx] & 0xff);
				msg_slice(de,
					  chroma_log2_weight_denom |
					  (((w->delta_chroma_weight_l0[idx][0] +
					     chroma_weight_denom) & 0x1ff)
						   << 3));
				msg_slice(de,
					  w->chroma_offset_l0[idx][0] & 0xff);
				msg_slice(de,
					  chroma_log2_weight_denom |
					  (((w->delta_chroma_weight_l0[idx][1] +
					     chroma_weight_denom) & 0x1ff)
						   << 3));
				msg_slice(de,
					  w->chroma_offset_l0[idx][1] & 0xff);
			}
		}

		for (idx = 0; idx < s->nb_refs[L1]; ++idx) {
			unsigned int dpb_no = sh->ref_idx_l1[idx];
			//v4l2_info(&de->ctx->dev->v4l2_dev,
			//          "L1[%d]=dpb[%d]\n", idx, dpb_no);
			msg_slice(de,
				  dpb_no |
				  (sh->dpb[dpb_no].rps ==
					 V4L2_HEVC_DPB_ENTRY_RPS_LT_CURR ?
						 (1 << 4) : 0) |
					(weighted_pred_flag ? (3 << 5) : 0));
			msg_slice(de, sh->dpb[dpb_no].pic_order_cnt[0]);
			if (weighted_pred_flag) {
				const struct v4l2_hevc_pred_weight_table
					*const w = &sh->pred_weight_table;
				const int luma_weight_denom =
					(1 << w->luma_log2_weight_denom);
				const unsigned int chroma_log2_weight_denom =
					(w->luma_log2_weight_denom +
					 w->delta_chroma_log2_weight_denom);
				const int chroma_weight_denom =
					(1 << chroma_log2_weight_denom);

				msg_slice(de,
					  w->luma_log2_weight_denom |
					  (((w->delta_luma_weight_l1[idx] +
					     luma_weight_denom) & 0x1ff) << 3));
				msg_slice(de, w->luma_offset_l1[idx] & 0xff);
				msg_slice(de,
					  chroma_log2_weight_denom |
					  (((w->delta_chroma_weight_l1[idx][0] +
					     chroma_weight_denom) & 0x1ff)
							<< 3));
				msg_slice(de,
					  w->chroma_offset_l1[idx][0] & 0xff);
				msg_slice(de,
					  chroma_log2_weight_denom |
					  (((w->delta_chroma_weight_l1[idx][1] +
					     chroma_weight_denom) & 0x1ff)
						   << 3));
				msg_slice(de,
					  w->chroma_offset_l1[idx][1] & 0xff);
			}
		}
	} else {
		msg_slice(de, cmd_slice);
	}

	msg_slice(de,
		  (sh->slice_beta_offset_div2 & 15) |
		  ((sh->slice_tc_offset_div2 & 15) << 4) |
		  ((sh->flags &
		    V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED) ?
						1 << 8 : 0) |
		  ((sh->flags &
			  V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) ?
						1 << 9 : 0) |
		  ((s->pps.flags &
			  V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED) ?
						1 << 10 : 0));

	msg_slice(de, ((sh->slice_cr_qp_offset & 31) << 5) +
		       (sh->slice_cb_qp_offset & 31)); // CMD_QPOFF
}

//////////////////////////////////////////////////////////////////////////////
// Write STATUS register with expected end CTU address of previous slice

static void end_previous_slice(struct rpivid_dec_env *const de,
			       const struct rpivid_dec_state *const s,
			       const int ctb_addr_ts)
{
	int last_x =
		s->ctb_addr_ts_to_rs[ctb_addr_ts - 1] % de->pic_width_in_ctbs_y;
	int last_y =
		s->ctb_addr_ts_to_rs[ctb_addr_ts - 1] / de->pic_width_in_ctbs_y;

	p1_apb_write(de, RPI_STATUS, 1 + (last_x << 5) + (last_y << 18));
}

static void wpp_pause(struct rpivid_dec_env *const de, int ctb_row)
{
	p1_apb_write(de, RPI_STATUS, (ctb_row << 18) + 0x25);
	p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
	p1_apb_write(de, RPI_MODE,
		     ctb_row == de->pic_height_in_ctbs_y - 1 ?
							0x70000 : 0x30000);
	p1_apb_write(de, RPI_CONTROL, (ctb_row << 16) + 2);
}

static void wpp_end_previous_slice(struct rpivid_dec_env *const de,
				   const struct rpivid_dec_state *const s,
				   int ctb_addr_ts)
{
	int new_x = s->sh->slice_segment_addr % de->pic_width_in_ctbs_y;
	int new_y = s->sh->slice_segment_addr / de->pic_width_in_ctbs_y;
	int last_x =
		s->ctb_addr_ts_to_rs[ctb_addr_ts - 1] % de->pic_width_in_ctbs_y;
	int last_y =
		s->ctb_addr_ts_to_rs[ctb_addr_ts - 1] / de->pic_width_in_ctbs_y;

	if (de->wpp_entry_x < 2 && (de->wpp_entry_y < new_y || new_x > 2) &&
	    de->pic_width_in_ctbs_y > 2)
		wpp_pause(de, last_y);
	p1_apb_write(de, RPI_STATUS, 1 + (last_x << 5) + (last_y << 18));
	if (new_x == 2 || (de->pic_width_in_ctbs_y == 2 &&
			   de->wpp_entry_y < new_y))
		p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
}

//////////////////////////////////////////////////////////////////////////////
// Wavefront mode

static void wpp_entry_point(struct rpivid_dec_env *const de,
			    const struct rpivid_dec_state *const s,
			    const int do_bte,
			    const int reset_qp_y, const int ctb_addr_ts)
{
	int ctb_size = 1 << s->log2_ctb_size;
	int ctb_addr_rs = s->ctb_addr_ts_to_rs[ctb_addr_ts];

	int ctb_col = de->wpp_entry_x = ctb_addr_rs % de->pic_width_in_ctbs_y;
	int ctb_row = de->wpp_entry_y = ctb_addr_rs / de->pic_width_in_ctbs_y;

	int endx = de->pic_width_in_ctbs_y - 1;
	int endy = ctb_row;

	u8 slice_w = ctb_to_slice_w_h(ctb_col, ctb_size,
				      s->sps.pic_width_in_luma_samples,
				      s->col_bd, s->num_tile_columns);
	u8 slice_h = ctb_to_slice_w_h(ctb_row, ctb_size,
				      s->sps.pic_height_in_luma_samples,
				      s->row_bd, s->num_tile_rows);

	p1_apb_write(de, RPI_TILESTART, 0);
	p1_apb_write(de, RPI_TILEEND, endx + (endy << 16));

	if (do_bte)
		p1_apb_write(de, RPI_BEGINTILEEND, endx + (endy << 16));

	write_slice(de, s, slice_w,
		    ctb_row == de->pic_height_in_ctbs_y - 1 ?
							slice_h : ctb_size);

	if (reset_qp_y) {
		unsigned int sps_qp_bd_offset =
			6 * s->sps.bit_depth_luma_minus8;

		p1_apb_write(de, RPI_QP, sps_qp_bd_offset + s->slice_qp);
	}

	p1_apb_write(de, RPI_MODE,
		     ctb_row == de->pic_height_in_ctbs_y - 1 ?
							0x60001 : 0x20001);
	p1_apb_write(de, RPI_CONTROL, (ctb_col << 0) + (ctb_row << 16));
}

//////////////////////////////////////////////////////////////////////////////
// Wavefront mode

static void wpp_decode_slice(struct rpivid_dec_env *const de,
			     const struct rpivid_dec_state *const s,
			     const struct v4l2_ctrl_hevc_slice_params *sh,
			     int ctb_addr_ts)
{
	int i, reset_qp_y = 1;
	int indep = !s->dependent_slice_segment_flag;
	int ctb_col = s->sh->slice_segment_addr % de->pic_width_in_ctbs_y;

	if (ctb_addr_ts)
		wpp_end_previous_slice(de, s, ctb_addr_ts);
	pre_slice_decode(de, s);
	write_bitstream(de, s);
	if (ctb_addr_ts == 0 || indep || de->pic_width_in_ctbs_y == 1)
		write_prob(de, s);
	else if (ctb_col == 0)
		p1_apb_write(de, RPI_TRANSFER, PROB_RELOAD);
	else
		reset_qp_y = 0;
	program_slicecmds(de, s->slice_idx);
	new_slice_segment(de, s);
	wpp_entry_point(de, s, indep, reset_qp_y, ctb_addr_ts);

	for (i = 0; i < s->sh->num_entry_point_offsets; i++) {
		int ctb_addr_rs = s->ctb_addr_ts_to_rs[ctb_addr_ts];
		int ctb_row = ctb_addr_rs / de->pic_width_in_ctbs_y;
		int last_x = de->pic_width_in_ctbs_y - 1;

		if (de->pic_width_in_ctbs_y > 2)
			wpp_pause(de, ctb_row);
		p1_apb_write(de, RPI_STATUS,
			     (ctb_row << 18) + (last_x << 5) + 2);
		if (de->pic_width_in_ctbs_y == 2)
			p1_apb_write(de, RPI_TRANSFER, PROB_BACKUP);
		if (de->pic_width_in_ctbs_y == 1)
			write_prob(de, s);
		else
			p1_apb_write(de, RPI_TRANSFER, PROB_RELOAD);
		ctb_addr_ts += s->column_width[0];
		wpp_entry_point(de, s, 0, 1, ctb_addr_ts);
	}
}

//////////////////////////////////////////////////////////////////////////////
// Tiles mode

static void decode_slice(struct rpivid_dec_env *const de,
			 const struct rpivid_dec_state *const s,
			 const struct v4l2_ctrl_hevc_slice_params *const sh,
			 int ctb_addr_ts)
{
	int i, reset_qp_y;

	if (ctb_addr_ts)
		end_previous_slice(de, s, ctb_addr_ts);

	pre_slice_decode(de, s);
	write_bitstream(de, s);

#if DEBUG_TRACE_P1_CMD
	if (p1_z < 256) {
		v4l2_info(&de->ctx->dev->v4l2_dev,
			  "TS=%d, tile=%d/%d, dss=%d, flags=%#llx\n",
			  ctb_addr_ts, s->tile_id[ctb_addr_ts],
			  s->tile_id[ctb_addr_ts - 1],
			  s->dependent_slice_segment_flag, sh->flags);
	}
#endif

	reset_qp_y = ctb_addr_ts == 0 ||
		   s->tile_id[ctb_addr_ts] != s->tile_id[ctb_addr_ts - 1] ||
		   !s->dependent_slice_segment_flag;
	if (reset_qp_y)
		write_prob(de, s);

	program_slicecmds(de, s->slice_idx);
	new_slice_segment(de, s);
	new_entry_point(de, s, !s->dependent_slice_segment_flag, reset_qp_y,
			ctb_addr_ts);

	for (i = 0; i < s->sh->num_entry_point_offsets; i++) {
		int ctb_addr_rs = s->ctb_addr_ts_to_rs[ctb_addr_ts];
		int ctb_col = ctb_addr_rs % de->pic_width_in_ctbs_y;
		int ctb_row = ctb_addr_rs / de->pic_width_in_ctbs_y;
		int tile_x = ctb_to_tile(ctb_col, s->col_bd,
					 s->num_tile_columns - 1);
		int tile_y =
			ctb_to_tile(ctb_row, s->row_bd, s->num_tile_rows - 1);
		int last_x = s->col_bd[tile_x + 1] - 1;
		int last_y = s->row_bd[tile_y + 1] - 1;

		p1_apb_write(de, RPI_STATUS,
			     2 + (last_x << 5) + (last_y << 18));
		write_prob(de, s);
		ctb_addr_ts += s->column_width[tile_x] * s->row_height[tile_y];
		new_entry_point(de, s, 0, 1, ctb_addr_ts);
	}
}

//////////////////////////////////////////////////////////////////////////////
// Scaling factors

static void expand_scaling_list(const unsigned int size_id,
				const unsigned int matrix_id, u8 *const dst0,
				const u8 *const src0, uint8_t dc)
{
	u8 *d;
	unsigned int x, y;

	// FIXME: matrix_id is unused ?
	switch (size_id) {
	case 0:
		memcpy(dst0, src0, 16);
		break;
	case 1:
		memcpy(dst0, src0, 64);
		break;
	case 2:
		d = dst0;

		for (y = 0; y != 16; y++) {
			const u8 *s = src0 + (y >> 1) * 8;

			for (x = 0; x != 8; ++x) {
				*d++ = *s;
				*d++ = *s++;
			}
		}
		dst0[0] = dc;
		break;
	default:
		d = dst0;

		for (y = 0; y != 32; y++) {
			const u8 *s = src0 + (y >> 2) * 8;

			for (x = 0; x != 8; ++x) {
				*d++ = *s;
				*d++ = *s;
				*d++ = *s;
				*d++ = *s++;
			}
		}
		dst0[0] = dc;
		break;
	}
}

static void populate_scaling_factors(const struct rpivid_run *const run,
				     struct rpivid_dec_env *const de,
				     const struct rpivid_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_scaling_matrix *const sl =
		run->h265.scaling_matrix;
	// Array of constants for scaling factors
	static const u32 scaling_factor_offsets[4][6] = {
		// MID0    MID1    MID2    MID3    MID4    MID5
		// SID0 (4x4)
		{ 0x0000, 0x0010, 0x0020, 0x0030, 0x0040, 0x0050 },
		// SID1 (8x8)
		{ 0x0060, 0x00A0, 0x00E0, 0x0120, 0x0160, 0x01A0 },
		// SID2 (16x16)
		{ 0x01E0, 0x02E0, 0x03E0, 0x04E0, 0x05E0, 0x06E0 },
		// SID3 (32x32)
		{ 0x07E0, 0x0BE0, 0x0000, 0x0000, 0x0000, 0x0000 }
	};

	unsigned int mid;

	for (mid = 0; mid < 6; mid++)
		expand_scaling_list(0, mid,
				    de->scaling_factors +
					    scaling_factor_offsets[0][mid],
				    sl->scaling_list_4x4[mid], 0);
	for (mid = 0; mid < 6; mid++)
		expand_scaling_list(1, mid,
				    de->scaling_factors +
					    scaling_factor_offsets[1][mid],
				    sl->scaling_list_8x8[mid], 0);
	for (mid = 0; mid < 6; mid++)
		expand_scaling_list(2, mid,
				    de->scaling_factors +
					    scaling_factor_offsets[2][mid],
				    sl->scaling_list_16x16[mid],
				    sl->scaling_list_dc_coef_16x16[mid]);
	for (mid = 0; mid < 2; mid += 1)
		expand_scaling_list(3, mid,
				    de->scaling_factors +
					    scaling_factor_offsets[3][mid],
				    sl->scaling_list_32x32[mid],
				    sl->scaling_list_dc_coef_32x32[mid]);
}

static void free_ps_info(struct rpivid_dec_state *const s)
{
	kfree(s->ctb_addr_rs_to_ts);
	s->ctb_addr_rs_to_ts = NULL;
	kfree(s->ctb_addr_ts_to_rs);
	s->ctb_addr_ts_to_rs = NULL;
	kfree(s->tile_id);
	s->tile_id = NULL;

	kfree(s->col_bd);
	s->col_bd = NULL;
	kfree(s->row_bd);
	s->row_bd = NULL;
}

static int updated_ps(struct rpivid_dec_state *const s)
{
	unsigned int ctb_addr_rs;
	int j, x, y, tile_id;
	unsigned int i;

	free_ps_info(s);

	// Inferred parameters
	s->log2_ctb_size = s->sps.log2_min_luma_coding_block_size_minus3 + 3 +
			   s->sps.log2_diff_max_min_luma_coding_block_size;

	s->ctb_width = (s->sps.pic_width_in_luma_samples +
			(1 << s->log2_ctb_size) - 1) >>
		       s->log2_ctb_size;
	s->ctb_height = (s->sps.pic_height_in_luma_samples +
			 (1 << s->log2_ctb_size) - 1) >>
			s->log2_ctb_size;
	s->ctb_size = s->ctb_width * s->ctb_height;

	// Inferred parameters

	if (!(s->pps.flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED)) {
		s->num_tile_columns = 1;
		s->num_tile_rows = 1;
		s->column_width[0] = s->ctb_width;
		s->row_height[0] = s->ctb_height;
	} else {
		s->num_tile_columns = s->pps.num_tile_columns_minus1 + 1;
		s->num_tile_rows = s->pps.num_tile_rows_minus1 + 1;
		for (i = 0; i < s->num_tile_columns; ++i)
			s->column_width[i] = s->pps.column_width_minus1[i] + 1;
		for (i = 0; i < s->num_tile_rows; ++i)
			s->row_height[i] = s->pps.row_height_minus1[i] + 1;
	}

	s->col_bd = kmalloc((s->num_tile_columns + 1) * sizeof(*s->col_bd),
			    GFP_KERNEL);
	s->row_bd = kmalloc((s->num_tile_rows + 1) * sizeof(*s->row_bd),
			    GFP_KERNEL);

	s->col_bd[0] = 0;
	for (i = 0; i < s->num_tile_columns; i++)
		s->col_bd[i + 1] = s->col_bd[i] + s->column_width[i];

	s->row_bd[0] = 0;
	for (i = 0; i < s->num_tile_rows; i++)
		s->row_bd[i + 1] = s->row_bd[i] + s->row_height[i];

	s->ctb_addr_rs_to_ts = kmalloc_array(s->ctb_size,
					     sizeof(*s->ctb_addr_rs_to_ts),
					     GFP_KERNEL);
	s->ctb_addr_ts_to_rs = kmalloc_array(s->ctb_size,
					     sizeof(*s->ctb_addr_ts_to_rs),
					     GFP_KERNEL);
	s->tile_id = kmalloc_array(s->ctb_size, sizeof(*s->tile_id),
				   GFP_KERNEL);

	for (ctb_addr_rs = 0; ctb_addr_rs < s->ctb_size; ctb_addr_rs++) {
		int tb_x = ctb_addr_rs % s->ctb_width;
		int tb_y = ctb_addr_rs / s->ctb_width;
		int tile_x = 0;
		int tile_y = 0;
		int val = 0;

		for (i = 0; i < s->num_tile_columns; i++) {
			if (tb_x < s->col_bd[i + 1]) {
				tile_x = i;
				break;
			}
		}

		for (i = 0; i < s->num_tile_rows; i++) {
			if (tb_y < s->row_bd[i + 1]) {
				tile_y = i;
				break;
			}
		}

		for (i = 0; i < tile_x; i++)
			val += s->row_height[tile_y] * s->column_width[i];
		for (i = 0; i < tile_y; i++)
			val += s->ctb_width * s->row_height[i];

		val += (tb_y - s->row_bd[tile_y]) * s->column_width[tile_x] +
		       tb_x - s->col_bd[tile_x];

		s->ctb_addr_rs_to_ts[ctb_addr_rs] = val;
		s->ctb_addr_ts_to_rs[val] = ctb_addr_rs;
	}

	for (j = 0, tile_id = 0; j < s->num_tile_rows; j++)
		for (i = 0; i < s->num_tile_columns; i++, tile_id++)
			for (y = s->row_bd[j]; y < s->row_bd[j + 1]; y++)
				for (x = s->col_bd[i];
				     x < s->col_bd[i + 1];
				     x++)
					s->tile_id[s->ctb_addr_rs_to_ts
							   [y * s->ctb_width +
							    x]] = tile_id;

	return 0;
}

static int frame_end(struct rpivid_dev *const dev,
		     struct rpivid_dec_env *const de,
		     const struct rpivid_dec_state *const s)
{
	const unsigned int last_x = s->col_bd[s->num_tile_columns] - 1;
	const unsigned int last_y = s->row_bd[s->num_tile_rows] - 1;
	size_t cmd_size;

	if (s->pps.flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED) {
		if (de->wpp_entry_x < 2 && de->pic_width_in_ctbs_y > 2)
			wpp_pause(de, last_y);
	}
	p1_apb_write(de, RPI_STATUS, 1 + (last_x << 5) + (last_y << 18));

	// Copy commands out to dma buf
	cmd_size = de->cmd_len * sizeof(de->cmd_fifo[0]);

	if (!de->cmd_copy_gptr->ptr || cmd_size > de->cmd_copy_gptr->size) {
		size_t cmd_alloc = round_up_size(cmd_size);

		if (gptr_realloc_new(dev, de->cmd_copy_gptr, cmd_alloc)) {
			v4l2_err(&dev->v4l2_dev,
				 "Alloc cmd buffer (%d): FAILED\n", cmd_alloc);
			return -ENOMEM;
		}
		v4l2_info(&dev->v4l2_dev, "Alloc cmd buffer (%d): OK\n",
			  cmd_alloc);
	}

	memcpy(de->cmd_copy_gptr->ptr, de->cmd_fifo, cmd_size);
	return 0;
}

static void setup_colmv(struct rpivid_ctx *const ctx, struct rpivid_run *run,
			struct rpivid_dec_state *const s)
{
	ctx->colmv_stride = ALIGN(s->sps.pic_width_in_luma_samples, 64);
	ctx->colmv_picsize = ctx->colmv_stride *
		(ALIGN(s->sps.pic_height_in_luma_samples, 64) >> 4);
}

// Can be called from irq context
static struct rpivid_dec_env *dec_env_new(struct rpivid_ctx *const ctx)
{
	struct rpivid_dec_env *de;
	unsigned long lock_flags;

	spin_lock_irqsave(&ctx->dec_lock, lock_flags);

	de = ctx->dec_free;
	if (de) {
		ctx->dec_free = de->next;
		de->next = NULL;
		de->state = RPIVID_DECODE_SLICE_START;
	}

	spin_unlock_irqrestore(&ctx->dec_lock, lock_flags);
	return de;
}

// Can be called from irq context
static void dec_env_delete(struct rpivid_dec_env *const de)
{
	struct rpivid_ctx * const ctx = de->ctx;
	unsigned long lock_flags;

	aux_q_release(ctx, &de->frame_aux);
	aux_q_release(ctx, &de->col_aux);

	spin_lock_irqsave(&ctx->dec_lock, lock_flags);

	de->state = RPIVID_DECODE_END;
	de->next = ctx->dec_free;
	ctx->dec_free = de;

	spin_unlock_irqrestore(&ctx->dec_lock, lock_flags);
}

static void dec_env_uninit(struct rpivid_ctx *const ctx)
{
	unsigned int i;

	if (ctx->dec_pool) {
		for (i = 0; i != RPIVID_DEC_ENV_COUNT; ++i) {
			struct rpivid_dec_env *const de = ctx->dec_pool + i;

			kfree(de->cmd_fifo);
		}

		kfree(ctx->dec_pool);
	}

	ctx->dec_pool = NULL;
	ctx->dec_free = NULL;
}

static int dec_env_init(struct rpivid_ctx *const ctx)
{
	unsigned int i;

	ctx->dec_pool = kzalloc(sizeof(*ctx->dec_pool) * RPIVID_DEC_ENV_COUNT,
				GFP_KERNEL);
	if (!ctx->dec_pool)
		return -1;

	spin_lock_init(&ctx->dec_lock);

	// Build free chain
	ctx->dec_free = ctx->dec_pool;
	for (i = 0; i != RPIVID_DEC_ENV_COUNT - 1; ++i)
		ctx->dec_pool[i].next = ctx->dec_pool + i + 1;

	// Fill in other bits
	for (i = 0; i != RPIVID_DEC_ENV_COUNT; ++i) {
		struct rpivid_dec_env *const de = ctx->dec_pool + i;

		de->ctx = ctx;
		de->decode_order = i;
		de->cmd_max = 1024;
		de->cmd_fifo = kmalloc_array(de->cmd_max,
					     sizeof(struct rpi_cmd),
					     GFP_KERNEL);
		if (!de->cmd_fifo)
			goto fail;
	}

	return 0;

fail:
	dec_env_uninit(ctx);
	return -1;
}

// Assume that we get exactly the same DPB for every slice
// it makes no real sense otherwise
#if V4L2_HEVC_DPB_ENTRIES_NUM_MAX > 16
#error HEVC_DPB_ENTRIES > h/w slots
#endif

static u32 mk_config2(const struct rpivid_dec_state *const s)
{
	const struct v4l2_ctrl_hevc_sps *const sps = &s->sps;
	const struct v4l2_ctrl_hevc_pps *const pps = &s->pps;
	u32 c;
	// BitDepthY
	c = (sps->bit_depth_luma_minus8 + 8) << 0;
	 // BitDepthC
	c |= (sps->bit_depth_chroma_minus8 + 8) << 4;
	 // BitDepthY
	if (sps->bit_depth_luma_minus8)
		c |= BIT(8);
	// BitDepthC
	if (sps->bit_depth_chroma_minus8)
		c |= BIT(9);
	c |= s->log2_ctb_size << 10;
	if (pps->flags & V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED)
		c |= BIT(13);
	if (sps->flags & V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED)
		c |= BIT(14);
	if (sps->flags & V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED)
		c |= BIT(15); /* Write motion vectors to external memory */
	c |= (pps->log2_parallel_merge_level_minus2 + 2) << 16;
	if (s->slice_temporal_mvp)
		c |= BIT(19);
	if (sps->flags & V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED)
		c |= BIT(20);
	c |= (pps->pps_cb_qp_offset & 31) << 21;
	c |= (pps->pps_cr_qp_offset & 31) << 26;
	return c;
}

static void rpivid_h265_setup(struct rpivid_ctx *ctx, struct rpivid_run *run)
{
	struct rpivid_dev *const dev = ctx->dev;
	const struct v4l2_ctrl_hevc_slice_params *const sh =
						run->h265.slice_params;
	const struct v4l2_hevc_pred_weight_table *pred_weight_table;
	struct rpivid_q_aux *dpb_q_aux[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	struct rpivid_dec_state *const s = ctx->state;
	struct vb2_queue *vq;
	struct rpivid_dec_env *de;
	int ctb_addr_ts;
	unsigned int i;
	int use_aux;
	bool slice_temporal_mvp;

	pred_weight_table = &sh->pred_weight_table;

	s->frame_end =
		((run->src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) == 0);

	de = ctx->dec0;
	slice_temporal_mvp = (sh->flags &
		   V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED);

	if (de && de->state != RPIVID_DECODE_END) {
		++s->slice_idx;

		switch (de->state) {
		case RPIVID_DECODE_SLICE_CONTINUE:
			// Expected state
			break;
		default:
			v4l2_err(&dev->v4l2_dev, "%s: Unexpected state: %d\n",
				 __func__, de->state);
		/* FALLTHRU */
		case RPIVID_DECODE_ERROR_CONTINUE:
			// Uncleared error - fail now
			goto fail;
		}

		if (s->slice_temporal_mvp != slice_temporal_mvp) {
			v4l2_warn(&dev->v4l2_dev,
				  "Slice Temporal MVP non-constant\n");
			goto fail;
		}
	} else {
		/* Frame start */
		unsigned int ctb_size_y;
		bool sps_changed = false;

		if (memcmp(&s->sps, run->h265.sps, sizeof(s->sps)) != 0) {
			/* SPS changed */
			v4l2_info(&dev->v4l2_dev, "SPS changed\n");
			memcpy(&s->sps, run->h265.sps, sizeof(s->sps));
			sps_changed = true;
		}
		if (sps_changed ||
		    memcmp(&s->pps, run->h265.pps, sizeof(s->pps)) != 0) {
			/* SPS changed */
			v4l2_info(&dev->v4l2_dev, "PPS changed\n");
			memcpy(&s->pps, run->h265.pps, sizeof(s->pps));

			/* Recalc stuff as required */
			updated_ps(s);
		}

		de = dec_env_new(ctx);
		if (!de) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to find free decode env\n");
			goto fail;
		}
		ctx->dec0 = de;

		ctb_size_y =
			1U << (s->sps.log2_min_luma_coding_block_size_minus3 +
			       3 +
			       s->sps.log2_diff_max_min_luma_coding_block_size);

		de->pic_width_in_ctbs_y =
			(s->sps.pic_width_in_luma_samples + ctb_size_y - 1) /
				ctb_size_y; // 7-15
		de->pic_height_in_ctbs_y =
			(s->sps.pic_height_in_luma_samples + ctb_size_y - 1) /
				ctb_size_y; // 7-17
		de->cmd_len = 0;
		de->dpbno_col = ~0U;

		de->bit_copy_gptr = ctx->bitbufs + 0;
		de->bit_copy_len = 0;
		de->cmd_copy_gptr = ctx->cmdbufs + 0;

		de->frame_c_offset = ctx->dst_fmt.height * 128;
		de->frame_stride = ctx->dst_fmt.bytesperline * 128;
		de->frame_addr =
			vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
		de->frame_aux = NULL;

		if (s->sps.bit_depth_luma_minus8 !=
		    s->sps.bit_depth_chroma_minus8) {
			v4l2_warn(&dev->v4l2_dev,
				  "Chroma depth (%d) != Luma depth (%d)\n",
				  s->sps.bit_depth_chroma_minus8 + 8,
				  s->sps.bit_depth_luma_minus8 + 8);
			goto fail;
		}
		if (s->sps.bit_depth_luma_minus8 == 0) {
			if (ctx->dst_fmt.pixelformat !=
						V4L2_PIX_FMT_NV12_COL128) {
				v4l2_err(&dev->v4l2_dev,
					 "Pixel format %#x != NV12_COL128 for 8-bit output",
					 ctx->dst_fmt.pixelformat);
				goto fail;
			}
		} else if (s->sps.bit_depth_luma_minus8 == 2) {
			if (ctx->dst_fmt.pixelformat !=
						V4L2_PIX_FMT_NV12_10_COL128) {
				v4l2_err(&dev->v4l2_dev,
					 "Pixel format %#x != NV12_10_COL128 for 10-bit output",
					 ctx->dst_fmt.pixelformat);
				goto fail;
			}
		} else {
			v4l2_warn(&dev->v4l2_dev,
				  "Luma depth (%d) unsupported\n",
				  s->sps.bit_depth_luma_minus8 + 8);
			goto fail;
		}
		if (run->dst->vb2_buf.num_planes != 1) {
			v4l2_warn(&dev->v4l2_dev, "Capture planes (%d) != 1\n",
				  run->dst->vb2_buf.num_planes);
			goto fail;
		}
		if (run->dst->planes[0].length <
		    ctx->dst_fmt.sizeimage) {
			v4l2_warn(&dev->v4l2_dev,
				  "Capture plane[0] length (%d) < sizeimage (%d)\n",
				  run->dst->planes[0].length,
				  ctx->dst_fmt.sizeimage);
			goto fail;
		}

		if (s->sps.pic_width_in_luma_samples > 4096 ||
		    s->sps.pic_height_in_luma_samples > 4096) {
			v4l2_warn(&dev->v4l2_dev,
				  "Pic dimension (%dx%d) exeeds 4096\n",
				  s->sps.pic_width_in_luma_samples,
				  s->sps.pic_height_in_luma_samples);
			goto fail;
		}

		// Fill in ref planes with our address s.t. if we mess
		// up refs somehow then we still have a valid address
		// entry
		for (i = 0; i != 16; ++i)
			de->ref_addrs[i] = de->frame_addr;

		/*
		 * Stash initial temporal_mvp flag
		 * This must be the same for all pic slices (7.4.7.1)
		 */
		s->slice_temporal_mvp = slice_temporal_mvp;

		// Phase 2 reg pre-calc
		de->rpi_config2 = mk_config2(s);
		de->rpi_framesize = (s->sps.pic_height_in_luma_samples << 16) |
				    s->sps.pic_width_in_luma_samples;
		de->rpi_currpoc = sh->slice_pic_order_cnt;

		if (s->sps.flags &
		    V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) {
			setup_colmv(ctx, run, s);
		}

		s->slice_idx = 0;

		if (sh->slice_segment_addr != 0) {
			v4l2_warn(&dev->v4l2_dev,
				  "New frame but segment_addr=%d\n",
				  sh->slice_segment_addr);
			goto fail;
		}

		/* Allocate a bitbuf if we need one - don't need one if single
		 * slice as we can use the src buf directly
		 */
		if (!s->frame_end && !de->bit_copy_gptr->ptr) {
			const size_t wxh = s->sps.pic_width_in_luma_samples *
				s->sps.pic_height_in_luma_samples;
			size_t bits_alloc;

			/* Annex A gives a min compression of 2 @ lvl 3.1
			 * (wxh <= 983040) and min 4 thereafter but avoid
			 * the odity of 983041 having a lower limit than
			 * 983040.
			 * Multiply by 3/2 for 4:2:0
			 */
			bits_alloc = wxh < 983040 ? wxh * 3 / 4 :
				wxh < 983040 * 2 ? 983040 * 3 / 4 :
				wxh * 3 / 8;
			bits_alloc = round_up_size(bits_alloc);

			if (gptr_alloc(dev, de->bit_copy_gptr,
				       bits_alloc,
				       DMA_ATTR_FORCE_CONTIGUOUS) != 0) {
				v4l2_err(&dev->v4l2_dev,
					 "Unable to alloc buf (%d) for bit copy\n",
					 bits_alloc);
				goto fail;
			}
			v4l2_info(&dev->v4l2_dev,
				  "Alloc buf (%d) for bit copy OK\n",
				  bits_alloc);
		}
	}

	// Pre calc a few things
	s->src_addr =
		!s->frame_end ?
			0 :
			vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	s->src_buf = s->src_addr != 0 ? NULL :
					vb2_plane_vaddr(&run->src->vb2_buf, 0);
	if (!s->src_addr && !s->src_buf) {
		v4l2_err(&dev->v4l2_dev, "Failed to map src buffer\n");
		goto fail;
	}

	s->sh = sh;
	s->slice_qp = 26 + s->pps.init_qp_minus26 + s->sh->slice_qp_delta;
	s->max_num_merge_cand = sh->slice_type == HEVC_SLICE_I ?
					0 :
					(5 - sh->five_minus_max_num_merge_cand);
	// * SH DSS flag invented by me - but clearly needed
	s->dependent_slice_segment_flag =
		((sh->flags &
		  V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) != 0);

	s->nb_refs[0] = (sh->slice_type == HEVC_SLICE_I) ?
				0 :
				sh->num_ref_idx_l0_active_minus1 + 1;
	s->nb_refs[1] = (sh->slice_type != HEVC_SLICE_B) ?
				0 :
				sh->num_ref_idx_l1_active_minus1 + 1;

	if (s->sps.flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)
		populate_scaling_factors(run, de, s);

	ctb_addr_ts = s->ctb_addr_rs_to_ts[sh->slice_segment_addr];

	if ((s->pps.flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED))
		wpp_decode_slice(de, s, sh, ctb_addr_ts);
	else
		decode_slice(de, s, sh, ctb_addr_ts);

	if (!s->frame_end)
		return;

	// Frame end
	memset(dpb_q_aux, 0,
	       sizeof(*dpb_q_aux) * V4L2_HEVC_DPB_ENTRIES_NUM_MAX);
	/*
	 * Need Aux ents for all (ref) DPB ents if temporal MV could
	 * be enabled for any pic
	 * ** At the moment we have aux ents for all pics whether or not
	 *    they are ref
	 */
	use_aux = ((s->sps.flags &
		  V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED) != 0);

	// Locate ref frames
	// At least in the current implementation this is constant across all
	// slices. If this changes we will need idx mapping code.
	// Uses sh so here rather than trigger

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	if (!vq) {
		v4l2_err(&dev->v4l2_dev, "VQ gone!\n");
		goto fail;
	}

	//        v4l2_info(&dev->v4l2_dev, "rpivid_h265_end of frame\n");
	if (frame_end(dev, de, s))
		goto fail;

	for (i = 0; i < sh->num_active_dpb_entries; ++i) {
		int buffer_index =
			vb2_find_timestamp(vq, sh->dpb[i].timestamp, 0);
		struct vb2_buffer *buf = buffer_index < 0 ?
					NULL :
					vb2_get_buffer(vq, buffer_index);

		if (!buf) {
			v4l2_warn(&dev->v4l2_dev,
				  "Missing DPB ent %d, timestamp=%lld, index=%d\n",
				  i, (long long)sh->dpb[i].timestamp,
				  buffer_index);
			continue;
		}

		if (use_aux) {
			dpb_q_aux[i] = aux_q_ref(ctx,
						 ctx->aux_ents[buffer_index]);
			if (!dpb_q_aux[i])
				v4l2_warn(&dev->v4l2_dev,
					  "Missing DPB AUX ent %d index=%d\n",
					  i, buffer_index);
		}

		de->ref_addrs[i] =
			vb2_dma_contig_plane_dma_addr(buf, 0);
	}

	// Move DPB from temp
	for (i = 0; i != V4L2_HEVC_DPB_ENTRIES_NUM_MAX; ++i) {
		aux_q_release(ctx, &s->ref_aux[i]);
		s->ref_aux[i] = dpb_q_aux[i];
	}
	// Unref the old frame aux too - it is either in the DPB or not
	// now
	aux_q_release(ctx, &s->frame_aux);

	if (use_aux) {
		// New frame so new aux ent
		// ??? Do we need this if non-ref ??? can we tell
		s->frame_aux = aux_q_new(ctx, run->dst->vb2_buf.index);

		if (!s->frame_aux) {
			v4l2_err(&dev->v4l2_dev,
				 "Failed to obtain aux storage for frame\n");
			goto fail;
		}

		de->frame_aux = aux_q_ref(ctx, s->frame_aux);
	}

	if (de->dpbno_col != ~0U) {
		if (de->dpbno_col >= sh->num_active_dpb_entries) {
			v4l2_err(&dev->v4l2_dev,
				 "Col ref index %d >= %d\n",
				 de->dpbno_col,
				 sh->num_active_dpb_entries);
		} else {
			// Standard requires that the col pic is
			// constant for the duration of the pic
			// (text of collocated_ref_idx in H265-2 2018
			// 7.4.7.1)

			// Spot the collocated ref in passing
			de->col_aux = aux_q_ref(ctx,
						dpb_q_aux[de->dpbno_col]);

			if (!de->col_aux) {
				v4l2_warn(&dev->v4l2_dev,
					  "Missing DPB ent for col\n");
				// Probably need to abort if this fails
				// as P2 may explode on bad data
				goto fail;
			}
		}
	}

	de->state = RPIVID_DECODE_PHASE1;
	return;

fail:
	if (de)
		// Actual error reporting happens in Trigger
		de->state = s->frame_end ? RPIVID_DECODE_ERROR_DONE :
					   RPIVID_DECODE_ERROR_CONTINUE;
}

//////////////////////////////////////////////////////////////////////////////
// Handle PU and COEFF stream overflow

// Returns:
// -1  Phase 1 decode error
//  0  OK
// >0  Out of space (bitmask)

#define STATUS_COEFF_EXHAUSTED	8
#define STATUS_PU_EXHAUSTED	16

static int check_status(const struct rpivid_dev *const dev)
{
	const u32 cfstatus = apb_read(dev, RPI_CFSTATUS);
	const u32 cfnum = apb_read(dev, RPI_CFNUM);
	u32 status = apb_read(dev, RPI_STATUS);

	// Handle PU and COEFF stream overflow

	// this is the definition of successful completion of phase 1
	// it assures that status register is zero and all blocks in each tile
	// have completed
	if (cfstatus == cfnum)
		return 0;	//No error

	status &= (STATUS_PU_EXHAUSTED | STATUS_COEFF_EXHAUSTED);
	if (status)
		return status;

	return -1;
}

static void cb_phase2(struct rpivid_dev *const dev, void *v)
{
	struct rpivid_dec_env *const de = v;
	struct rpivid_ctx *const ctx = de->ctx;

	xtrace_in(dev, de);

	v4l2_m2m_cap_buf_return(dev->m2m_dev, ctx->fh.m2m_ctx, de->frame_buf,
				VB2_BUF_STATE_DONE);
	de->frame_buf = NULL;

	/* Delete de before finish as finish might immediately trigger a reuse
	 * of de
	 */
	dec_env_delete(de);

	if (atomic_add_return(-1, &ctx->p2out) >= RPIVID_P2BUF_COUNT - 1) {
		xtrace_fin(dev, de);
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_DONE);
	}

	xtrace_ok(dev, de);
}

static void phase2_claimed(struct rpivid_dev *const dev, void *v)
{
	struct rpivid_dec_env *const de = v;
	unsigned int i;

	xtrace_in(dev, de);

	apb_write_vc_addr(dev, RPI_PURBASE, de->pu_base_vc);
	apb_write_vc_len(dev, RPI_PURSTRIDE, de->pu_stride);
	apb_write_vc_addr(dev, RPI_COEFFRBASE, de->coeff_base_vc);
	apb_write_vc_len(dev, RPI_COEFFRSTRIDE, de->coeff_stride);

	apb_write_vc_addr(dev, RPI_OUTYBASE, de->frame_addr);
	apb_write_vc_addr(dev, RPI_OUTCBASE,
			  de->frame_addr + de->frame_c_offset);
	apb_write_vc_len(dev, RPI_OUTYSTRIDE, de->frame_stride);
	apb_write_vc_len(dev, RPI_OUTCSTRIDE, de->frame_stride);

	//    v4l2_info(&dev->v4l2_dev, "Frame: Y=%llx, C=%llx, Stride=%x\n",
	//              de->frame_addr, de->frame_addr + de->frame_c_offset,
	//              de->frame_stride);

	for (i = 0; i < 16; i++) {
		// Strides are in fact unused but fill in anyway
		apb_write_vc_addr(dev, 0x9000 + 16 * i, de->ref_addrs[i]);
		apb_write_vc_len(dev, 0x9004 + 16 * i, de->frame_stride);
		apb_write_vc_addr(dev, 0x9008 + 16 * i,
				  de->ref_addrs[i] + de->frame_c_offset);
		apb_write_vc_len(dev, 0x900C + 16 * i, de->frame_stride);
	}

	apb_write(dev, RPI_CONFIG2, de->rpi_config2);
	apb_write(dev, RPI_FRAMESIZE, de->rpi_framesize);
	apb_write(dev, RPI_CURRPOC, de->rpi_currpoc);
	//    v4l2_info(&dev->v4l2_dev, "Config2=%#x, FrameSize=%#x, POC=%#x\n",
	//    de->rpi_config2, de->rpi_framesize, de->rpi_currpoc);

	// collocated reads/writes
	apb_write_vc_len(dev, RPI_COLSTRIDE,
			 de->ctx->colmv_stride); // Read vals
	apb_write_vc_len(dev, RPI_MVSTRIDE,
			 de->ctx->colmv_stride); // Write vals
	apb_write_vc_addr(dev, RPI_MVBASE,
			  !de->frame_aux ? 0 : de->frame_aux->col.addr);
	apb_write_vc_addr(dev, RPI_COLBASE,
			  !de->col_aux ? 0 : de->col_aux->col.addr);

	//v4l2_info(&dev->v4l2_dev,
	//	   "Mv=%llx, Col=%llx, Stride=%x, Buf=%llx->%llx\n",
	//	   de->rpi_mvbase, de->rpi_colbase, de->ctx->colmv_stride,
	//	   de->ctx->colmvbuf.addr, de->ctx->colmvbuf.addr +
	//	   de->ctx->colmvbuf.size);

	rpivid_hw_irq_active2_irq(dev, &de->irq_ent, cb_phase2, de);

	apb_write_final(dev, RPI_NUMROWS, de->pic_height_in_ctbs_y);

	xtrace_ok(dev, de);
}

static void phase1_claimed(struct rpivid_dev *const dev, void *v);

static void phase1_thread(struct rpivid_dev *const dev, void *v)
{
	struct rpivid_dec_env *const de = v;
	struct rpivid_ctx *const ctx = de->ctx;

	struct rpivid_gptr *const pu_gptr = ctx->pu_bufs + ctx->p2idx;
	struct rpivid_gptr *const coeff_gptr = ctx->coeff_bufs + ctx->p2idx;

	xtrace_in(dev, de);

	if (de->p1_status & STATUS_PU_EXHAUSTED) {
		if (gptr_realloc_new(dev, pu_gptr, next_size(pu_gptr->size))) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: PU realloc (%#x) failed\n",
				 __func__, pu_gptr->size);
			goto fail;
		}
		v4l2_info(&dev->v4l2_dev, "%s: PU realloc (%#x) OK\n",
			  __func__, pu_gptr->size);
	}

	if (de->p1_status & STATUS_COEFF_EXHAUSTED) {
		if (gptr_realloc_new(dev, coeff_gptr,
				     next_size(coeff_gptr->size))) {
			v4l2_err(&dev->v4l2_dev,
				 "%s: Coeff realloc (%#x) failed\n",
				 __func__, coeff_gptr->size);
			goto fail;
		}
		v4l2_info(&dev->v4l2_dev, "%s: Coeff realloc (%#x) OK\n",
			  __func__, coeff_gptr->size);
	}

	phase1_claimed(dev, de);
	xtrace_ok(dev, de);
	return;

fail:
	dec_env_delete(de);
	xtrace_fin(dev, de);
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
					 VB2_BUF_STATE_ERROR);
	xtrace_fail(dev, de);
}

/* Always called in irq context (this is good) */
static void cb_phase1(struct rpivid_dev *const dev, void *v)
{
	struct rpivid_dec_env *const de = v;
	struct rpivid_ctx *const ctx = de->ctx;

	xtrace_in(dev, de);

	de->p1_status = check_status(dev);
	if (de->p1_status != 0) {
		v4l2_info(&dev->v4l2_dev, "%s: Post wait: %#x\n",
			  __func__, de->p1_status);

		if (de->p1_status < 0)
			goto fail;

		/* Need to realloc - push onto a thread rather than IRQ */
		rpivid_hw_irq_active1_thread(dev, &de->irq_ent,
					     phase1_thread, de);
		return;
	}

	/* After the frame-buf is detached it must be returned but from
	 * this point onward (phase2_claimed, cb_phase2) there are no error
	 * paths so the return at the end of cb_phase2 is all that is needed
	 */
	de->frame_buf = v4l2_m2m_cap_buf_detach(dev->m2m_dev, ctx->fh.m2m_ctx);
	if (!de->frame_buf) {
		v4l2_err(&dev->v4l2_dev, "%s: No detached buffer\n", __func__);
		goto fail;
	}

	ctx->p2idx =
		(ctx->p2idx + 1 >= RPIVID_P2BUF_COUNT) ? 0 : ctx->p2idx + 1;

	// Enable the next setup if our Q isn't too big
	if (atomic_add_return(1, &ctx->p2out) < RPIVID_P2BUF_COUNT) {
		xtrace_fin(dev, de);
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_DONE);
	}

	rpivid_hw_irq_active2_claim(dev, &de->irq_ent, phase2_claimed, de);

	xtrace_ok(dev, de);
	return;

fail:
	dec_env_delete(de);
	xtrace_fin(dev, de);
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
					 VB2_BUF_STATE_ERROR);
	xtrace_fail(dev, de);
}

static void phase1_claimed(struct rpivid_dev *const dev, void *v)
{
	struct rpivid_dec_env *const de = v;
	struct rpivid_ctx *const ctx = de->ctx;

	const struct rpivid_gptr * const pu_gptr = ctx->pu_bufs + ctx->p2idx;
	const struct rpivid_gptr * const coeff_gptr = ctx->coeff_bufs +
						      ctx->p2idx;

	xtrace_in(dev, de);

	de->pu_base_vc = pu_gptr->addr;
	de->pu_stride =
		ALIGN_DOWN(pu_gptr->size / de->pic_height_in_ctbs_y, 64);

	de->coeff_base_vc = coeff_gptr->addr;
	de->coeff_stride =
		ALIGN_DOWN(coeff_gptr->size / de->pic_height_in_ctbs_y, 64);

	apb_write_vc_addr(dev, RPI_PUWBASE, de->pu_base_vc);
	apb_write_vc_len(dev, RPI_PUWSTRIDE, de->pu_stride);
	apb_write_vc_addr(dev, RPI_COEFFWBASE, de->coeff_base_vc);
	apb_write_vc_len(dev, RPI_COEFFWSTRIDE, de->coeff_stride);

	// Trigger command FIFO
	apb_write(dev, RPI_CFNUM, de->cmd_len);

	// Claim irq
	rpivid_hw_irq_active1_irq(dev, &de->irq_ent, cb_phase1, de);

	// And start the h/w
	apb_write_vc_addr_final(dev, RPI_CFBASE, de->cmd_copy_gptr->addr);

	xtrace_ok(dev, de);
}

static void dec_state_delete(struct rpivid_ctx *const ctx)
{
	unsigned int i;
	struct rpivid_dec_state *const s = ctx->state;

	if (!s)
		return;
	ctx->state = NULL;

	free_ps_info(s);

	for (i = 0; i != HEVC_MAX_REFS; ++i)
		aux_q_release(ctx, &s->ref_aux[i]);
	aux_q_release(ctx, &s->frame_aux);

	kfree(s);
}

static void rpivid_h265_stop(struct rpivid_ctx *ctx)
{
	struct rpivid_dev *const dev = ctx->dev;
	unsigned int i;

	v4l2_info(&dev->v4l2_dev, "%s\n", __func__);

	dec_env_uninit(ctx);
	dec_state_delete(ctx);

	// dec_env & state must be killed before this to release the buffer to
	// the free pool
	aux_q_uninit(ctx);

	for (i = 0; i != ARRAY_SIZE(ctx->bitbufs); ++i)
		gptr_free(dev, ctx->bitbufs + i);
	for (i = 0; i != ARRAY_SIZE(ctx->cmdbufs); ++i)
		gptr_free(dev, ctx->cmdbufs + i);
	for (i = 0; i != ARRAY_SIZE(ctx->pu_bufs); ++i)
		gptr_free(dev, ctx->pu_bufs + i);
	for (i = 0; i != ARRAY_SIZE(ctx->coeff_bufs); ++i)
		gptr_free(dev, ctx->coeff_bufs + i);
}

static int rpivid_h265_start(struct rpivid_ctx *ctx)
{
	struct rpivid_dev *const dev = ctx->dev;
	unsigned int i;

	unsigned int w = ctx->dst_fmt.width;
	unsigned int h = ctx->dst_fmt.height;
	unsigned int wxh;
	size_t pu_alloc;
	size_t coeff_alloc;

	// Generate a sanitised WxH for memory alloc
	// Assume HD if unset
	if (w == 0)
		w = 1920;
	if (w > 4096)
		w = 4096;
	if (h == 0)
		h = 1088;
	if (h > 4096)
		h = 4096;
	wxh = w * h;

	v4l2_info(&dev->v4l2_dev, "%s: (%dx%d)\n", __func__,
		  ctx->dst_fmt.width, ctx->dst_fmt.height);

	ctx->dec0 = NULL;
	ctx->state = kzalloc(sizeof(*ctx->state), GFP_KERNEL);
	if (!ctx->state) {
		v4l2_err(&dev->v4l2_dev, "Failed to allocate decode state\n");
		goto fail;
	}

	if (dec_env_init(ctx) != 0) {
		v4l2_err(&dev->v4l2_dev, "Failed to allocate decode envs\n");
		goto fail;
	}

	// 16k is plenty for most purposes but we will realloc if needed
	for (i = 0; i != ARRAY_SIZE(ctx->cmdbufs); ++i) {
		if (gptr_alloc(dev, ctx->cmdbufs + i, 0x4000,
			       DMA_ATTR_FORCE_CONTIGUOUS))
			goto fail;
	}

	// Finger in the air PU & Coeff alloc
	// Will be realloced if too small
	coeff_alloc = round_up_size(wxh);
	pu_alloc = round_up_size(wxh / 4);
	for (i = 0; i != ARRAY_SIZE(ctx->pu_bufs); ++i) {
		// Don't actually need a kernel mapping here
		if (gptr_alloc(dev, ctx->pu_bufs + i, pu_alloc,
			       DMA_ATTR_FORCE_CONTIGUOUS |
					DMA_ATTR_NO_KERNEL_MAPPING))
			goto fail;
		if (gptr_alloc(dev, ctx->coeff_bufs + i, coeff_alloc,
			       DMA_ATTR_FORCE_CONTIGUOUS |
					DMA_ATTR_NO_KERNEL_MAPPING))
			goto fail;
	}
	aux_q_init(ctx);

	return 0;

fail:
	rpivid_h265_stop(ctx);
	return -ENOMEM;
}

static void rpivid_h265_trigger(struct rpivid_ctx *ctx)
{
	struct rpivid_dev *const dev = ctx->dev;
	struct rpivid_dec_env *const de = ctx->dec0;

	xtrace_in(dev, de);

	switch (!de ? RPIVID_DECODE_ERROR_CONTINUE : de->state) {
	case RPIVID_DECODE_SLICE_START:
		de->state = RPIVID_DECODE_SLICE_CONTINUE;
	/* FALLTHRU */
	case RPIVID_DECODE_SLICE_CONTINUE:
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_DONE);
		break;
	default:
		v4l2_err(&dev->v4l2_dev, "%s: Unexpected state: %d\n", __func__,
			 de->state);
	/* FALLTHRU */
	case RPIVID_DECODE_ERROR_DONE:
		ctx->dec0 = NULL;
		dec_env_delete(de);
	/* FALLTHRU */
	case RPIVID_DECODE_ERROR_CONTINUE:
		xtrace_fin(dev, de);
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_ERROR);
		break;
	case RPIVID_DECODE_PHASE1:
		ctx->dec0 = NULL;
		rpivid_hw_irq_active1_claim(dev, &de->irq_ent, phase1_claimed,
					    de);
		break;
	}

	xtrace_ok(dev, de);
}

struct rpivid_dec_ops rpivid_dec_ops_h265 = {
	.setup = rpivid_h265_setup,
	.start = rpivid_h265_start,
	.stop = rpivid_h265_stop,
	.trigger = rpivid_h265_trigger,
};
