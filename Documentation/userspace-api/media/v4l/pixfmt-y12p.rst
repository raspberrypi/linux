.. Permission is granted to copy, distribute and/or modify this
.. document under the terms of the GNU Free Documentation License,
.. Version 1.1 or any later version published by the Free Software
.. Foundation, with no Invariant Sections, no Front-Cover Texts
.. and no Back-Cover Texts. A copy of the license is included at
.. Documentation/media/uapi/fdl-appendix.rst.
..
.. TODO: replace it to GFDL-1.1-or-later WITH no-invariant-sections

.. _V4L2-PIX-FMT-Y12P:

******************************
V4L2_PIX_FMT_Y12P ('Y12P')
******************************

Grey-scale image as a MIPI RAW12 packed array


Description
===========

This is a packed grey-scale image format with a depth of 12 bits per
pixel. Two consecutive pixels are packed into 3 bytes. The first 2 bytes
contain the 8 high order bits of the pixels, and the 3rd byte contains the 4
least significants bits of each pixel, in the same order.

**Byte Order.**
Each cell is one byte.

.. tabularcolumns:: |p{2.2cm}|p{1.2cm}|p{1.2cm}|p{3.1cm}|


.. flat-table::
    :header-rows:  0
    :stub-columns: 0
    :widths:       2 1 1 1


    -  -  start + 0:
       -  Y'\ :sub:`00high`
       -  Y'\ :sub:`01high`
       -  Y'\ :sub:`01low`\ (bits 7--4)

          Y'\ :sub:`00low`\ (bits 3--0)

