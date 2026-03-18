/* SPDX-License-Identifier: GPL-2.0+ */
/*
 *  Copyright IBM Corp. 2001, 2012
 *  Author(s): Robert Burroughs
 *	       Eric Rossman (edrossma@us.ibm.com)
 *
 *  Hotplug & misc device support: Jochen Roehrig (roehrig@de.ibm.com)
 *  Major cleanup & driver split: Martin Schwidefsky <schwidefsky@de.ibm.com>
 *  MSGTYPE restruct:		  Holger Dengler <hd@linux.vnet.ibm.com>
 */

#ifndef _ZCRYPT_MSGTYPE6_H_
#define _ZCRYPT_MSGTYPE6_H_

#include <asm/zcrypt.h>

#define MSGTYPE06_NAME			"zcrypt_msgtype6"
#define MSGTYPE06_VARIANT_DEFAULT	0
#define MSGTYPE06_VARIANT_NORNG		1
#define MSGTYPE06_VARIANT_EP11		2

/**
 * The type 6 message family is associated with CEXxC/CEXxP cards.
 *
 * It contains a message header followed by a CPRB, both of which
 * are described below.
 *
 * Note that all reserved fields must be zeroes.
 */
struct type6_hdr {
	unsigned char reserved1;	/* 0x00				*/
	unsigned char type;		/* 0x06				*/
	unsigned char reserved2[2];	/* 0x0000			*/
	unsigned char right[4];		/* 0x00000000			*/
	unsigned char reserved3[2];	/* 0x0000			*/
	unsigned char reserved4[2];	/* 0x0000			*/
	unsigned char apfs[4];		/* 0x00000000			*/
	unsigned int  offset1;		/* 0x00000058 (offset to CPRB)	*/
	unsigned int  offset2;		/* 0x00000000			*/
	unsigned int  offset3;		/* 0x00000000			*/
	unsigned int  offset4;		/* 0x00000000			*/
	unsigned char agent_id[16];	/* 0x4341000000000000		*/
					/* 0x0000000000000000		*/
	unsigned char rqid[2];		/* rqid.  internal to 603	*/
	unsigned char reserved5[2];	/* 0x0000			*/
	unsigned char function_code[2];	/* for PKD, 0x5044 (ascii 'PD')	*/
	unsigned char reserved6[2];	/* 0x0000			*/
	unsigned int  tocardlen1;	/* (request CPRB len + 3) & -4	*/
	unsigned int  tocardlen2;	/* db len 0x00000000 for PKD	*/
	unsigned int  tocardlen3;	/* 0x00000000			*/
	unsigned int  tocardlen4;	/* 0x00000000			*/
	unsigned int  fromcardlen1;	/* response buffer length	*/
	unsigned int  fromcardlen2;	/* db len 0x00000000 for PKD	*/
	unsigned int  fromcardlen3;	/* 0x00000000			*/
	unsigned int  fromcardlen4;	/* 0x00000000			*/
} __packed;

/**
 * The type 86 message family is associated with CEXxC/CEXxP cards.
 *
 * It contains a message header followed by a CPRB.  The CPRB is
 * the same as the request CPRB, which is described above.
 *
 * If format is 1, an error condition exists and no data beyond
 * the 8-byte message header is of interest.
 *
 * The non-error message is shown below.
 *
 * Note that all reserved fields must be zeroes.
 */
struct type86_hdr {
	unsigned char reserved1;	/* 0x00				*/
	unsigned char type;		/* 0x86				*/
	unsigned char format;		/* 0x01 (error) or 0x02 (ok)	*/
	unsigned char reserved2;	/* 0x00				*/
	unsigned char reply_code;	/* reply code (see above)	*/
	unsigned char reserved3[3];	/* 0x000000			*/
} __packed;

#define TYPE86_RSP_CODE 0x86
#define TYPE87_RSP_CODE 0x87
#define TYPE86_FMT2	0x02

struct type86_fmt2_ext {
	unsigned char	  reserved[4];	/* 0x00000000			*/
	unsigned char	  apfs[4];	/* final status			*/
	unsigned int	  count1;	/* length of CPRB + parameters	*/
	unsigned int	  offset1;	/* offset to CPRB		*/
	unsigned int	  count2;	/* 0x00000000			*/
	unsigned int	  offset2;	/* db offset 0x00000000 for PKD	*/
	unsigned int	  count3;	/* 0x00000000			*/
	unsigned int	  offset3;	/* 0x00000000			*/
	unsigned int	  count4;	/* 0x00000000			*/
	unsigned int	  offset4;	/* 0x00000000			*/
} __packed;

int prep_cca_ap_msg(bool userspace, struct ica_xcRB *xcrb,
		    struct ap_message *ap_msg,
		    unsigned int *fc, unsigned short **dom);
int prep_ep11_ap_msg(bool userspace, struct ep11_urb *xcrb,
		     struct ap_message *ap_msg,
		     unsigned int *fc, unsigned int *dom);
int prep_rng_ap_msg(struct ap_message *ap_msg,
		    int *fc, unsigned int *dom);

#define LOW	10
#define MEDIUM	100
#define HIGH	500

int speed_idx_cca(int);
int speed_idx_ep11(int);

void zcrypt_msgtype6_init(void);
void zcrypt_msgtype6_exit(void);

#endif /* _ZCRYPT_MSGTYPE6_H_ */
