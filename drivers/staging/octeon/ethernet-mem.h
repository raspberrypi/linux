/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This file is based on code from OCTEON SDK by Cavium Networks.
 *
 * Copyright (c) 2003-2007 Cavium Networks
 */

int cvm_oct_mem_fill_fpa(int pool, int size, int elements);
struct platform_device;

void cvm_oct_mem_empty_fpa(struct platform_device *pdev, int pool, int size,
			   int elements);
