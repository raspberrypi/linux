.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _V4L2-PIX-FMT-Y14P:

**************************
V4L2_PIX_FMT_Y14P ('Y14P')
**************************

Grey-scale image as a MIPI RAW14 packed array


Description
===========

This is a packed grey-scale image format with a depth of 14 bits per
pixel. Every four consecutive samples are packed into seven bytes. Each
of the first four bytes contain the eight high order bits of the pixels,
and the three following bytes contains the six least significants bits of
each pixel, in the same order.

**Byte Order.**
Each cell is one byte.

.. tabularcolumns:: |p{1.8cm}|p{1.0cm}|p{1.0cm}|p{1.0cm}|p{1.1cm}|p{3.3cm}|p{3.3cm}|p{3.3cm}|

.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       2 1 1 1 1 3 3 3


    -  -  start + 0:
       -  Y'\ :sub:`00high`
       -  Y'\ :sub:`01high`
       -  Y'\ :sub:`02high`
       -  Y'\ :sub:`03high`
       -  Y'\ :sub:`01low bits 1--0`\ (bits 7--6)

	  Y'\ :sub:`00low bits 5--0`\ (bits 5--0)

       -  Y'\ :sub:`02low bits 3--0`\ (bits 7--4)

	  Y'\ :sub:`01low bits 5--2`\ (bits 3--0)

       -  Y'\ :sub:`03low bits 5--0`\ (bits 7--2)

	  Y'\ :sub:`02low bits 5--4`\ (bits 1--0)
