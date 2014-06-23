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

#define V3D_IDENT0   0x00000
# define VC4_EXPECTED_IDENT0 \
	((2 << 24) | \
	('V' << 0) | \
	('3' << 8) | \
	 ('D' << 16))

#define V3D_IDENT1   0x00004
#define V3D_IDENT2   0x00008
#define V3D_SCRATCH  0x00010
#define V3D_L2CACTL  0x00020
#define V3D_SLCACTL  0x00024
#define V3D_INTCTL   0x00030
#define V3D_INTENA   0x00034
#define V3D_INTDIS   0x00038

#define V3D_CT0CS    0x00100
#define V3D_CT1CS    0x00104
#define V3D_CTNCS(n) (V3D_CT0CS + 4 * n)
# define V3D_CTRSTA      (1 << 15)
# define V3D_CTSEMA      (1 << 12)
# define V3D_CTRTSD      (1 << 8)
# define V3D_CTRUN       (1 << 5)
# define V3D_CTSUBS      (1 << 4)
# define V3D_CTERR       (1 << 3)
# define V3D_CTMODE      (1 << 0)

#define V3D_CT0EA    0x00108
#define V3D_CT1EA    0x0010c
#define V3D_CTNEA(n) (V3D_CT0EA + 4 * (n))
#define V3D_CT0CA    0x00110
#define V3D_CT1CA    0x00114
#define V3D_CTNCA(n) (V3D_CT0CA + 4 * (n))
#define V3D_CT00RA0  0x00118
#define V3D_CT01RA0  0x0011c
#define V3D_CTNRA0(n) (V3D_CT00RA0 + 4 * (n))
#define V3D_CT0LC    0x00120
#define V3D_CT1LC    0x00124
#define V3D_CTNLC(n) (V3D_CT0LC + 4 * (n))
#define V3D_CT0PC    0x00128
#define V3D_CT1PC    0x0012c
#define V3D_CTNPC(n) (V3D_CT0PC + 4 * (n))

#define V3D_PCS      0x00130
# define V3D_BMOOM       (1 << 8)
# define V3D_RMBUSY      (1 << 3)
# define V3D_RMACTIVE    (1 << 2)
# define V3D_BMBUSY      (1 << 1)
# define V3D_BMACTIVE    (1 << 0)

#define V3D_BFC      0x00134
#define V3D_RFC      0x00138
#define V3D_BPCA     0x00300
#define V3D_BPCS     0x00304
#define V3D_BPOA     0x00308
#define V3D_BPOS     0x0030c
#define V3D_BXCF     0x00310
#define V3D_SQRSV0   0x00410
#define V3D_SQRSV1   0x00414
#define V3D_SQCNTL   0x00418
#define V3D_SRQPC    0x00430
#define V3D_SRQUA    0x00434
#define V3D_SRQUL    0x00438
#define V3D_SRQCS    0x0043c
#define V3D_VPACNTL  0x00500
#define V3D_VPMBASE  0x00504
#define V3D_PCTRC    0x00670
#define V3D_PCTRE    0x00674
#define V3D_PCTR0    0x00680
#define V3D_PCTRS0   0x00684
#define V3D_PCTR1    0x00688
#define V3D_PCTRS1   0x0068c
#define V3D_PCTR2    0x00690
#define V3D_PCTRS2   0x00694
#define V3D_PCTR3    0x00698
#define V3D_PCTRS3   0x0069c
#define V3D_PCTR4    0x006a0
#define V3D_PCTRS4   0x006a4
#define V3D_PCTR5    0x006a8
#define V3D_PCTRS5   0x006ac
#define V3D_PCTR6    0x006b0
#define V3D_PCTRS6   0x006b4
#define V3D_PCTR7    0x006b8
#define V3D_PCTRS7   0x006bc
#define V3D_PCTR8    0x006c0
#define V3D_PCTRS8   0x006c4
#define V3D_PCTR9    0x006c8
#define V3D_PCTRS9   0x006cc
#define V3D_PCTR10   0x006d0
#define V3D_PCTRS10  0x006d4
#define V3D_PCTR11   0x006d8
#define V3D_PCTRS11  0x006dc
#define V3D_PCTR12   0x006e0
#define V3D_PCTRS12  0x006e4
#define V3D_PCTR13   0x006e8
#define V3D_PCTRS13  0x006ec
#define V3D_PCTR14   0x006f0
#define V3D_PCTRS14  0x006f4
#define V3D_PCTR15   0x006f8
#define V3D_PCTRS15  0x006fc
#define V3D_BGE      0x00f00
#define V3D_FDBGO    0x00f04
#define V3D_FDBGB    0x00f08
#define V3D_FDBGR    0x00f0c
#define V3D_FDBGS    0x00f10
#define V3D_ERRSTAT  0x00f20

