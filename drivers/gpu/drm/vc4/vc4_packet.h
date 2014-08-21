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

#ifndef VC4_PACKET_H
#define VC4_PACKET_H

enum vc4_packet {
        VC4_PACKET_HALT = 0,
        VC4_PACKET_NOP = 1,

        VC4_PACKET_FLUSH = 4,
        VC4_PACKET_FLUSH_ALL = 5,
        VC4_PACKET_START_TILE_BINNING = 6,
        VC4_PACKET_INCREMENT_SEMAPHORE = 7,
        VC4_PACKET_WAIT_ON_SEMAPHORE = 8,

        VC4_PACKET_BRANCH = 16,
        VC4_PACKET_BRANCH_TO_SUB_LIST = 17,

        VC4_PACKET_STORE_MS_TILE_BUFFER = 24,
        VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF = 25,
        VC4_PACKET_STORE_FULL_RES_TILE_BUFFER = 26,
        VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER = 27,
        VC4_PACKET_STORE_TILE_BUFFER_GENERAL = 28,
        VC4_PACKET_LOAD_TILE_BUFFER_GENERAL = 29,

        VC4_PACKET_GL_INDEXED_PRIMITIVE = 32,
        VC4_PACKET_GL_ARRAY_PRIMITIVE = 33,

        VC4_PACKET_COMPRESSED_PRIMITIVE = 48,
        VC4_PACKET_CLIPPED_COMPRESSED_PRIMITIVE = 49,

        VC4_PACKET_PRIMITIVE_LIST_FORMAT = 56,

        VC4_PACKET_GL_SHADER_STATE = 64,
        VC4_PACKET_NV_SHADER_STATE = 65,
        VC4_PACKET_VG_SHADER_STATE = 66,

        VC4_PACKET_CONFIGURATION_BITS = 96,
        VC4_PACKET_FLAT_SHADE_FLAGS = 97,
        VC4_PACKET_POINT_SIZE = 98,
        VC4_PACKET_LINE_WIDTH = 99,
        VC4_PACKET_RHT_X_BOUNDARY = 100,
        VC4_PACKET_DEPTH_OFFSET = 101,
        VC4_PACKET_CLIP_WINDOW = 102,
        VC4_PACKET_VIEWPORT_OFFSET = 103,
        VC4_PACKET_Z_CLIPPING = 104,
        VC4_PACKET_CLIPPER_XY_SCALING = 105,
        VC4_PACKET_CLIPPER_Z_SCALING = 106,

        VC4_PACKET_TILE_BINNING_MODE_CONFIG = 112,
        VC4_PACKET_TILE_RENDERING_MODE_CONFIG = 113,
        VC4_PACKET_CLEAR_COLORS = 114,
        VC4_PACKET_TILE_COORDINATES = 115,

        /* Not an actual hardware packet -- this is what we use to put
         * references to GEM bos in the command stream, since we need the u32
         * int the actual address packet in order to store the offset from the
         * start of the BO.
         */
        VC4_PACKET_GEM_HANDLES = 254,
} __attribute__ ((__packed__));

/** @{
 * Bits used by packets like VC4_PACKET_STORE_TILE_BUFFER_GENERAL and
 * VC4_PACKET_TILE_RENDERING_MODE_CONFIG.
*/
#define VC4_TILING_FORMAT_LINEAR    0
#define VC4_TILING_FORMAT_T         1
#define VC4_TILING_FORMAT_LT        2
/** @} */

/** @{
 *
 * byte 2 of VC4_PACKET_STORE_TILE_BUFFER_GENERAL and
 * VC4_PACKET_LOAD_TILE_BUFFER_GENERAL (low bits of the address)
 */

#define VC4_LOADSTORE_TILE_BUFFER_DISABLE_FULL_VG_MASK (1 << 2)
#define VC4_LOADSTORE_TILE_BUFFER_DISABLE_FULL_ZS      (1 << 1)
#define VC4_LOADSTORE_TILE_BUFFER_DISABLE_FULL_COLOR   (1 << 0)

/** @} */

