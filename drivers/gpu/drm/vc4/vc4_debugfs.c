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

#include <linux/seq_file.h>
#include <linux/circ_buf.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <drm/drmP.h>

#include "vc4_drv.h"
#include "vc4_regs.h"

#define REGDEF(reg) { reg, #reg }
static const struct {
	uint32_t offset;
	const char *name;
} vc4_reg_defs[] = {
	REGDEF(V3D_IDENT0),
	REGDEF(V3D_IDENT1),
	REGDEF(V3D_IDENT2),
	REGDEF(V3D_SCRATCH),
	REGDEF(V3D_L2CACTL),
	REGDEF(V3D_SLCACTL),
	REGDEF(V3D_INTCTL),
	REGDEF(V3D_INTENA),
	REGDEF(V3D_INTDIS),
	REGDEF(V3D_CT0CS),
	REGDEF(V3D_CT1CS),
	REGDEF(V3D_CT0EA),
	REGDEF(V3D_CT1EA),
	REGDEF(V3D_CT0CA),
	REGDEF(V3D_CT1CA),
	REGDEF(V3D_CT00RA0),
	REGDEF(V3D_CT01RA0),
	REGDEF(V3D_CT0LC),
	REGDEF(V3D_CT1LC),
	REGDEF(V3D_CT0PC),
	REGDEF(V3D_CT1PC),
	REGDEF(V3D_PCS),
	REGDEF(V3D_BFC),
	REGDEF(V3D_RFC),
	REGDEF(V3D_BPCA),
	REGDEF(V3D_BPCS),
	REGDEF(V3D_BPOA),
	REGDEF(V3D_BPOS),
	REGDEF(V3D_BXCF),
	REGDEF(V3D_SQRSV0),
	REGDEF(V3D_SQRSV1),
	REGDEF(V3D_SQCNTL),
	REGDEF(V3D_SRQPC),
	REGDEF(V3D_SRQUA),
	REGDEF(V3D_SRQUL),
	REGDEF(V3D_SRQCS),
	REGDEF(V3D_VPACNTL),
	REGDEF(V3D_VPMBASE),
	REGDEF(V3D_PCTRC),
	REGDEF(V3D_PCTRE),
	REGDEF(V3D_PCTR0),
	REGDEF(V3D_PCTRS0),
	REGDEF(V3D_PCTR1),
	REGDEF(V3D_PCTRS1),
	REGDEF(V3D_PCTR2),
	REGDEF(V3D_PCTRS2),
	REGDEF(V3D_PCTR3),
	REGDEF(V3D_PCTRS3),
	REGDEF(V3D_PCTR4),
	REGDEF(V3D_PCTRS4),
	REGDEF(V3D_PCTR5),
	REGDEF(V3D_PCTRS5),
	REGDEF(V3D_PCTR6),
	REGDEF(V3D_PCTRS6),
	REGDEF(V3D_PCTR7),
	REGDEF(V3D_PCTRS7),
	REGDEF(V3D_PCTR8),
	REGDEF(V3D_PCTRS8),
	REGDEF(V3D_PCTR9),
	REGDEF(V3D_PCTRS9),
	REGDEF(V3D_PCTR10),
	REGDEF(V3D_PCTRS10),
	REGDEF(V3D_PCTR11),
	REGDEF(V3D_PCTRS11),
	REGDEF(V3D_PCTR12),
	REGDEF(V3D_PCTRS12),
	REGDEF(V3D_PCTR13),
	REGDEF(V3D_PCTRS13),
	REGDEF(V3D_PCTR14),
	REGDEF(V3D_PCTRS14),
	REGDEF(V3D_PCTR15),
	REGDEF(V3D_PCTRS15),
	REGDEF(V3D_BGE),
	REGDEF(V3D_FDBGO),
	REGDEF(V3D_FDBGB),
	REGDEF(V3D_FDBGR),
	REGDEF(V3D_FDBGS),
	REGDEF(V3D_ERRSTAT),
};

static int
vc4_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(vc4_reg_defs); i++) {
		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   vc4_reg_defs[i].name, vc4_reg_defs[i].offset,
			   VC4_READ(vc4_reg_defs[i].offset));
	}

	return 0;
}

static const struct {
	uint32_t offset;
	const char *name;
} hvs_reg_defs[] = {
	REGDEF(SCALER_DISPCTRL),
	REGDEF(SCALER_DISPSTAT),
	REGDEF(SCALER_DISPID),
	REGDEF(SCALER_DISPECTRL),
	REGDEF(SCALER_DISPPROF),
	REGDEF(SCALER_DISPDITHER),
	REGDEF(SCALER_DISPEOLN),
	REGDEF(SCALER_DISPLIST0),
	REGDEF(SCALER_DISPLIST1),
	REGDEF(SCALER_DISPLIST2),
	REGDEF(SCALER_DISPLSTAT),
	REGDEF(SCALER_DISPLACT0),
	REGDEF(SCALER_DISPLACT1),
	REGDEF(SCALER_DISPLACT2),
	REGDEF(SCALER_DISPCTRL0),
	REGDEF(SCALER_DISPBKGND0),
	REGDEF(SCALER_DISPSTAT0),
	REGDEF(SCALER_DISPBASE0),
	REGDEF(SCALER_DISPCTRL1),
	REGDEF(SCALER_DISPBKGND1),
	REGDEF(SCALER_DISPSTAT1),
	REGDEF(SCALER_DISPBASE1),
	REGDEF(SCALER_DISPCTRL2),
	REGDEF(SCALER_DISPBKGND2),
	REGDEF(SCALER_DISPSTAT2),
	REGDEF(SCALER_DISPBASE2),
	REGDEF(SCALER_DISPALPHA2),
};

static int
hvs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(hvs_reg_defs); i++) {
		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   hvs_reg_defs[i].name, hvs_reg_defs[i].offset,
			   HVS_READ(hvs_reg_defs[i].offset));
	}

	return 0;
}

static const struct drm_info_list vc4_debugfs_list[] = {
	{"vc4_regs", vc4_regs, 0},
	{"hvs_regs", hvs_regs, 0},
};
#define VC4_DEBUGFS_ENTRIES ARRAY_SIZE(vc4_debugfs_list)

int
vc4_debugfs_init(struct drm_minor *minor)
{
	return drm_debugfs_create_files(vc4_debugfs_list,
					VC4_DEBUGFS_ENTRIES,
					minor->debugfs_root, minor);
}

void
vc4_debugfs_cleanup(struct drm_minor *minor)
{
	drm_debugfs_remove_files(vc4_debugfs_list,
				 VC4_DEBUGFS_ENTRIES, minor);
}