#define SCALER_DISPCTRL                         0x00000000
#define SCALER_DISPSTAT                         0x00000004
#define SCALER_DISPID                           0x00000008
#define SCALER_DISPECTRL                        0x0000000c
#define SCALER_DISPPROF                         0x00000010
#define SCALER_DISPDITHER                       0x00000014
#define SCALER_DISPEOLN                         0x00000018
#define SCALER_DISPLIST0                        0x00000020
#define SCALER_DISPLIST1                        0x00000024
#define SCALER_DISPLIST2                        0x00000028
#define SCALER_DISPLSTAT                        0x0000002c
#define SCALER_DISPLACT0                        0x00000030
#define SCALER_DISPLACT1                        0x00000034
#define SCALER_DISPLACT2                        0x00000038
#define SCALER_DISPCTRL0                        0x00000040
#define SCALER_DISPBKGND0                       0x00000044
#define SCALER_DISPSTAT0                        0x00000048
#define SCALER_DISPBASE0                        0x0000004c
#define SCALER_DISPCTRL1                        0x00000050
#define SCALER_DISPBKGND1                       0x00000054
#define SCALER_DISPSTAT1                        0x00000058
#define SCALER_DISPBASE1                        0x0000005c
#define SCALER_DISPCTRL2                        0x00000060
#define SCALER_DISPBKGND2                       0x00000064
#define SCALER_DISPSTAT2                        0x00000068
#define SCALER_DISPBASE2                        0x0000006c
#define SCALER_DISPALPHA2                       0x00000070
#define SCALER_GAMADDR                          0x00000078
#define SCALER_GAMDATA                          0x000000e0

/* HVS display list information. */
#define HVS_BOOTLOADER_DLIST_END                32

enum hvs_pixel_format {
	HVS_PIXEL_FORMAT_RGB4444 = 1,
	HVS_PIXEL_FORMAT_RGB555 = 2,
	HVS_PIXEL_FORMAT_RGB5551 = 3,
	HVS_PIXEL_FORMAT_RGB565 = 4,
	HVS_PIXEL_FORMAT_RGB888 = 5,
	HVS_PIXEL_FORMAT_RGB6666 = 6,
	HVS_PIXEL_FORMAT_RGB8888 = 7
};

/* Note: the LSB is the rightmost character shown.  Only valid for
 * HVS_PIXEL_FORMAT_RGB8888, not RGB888.
 */
enum hvs_pixel_order {
	HVS_PIXEL_ORDER_RGBA,
	HVS_PIXEL_ORDER_BGRA,
	HVS_PIXEL_ORDER_ARGB,
	HVS_PIXEL_ORDER_ABGR,
};

#define SCALER_CTL0_END				0x80000000
#define SCALER_CTL0_VALID			0x40000000

#define SCALER_CTL0_SIZE_MASK			0x3f000000
#define SCALER_CTL0_SIZE_SHIFT			24

#define SCALER_CTL0_HFLIP                       (1 << 16)
#define SCALER_CTL0_VFLIP                       (1 << 15)

#define SCALER_CTL0_ORDER_MASK			0x00006000
#define SCALER_CTL0_ORDER_SHIFT			13

/* Set to indicate no scaling. */
#define SCALER_CTL0_UNITY			(1 << 4)

#define SCALER_CTL0_PIXEL_FORMAT_MASK		0x0000000f
#define SCALER_CTL0_PIXEL_FORMAT_SHIFT		0

#define SCALER_POS0_ALPHA_MASK			0xff000000
#define SCALER_POS0_ALPHA_SHIFT			24

#define SCALER_POS0_START_Y_MASK		0x007fe000
#define SCALER_POS0_START_Y_SHIFT		13

#define SCALER_POS0_START_X_MASK		0x00001fff
#define SCALER_POS0_START_X_SHIFT		0

#define SCALER_POS2_ALPHA_MODE_MASK		0xc0000000
#define SCALER_POS2_ALPHA_MODE_SHIFT		30

#define SCALER_POS2_HEIGHT_MASK			0x0fff0000
#define SCALER_POS2_HEIGHT_SHIFT		16

#define SCALER_POS2_WIDTH_MASK			0x00000fff
#define SCALER_POS2_WIDTH_SHIFT			0

#define SCALER_SRC_PITCH_MASK			0x0000ffff