/** @{
 *
 * byte 1 of VC4_PACKET_STORE_TILE_BUFFER_GENERAL and
 * VC4_PACKET_LOAD_TILE_BUFFER_GENERAL
 */
#define VC4_STORE_TILE_BUFFER_DISABLE_VG_MASK_CLEAR (1 << 7)
#define VC4_STORE_TILE_BUFFER_DISABLE_ZS_CLEAR     (1 << 6)
#define VC4_STORE_TILE_BUFFER_DISABLE_COLOR_CLEAR  (1 << 5)
#define VC4_STORE_TILE_BUFFER_DISABLE_SWAP         (1 << 4)

#define VC4_LOADSTORE_TILE_BUFFER_RGBA8888         (0 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_BGR565_DITHER    (1 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_BGR565           (2 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_MASK             (3 << 0)
/** @} */

/** @{
 *
 * byte 0 of VC4_PACKET_STORE_TILE_BUFFER_GENERAL and
 * VC4_PACKET_LOAD_TILE_BUFFER_GENERAL
 */
#define VC4_STORE_TILE_BUFFER_MODE_SAMPLE0         (0 << 6)
#define VC4_STORE_TILE_BUFFER_MODE_DECIMATE_X4     (1 << 6)
#define VC4_STORE_TILE_BUFFER_MODE_DECIMATE_X16    (2 << 6)

/** The values of the field are VC4_TILING_FORMAT_* */
#define VC4_LOADSTORE_TILE_BUFFER_FORMAT_MASK      (3 << 4)
#define VC4_LOADSTORE_TILE_BUFFER_FORMAT_SHIFT     4


#define VC4_LOADSTORE_TILE_BUFFER_NONE             (0 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_COLOR            (1 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_ZS               (2 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_Z                (3 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_VG_MASK          (4 << 0)
#define VC4_LOADSTORE_TILE_BUFFER_FULL             (5 << 0)
/** @} */

#define VC4_INDEX_BUFFER_U8                        (0 << 4)
#define VC4_INDEX_BUFFER_U16                       (1 << 4)

/* This flag is only present in NV shader state. */
#define VC4_SHADER_FLAG_SHADED_CLIP_COORDS         (1 << 3)
#define VC4_SHADER_FLAG_ENABLE_CLIPPING            (1 << 2)
#define VC4_SHADER_FLAG_VS_POINT_SIZE              (1 << 1)
#define VC4_SHADER_FLAG_FS_SINGLE_THREAD           (1 << 0)

/** @{ byte 2 of config bits. */
#define VC4_CONFIG_BITS_EARLY_Z_UPDATE             (1 << 1)
#define VC4_CONFIG_BITS_EARLY_Z                    (1 << 0)
/** @} */

/** @{ byte 1 of config bits. */
#define VC4_CONFIG_BITS_Z_UPDATE                   (1 << 7)
/** same values in this 3-bit field as PIPE_FUNC_* */
#define VC4_CONFIG_BITS_DEPTH_FUNC_SHIFT           4
#define VC4_CONFIG_BITS_COVERAGE_READ_LEAVE        (1 << 3)

#define VC4_CONFIG_BITS_COVERAGE_UPDATE_NONZERO    (0 << 1)
#define VC4_CONFIG_BITS_COVERAGE_UPDATE_ODD        (1 << 1)
#define VC4_CONFIG_BITS_COVERAGE_UPDATE_OR         (2 << 1)
#define VC4_CONFIG_BITS_COVERAGE_UPDATE_ZERO       (3 << 1)

#define VC4_CONFIG_BITS_COVERAGE_PIPE_SELECT       (1 << 0)
/** @} */

/** @{ byte 0 of config bits. */
#define VC4_CONFIG_BITS_RASTERIZER_OVERSAMPLE_NONE (0 << 6)
#define VC4_CONFIG_BITS_RASTERIZER_OVERSAMPLE_4X   (1 << 6)
#define VC4_CONFIG_BITS_RASTERIZER_OVERSAMPLE_16X  (2 << 6)

