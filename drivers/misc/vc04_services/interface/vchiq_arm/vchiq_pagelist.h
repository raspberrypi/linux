/*
 * Copyright (c) 2010-2011 Broadcom Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef VCHIQ_PAGELIST_H
#define VCHIQ_PAGELIST_H

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define CACHE_LINE_SIZE 32
#define PAGELIST_WRITE 0
#define PAGELIST_READ 1
#define PAGELIST_READ_WITH_FRAGMENTS 2

typedef struct pagelist_struct {
	unsigned long length;
	unsigned short type;
	unsigned short offset;
	unsigned long addrs[1];	/* N.B. 12 LSBs hold the number of following
				   pages at consecutive addresses. */
} PAGELIST_T;

typedef struct fragments_struct {
	char headbuf[CACHE_LINE_SIZE];
	char tailbuf[CACHE_LINE_SIZE];
} FRAGMENTS_T;

#endif /* VCHIQ_PAGELIST_H */
