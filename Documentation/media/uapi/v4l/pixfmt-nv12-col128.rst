.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _V4L2_PIX_FMT_NV12_COL128:
.. _V4L2_PIX_FMT_NV12_10_COL128:

********************************************************************************
V4L2_PIX_FMT_NV12_COL128, V4L2_PIX_FMT_NV12_10_COL128
********************************************************************************


V4L2_PIX_FMT_NV21_COL128
Formats with ½ horizontal and vertical chroma resolution. This format
has two planes - one for luminance and one for chrominance. Chroma
samples are interleaved. The difference to ``V4L2_PIX_FMT_NV12`` is the
memory layout. The image is split into columns of 128 bytes wide rather than
being in raster order.

V4L2_PIX_FMT_NV12_10_COL128
Follows the same pattern as ``V4L2_PIX_FMT_NV21_COL128`` with 128 byte, but is
a 10bit format with 3 10-bit samples being packed into 4 bytes. Each 128 byte
wide column therefore contains 96 samples.


Description
===========

This is the two-plane versions of the YUV 4:2:0 format where data is
grouped into 128 byte wide columns. The three components are separated into
two sub-images or planes. The Y plane has one byte per pixel and pixels
are grouped into 128 byte wide columns. The CbCr plane has the same width,
in bytes, as the Y plane (and the image), but is half as tall in pixels.
The chroma plane is also in 128 byte columns, reflecting 64 Cb and 64 Cr
samples.

The chroma samples for a column follow the luma samples. If there is any
paddding, then that will be reflected via the selection API.
The luma height must be a multiple of 2 lines.

The normal bytesperline is effectively fixed at 128. However the format
requires knowledge of the stride between columns, therefore the bytesperline
value has been repurposed to denote the number of 128 byte long lines between
the start of each column.

**Byte Order.**


.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 12 12 12 12 4 12 12 12 12

    * - start + 0:
      - Y'\ :sub:`0,0`
      - Y'\ :sub:`0,1`
      - Y'\ :sub:`0,2`
      - Y'\ :sub:`0,3`
      - ...
      - Y'\ :sub:`0,124`
      - Y'\ :sub:`0,125`
      - Y'\ :sub:`0,126`
      - Y'\ :sub:`0,127`
    * - start + 128:
      - Y'\ :sub:`1,0`
      - Y'\ :sub:`1,1`
      - Y'\ :sub:`1,2`
      - Y'\ :sub:`1,3`
      - ...
      - Y'\ :sub:`1,124`
      - Y'\ :sub:`1,125`
      - Y'\ :sub:`1,126`
      - Y'\ :sub:`1,127`
    * - start + 256:
      - Y'\ :sub:`2,0`
      - Y'\ :sub:`2,1`
      - Y'\ :sub:`2,2`
      - Y'\ :sub:`2,3`
      - ...
      - Y'\ :sub:`2,124`
      - Y'\ :sub:`2,125`
      - Y'\ :sub:`2,126`
      - Y'\ :sub:`2,127`
    * - ...
      - ...
      - ...
      - ...
      - ...
      - ...
      - ...
      - ...
    * - start + ((height-1) * 128):
      - Y'\ :sub:`height-1,0`
      - Y'\ :sub:`height-1,1`
      - Y'\ :sub:`height-1,2`
      - Y'\ :sub:`height-1,3`
      - ...
      - Y'\ :sub:`height-1,124`
      - Y'\ :sub:`height-1,125`
      - Y'\ :sub:`height-1,126`
      - Y'\ :sub:`height-1,127`
    * - start + ((height) * 128):
      - Cb\ :sub:`0,0`
      - Cr\ :sub:`0,0`
      - Cb\ :sub:`0,1`
      - Cr\ :sub:`0,1`
      - ...
      - Cb\ :sub:`0,62`
      - Cr\ :sub:`0,62`
      - Cb\ :sub:`0,63`
      - Cr\ :sub:`0,63`
    * - start + ((height+1) * 128):
      - Cb\ :sub:`1,0`
      - Cr\ :sub:`1,0`
      - Cb\ :sub:`1,1`
      - Cr\ :sub:`1,1`
      - ...
      - Cb\ :sub:`1,62`
      - Cr\ :sub:`1,62`
      - Cb\ :sub:`1,63`
      - Cr\ :sub:`1,63`
    * - ...
      - ...
      - ...
      - ...
      - ...
      - ...
      - ...
      - ...
    * - start + ((height+(height/2)-1) * 128):
      - Cb\ :sub:`(height/2)-1,0`
      - Cr\ :sub:`(height/2)-1,0`
      - Cb\ :sub:`(height/2)-1,1`
      - Cr\ :sub:`(height/2)-1,1`
      - ...
      - Cb\ :sub:`(height/2)-1,62`
      - Cr\ :sub:`(height/2)-1,62`
      - Cb\ :sub:`(height/2)-1,63`
      - Cr\ :sub:`(height/2)-1,63`
    * - start + (bytesperline * 128):
      - Y'\ :sub:`0,128`
      - Y'\ :sub:`0,129`
      - Y'\ :sub:`0,130`
      - Y'\ :sub:`0,131`
      - ...
      - Y'\ :sub:`0,252`
      - Y'\ :sub:`0,253`
      - Y'\ :sub:`0,254`
      - Y'\ :sub:`0,255`
    * - ...
      - ...
      - ...
      - ...
      - ...
      - ...
      - ...
      - ...