#define VC4_CONFIG_BITS_AA_POINTS_AND_LINES        (1 << 4)
#define VC4_CONFIG_BITS_ENABLE_DEPTH_OFFSET        (1 << 3)
#define VC4_CONFIG_BITS_CW_PRIMITIVES              (1 << 2)
#define VC4_CONFIG_BITS_ENABLE_PRIM_BACK           (1 << 1)
#define VC4_CONFIG_BITS_ENABLE_PRIM_FRONT          (1 << 0)
/** @} */

/** @{ bits in the last u8 of VC4_PACKET_TILE_BINNING_MODE_CONFIG */
#define VC4_BIN_CONFIG_DB_NON_MS                   (1 << 7)

#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_32         (0 << 5)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_64         (1 << 5)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_128        (2 << 5)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_256        (3 << 5)

#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_32    (0 << 3)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_64    (1 << 3)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_128   (2 << 3)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_256   (3 << 3)

#define VC4_BIN_CONFIG_AUTO_INIT_TSDA              (1 << 2)
#define VC4_BIN_CONFIG_TILE_BUFFER_64BIT           (1 << 1)
#define VC4_BIN_CONFIG_MS_MODE_4X                  (1 << 0)
/** @} */

/** @{ bits in the last u16 of VC4_PACKET_TILE_RENDERING_MODE_CONFIG */
#define VC4_RENDER_CONFIG_DB_NON_MS                (1 << 12)
#define VC4_RENDER_CONFIG_EARLY_Z_COVERAGE_DISABLE (1 << 11)
#define VC4_RENDER_CONFIG_EARLY_Z_DIRECTION_G      (1 << 10)
#define VC4_RENDER_CONFIG_COVERAGE_MODE            (1 << 9)
#define VC4_RENDER_CONFIG_ENABLE_VG_MASK           (1 << 8)

/** The values of the field are VC4_TILING_FORMAT_* */
#define VC4_RENDER_CONFIG_MEMORY_FORMAT_MASK       (3 << 6)
#define VC4_RENDER_CONFIG_MEMORY_FORMAT_SHIFT      6

#define VC4_RENDER_CONFIG_DECIMATE_MODE_1X         (0 << 4)
#define VC4_RENDER_CONFIG_DECIMATE_MODE_4X         (1 << 4)
#define VC4_RENDER_CONFIG_DECIMATE_MODE_16X        (2 << 4)

#define VC4_RENDER_CONFIG_FORMAT_BGR565            (0 << 2)
#define VC4_RENDER_CONFIG_FORMAT_RGBA8888          (1 << 2)
#define VC4_RENDER_CONFIG_FORMAT_BGR565_DITHERED   (2 << 2)
#define VC4_RENDER_CONFIG_FORMAT_MASK              (3 << 2)

#define VC4_RENDER_CONFIG_TILE_BUFFER_64BIT        (1 << 1)
#define VC4_RENDER_CONFIG_MS_MODE_4X               (1 << 0)


enum vc4_texture_data_type {
        VC4_TEXTURE_TYPE_RGBA8888 = 0,
        VC4_TEXTURE_TYPE_RGBX8888 = 1,
        VC4_TEXTURE_TYPE_RGBA4444 = 2,
        VC4_TEXTURE_TYPE_RGBA5551 = 3,
        VC4_TEXTURE_TYPE_RGB565 = 4,
        VC4_TEXTURE_TYPE_LUMINANCE = 5,
        VC4_TEXTURE_TYPE_ALPHA = 6,
        VC4_TEXTURE_TYPE_LUMALPHA = 7,
        VC4_TEXTURE_TYPE_ETC1 = 8,
        VC4_TEXTURE_TYPE_S16F = 9,
        VC4_TEXTURE_TYPE_S8 = 10,
        VC4_TEXTURE_TYPE_S16 = 11,
        VC4_TEXTURE_TYPE_BW1 = 12,
        VC4_TEXTURE_TYPE_A4 = 13,
        VC4_TEXTURE_TYPE_A1 = 14,
        VC4_TEXTURE_TYPE_RGBA64 = 15,
        VC4_TEXTURE_TYPE_RGBA32R = 16,
        VC4_TEXTURE_TYPE_YUV422R = 17,
};

#endif /* VC4_PACKET_H */
