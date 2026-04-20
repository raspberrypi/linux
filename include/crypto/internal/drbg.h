/* SPDX-License-Identifier: GPL-2.0 */

/*
 * NIST SP800-90A DRBG derivation function
 *
 * Copyright (C) 2014, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _INTERNAL_DRBG_H
#define _INTERNAL_DRBG_H

/*
 * Concatenation Helper and string operation helper
 *
 * SP800-90A requires the concatenation of different data. To avoid copying
 * buffers around or allocate additional memory, the following data structure
 * is used to point to the original memory with its size. In addition, it
 * is used to build a linked list. The linked list defines the concatenation
 * of individual buffers. The order of memory block referenced in that
 * linked list determines the order of concatenation.
 */
struct drbg_string {
	const unsigned char *buf;
	size_t len;
	struct list_head list;
};

static inline void drbg_string_fill(struct drbg_string *string,
				    const unsigned char *buf, size_t len)
{
	string->buf = buf;
	string->len = len;
	INIT_LIST_HEAD(&string->list);
}

#endif //_INTERNAL_DRBG_H