V4L2_PIX_FMT_NV12_10_COL128 uses the same 128 byte column structure, but
encodes 10-bit YUV.
3 10-bit values are packed into 4 bytes as bits 9:0, 19:10, and 29:20, with
bits 30 & 31 unused. For the luma plane, bits 9:0 are Y0, 19:10 are Y1, and
29:20 are Y2. For the chroma plane the samples always come in pairs of Cr
and Cb, so it needs to be considered 6 values packed in 8 bytes.

Bit-packed representation.

.. raw:: latex

    \small

.. tabularcolumns:: |p{1.2cm}||p{1.2cm}||p{1.2cm}||p{1.2cm}|p{3.2cm}|p{3.2cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 8 8 8 8

    * - Y'\ :sub:`00[7:0]`
      - Y'\ :sub:`01[5:0] (bits 7--2)` Y'\ :sub:`00[9:8]`\ (bits 1--0)
      - Y'\ :sub:`02[3:0] (bits 7--4)` Y'\ :sub:`01[9:6]`\ (bits 3--0)
      - unused (bits 7--6)` Y'\ :sub:`02[9:4]`\ (bits 5--0)

.. raw:: latex

    \small

.. tabularcolumns:: |p{1.2cm}||p{1.2cm}||p{1.2cm}||p{1.2cm}|p{3.2cm}|p{3.2cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths: 12 12 12 12 12 12 12 12

    * - Cb\ :sub:`00[7:0]`
      - Cr\ :sub:`00[5:0]`\ (bits 7--2) Cb\ :sub:`00[9:8]`\ (bits 1--0)
      - Cb\ :sub:`01[3:0]`\ (bits 7--4) Cr\ :sub:`00[9:6]`\ (bits 3--0)
      - unused (bits 7--6) Cb\ :sub:`02[9:4]`\ (bits 5--0)
      - Cr\ :sub:`01[7:0]`
      - Cb\ :sub:`02[5:0]`\ (bits 7--2) Cr\ :sub:`01[9:8]`\ (bits 1--0)
      - Cr\ :sub:`02[3:0]`\ (bits 7--4) Cb\ :sub:`02[9:6]`\ (bits 3--0)
      - unused (bits 7--6) Cr\ :sub:`02[9:4]`\ (bits 5--0)

.. raw:: latex

    \normalsize




