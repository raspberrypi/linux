// SPDX-License-Identifier: GPL-2.0-only
// based on arch/arm/mm/alignment.c

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/perf_event.h>
#include <linux/uaccess.h>

#include <asm/exception.h>
#include <asm/ptrace.h>
#include <asm/traps.h>

#include <asm/fpsimd.h>
#include <asm/neon.h>
#include <asm/simd.h>

/*
 * 32-bit misaligned trap handler (c) 1998 San Mehat (CCC) -July 1998
 *
 * Speed optimisations and better fault handling by Russell King.
 */
#define CODING_BITS(i)	(i & 0x0e000000)

#define LDST_P_BIT(i)	(i & (1 << 24))		/* Preindex		*/
#define LDST_U_BIT(i)	(i & (1 << 23))		/* Add offset		*/
#define LDST_W_BIT(i)	(i & (1 << 21))		/* Writeback		*/
#define LDST_L_BIT(i)	(i & (1 << 20))		/* Load			*/

#define LDST_P_EQ_U(i)	((((i) ^ ((i) >> 1)) & (1 << 23)) == 0)

#define LDSTHD_I_BIT(i)	(i & (1 << 22))		/* double/half-word immed */

#define RN_BITS(i)	((i >> 16) & 15)	/* Rn			*/
#define RD_BITS(i)	((i >> 12) & 15)	/* Rd			*/
#define RM_BITS(i)	(i & 15)		/* Rm			*/

#define REGMASK_BITS(i)	(i & 0xffff)

#define BAD_INSTR 	0xdeadc0de

/* Thumb-2 32 bit format per ARMv7 DDI0406A A6.3, either f800h,e800h,f800h */
#define IS_T32(hi16) \
	(((hi16) & 0xe000) == 0xe000 && ((hi16) & 0x1800))

union offset_union {
	unsigned long un;
	  signed long sn;
};

#define TYPE_ERROR	0
#define TYPE_FAULT	1
#define TYPE_LDST	2
#define TYPE_DONE	3

static void
do_alignment_finish_ldst(unsigned long addr, u32 instr, struct pt_regs *regs,
			 union offset_union offset)
{
	if (!LDST_U_BIT(instr))
		offset.un = -offset.un;

	if (!LDST_P_BIT(instr))
		addr += offset.un;

	if (!LDST_P_BIT(instr) || LDST_W_BIT(instr))
		regs->regs[RN_BITS(instr)] = addr;
}

static int
do_alignment_ldrdstrd(unsigned long addr, u32 instr, struct pt_regs *regs)
{
	unsigned int rd = RD_BITS(instr);
	unsigned int rd2;
	int load;

	if ((instr & 0xfe000000) == 0xe8000000) {
		/* ARMv7 Thumb-2 32-bit LDRD/STRD */
		rd2 = (instr >> 8) & 0xf;
		load = !!(LDST_L_BIT(instr));
	} else if (((rd & 1) == 1) || (rd == 14)) {
		return TYPE_ERROR;
	} else {
		load = ((instr & 0xf0) == 0xd0);
		rd2 = rd + 1;
	}

	if (load) {
		unsigned int val, val2;

		if (get_user(val, (u32 __user *)addr) ||
		    get_user(val2, (u32 __user *)(addr + 4)))
			return TYPE_FAULT;
		regs->regs[rd] = val;
		regs->regs[rd2] = val2;
	} else {
		if (put_user(regs->regs[rd], (u32 __user *)addr) ||
		    put_user(regs->regs[rd2], (u32 __user *)(addr + 4)))
			return TYPE_FAULT;
	}
	return TYPE_LDST;
}

/*
 * LDM/STM alignment handler.
 *
 * There are 4 variants of this instruction:
 *
 * B = rn pointer before instruction, A = rn pointer after instruction
 *              ------ increasing address ----->
 *	        |    | r0 | r1 | ... | rx |    |
 * PU = 01             B                    A
 * PU = 11        B                    A
 * PU = 00        A                    B
 * PU = 10             A                    B
 */
static int
do_alignment_ldmstm(unsigned long addr, u32 instr, struct pt_regs *regs)
{
	unsigned int rd, rn, nr_regs, regbits;
	unsigned long eaddr, newaddr;
	unsigned int val;

	/* count the number of registers in the mask to be transferred */
	nr_regs = hweight16(REGMASK_BITS(instr)) * 4;

	rn = RN_BITS(instr);
	newaddr = eaddr = regs->regs[rn];

	if (!LDST_U_BIT(instr))
		nr_regs = -nr_regs;
	newaddr += nr_regs;
	if (!LDST_U_BIT(instr))
		eaddr = newaddr;

	if (LDST_P_EQ_U(instr))	/* U = P */
		eaddr += 4;

	for (regbits = REGMASK_BITS(instr), rd = 0; regbits;
	     regbits >>= 1, rd += 1)
		if (regbits & 1) {
			if (LDST_L_BIT(instr)) {
				if (get_user(val, (u32 __user *)eaddr))
					return TYPE_FAULT;
				if (rd < 15)
					regs->regs[rd] = val;
				else
					regs->pc = val;
			} else {
				/*
				 * The PC register has a bias of +8 in ARM mode
				 * and +4 in Thumb mode. This means that a read
				 * of the value of PC should account for this.
				 * Since Thumb does not permit STM instructions
				 * to refer to PC, just add 8 here.
				 */
				val = (rd < 15) ? regs->regs[rd] : regs->pc + 8;
				if (put_user(val, (u32 __user *)eaddr))
					return TYPE_FAULT;
			}
			eaddr += 4;
		}

	if (LDST_W_BIT(instr))
		regs->regs[rn] = newaddr;

	return TYPE_DONE;
}

/*
 * Convert Thumb multi-word load/store instruction forms to equivalent ARM
 * instructions so we can reuse ARM userland alignment fault fixups for Thumb.
 *
 * This implementation was initially based on the algorithm found in
 * gdb/sim/arm/thumbemu.c. It is basically just a code reduction of same
 * to convert only Thumb ld/st instruction forms to equivalent ARM forms.
 *
 * NOTES:
 * 1. Comments below refer to ARM ARM DDI0100E Thumb Instruction sections.
 * 2. If for some reason we're passed an non-ld/st Thumb instruction to
 *    decode, we return 0xdeadc0de. This should never happen under normal
 *    circumstances but if it does, we've got other problems to deal with
 *    elsewhere and we obviously can't fix those problems here.
 */

static unsigned long thumb2arm(u16 tinstr)
{
	u32 L = (tinstr & (1<<11)) >> 11;

	switch ((tinstr & 0xf800) >> 11) {
	/* 6.6.1 Format 1: */
	case 0xc000 >> 11:				/* 7.1.51 STMIA */
	case 0xc800 >> 11:				/* 7.1.25 LDMIA */
		{
			u32 Rn = (tinstr & (7<<8)) >> 8;
			u32 W = ((L<<Rn) & (tinstr&255)) ? 0 : 1<<21;

			return 0xe8800000 | W | (L<<20) | (Rn<<16) |
				(tinstr&255);
		}

	/* 6.6.1 Format 2: */
	case 0xb000 >> 11:				/* 7.1.48 PUSH */
	case 0xb800 >> 11:				/* 7.1.47 POP */
		if ((tinstr & (3 << 9)) == 0x0400) {
			static const u32 subset[4] = {
				0xe92d0000,	/* STMDB sp!,{registers} */
				0xe92d4000,	/* STMDB sp!,{registers,lr} */
				0xe8bd0000,	/* LDMIA sp!,{registers} */
				0xe8bd8000	/* LDMIA sp!,{registers,pc} */
			};
			return subset[(L<<1) | ((tinstr & (1<<8)) >> 8)] |
			    (tinstr & 255);		/* register_list */
		}
		fallthrough;	/* for illegal instruction case */

	default:
		return BAD_INSTR;
	}
}

/*
 * Convert Thumb-2 32 bit LDM, STM, LDRD, STRD to equivalent instruction
 * handlable by ARM alignment handler, also find the corresponding handler,
 * so that we can reuse ARM userland alignment fault fixups for Thumb.
 *
 * @pinstr: original Thumb-2 instruction; returns new handlable instruction
 * @regs: register context.
 * @poffset: return offset from faulted addr for later writeback
 *
 * NOTES:
 * 1. Comments below refer to ARMv7 DDI0406A Thumb Instruction sections.
 * 2. Register name Rt from ARMv7 is same as Rd from ARMv6 (Rd is Rt)
 */
static void *
do_alignment_t32_to_handler(u32 *pinstr, struct pt_regs *regs,
			    union offset_union *poffset)
{
	u32 instr = *pinstr;
	u16 tinst1 = (instr >> 16) & 0xffff;
	u16 tinst2 = instr & 0xffff;

	switch (tinst1 & 0xffe0) {
	/* A6.3.5 Load/Store multiple */
	case 0xe880:		/* STM/STMIA/STMEA,LDM/LDMIA, PUSH/POP T2 */
	case 0xe8a0:		/* ...above writeback version */
	case 0xe900:		/* STMDB/STMFD, LDMDB/LDMEA */
	case 0xe920:		/* ...above writeback version */
		/* no need offset decision since handler calculates it */
		return do_alignment_ldmstm;

	case 0xf840:		/* POP/PUSH T3 (single register) */
		if (RN_BITS(instr) == 13 && (tinst2 & 0x09ff) == 0x0904) {
			u32 L = !!(LDST_L_BIT(instr));
			const u32 subset[2] = {
				0xe92d0000,	/* STMDB sp!,{registers} */
				0xe8bd0000,	/* LDMIA sp!,{registers} */
			};
			*pinstr = subset[L] | (1<<RD_BITS(instr));
			return do_alignment_ldmstm;
		}
		/* Else fall through for illegal instruction case */
		break;

	/* A6.3.6 Load/store double, STRD/LDRD(immed, lit, reg) */
	case 0xe860:
	case 0xe960:
	case 0xe8e0:
	case 0xe9e0:
		poffset->un = (tinst2 & 0xff) << 2;
		fallthrough;

	case 0xe940:
	case 0xe9c0:
		return do_alignment_ldrdstrd;

	/*
	 * No need to handle load/store instructions up to word size
	 * since ARMv6 and later CPUs can perform unaligned accesses.
	 */
	default:
		break;
	}
	return NULL;
}

static int alignment_get_arm(struct pt_regs *regs, __le32 __user *ip, u32 *inst)
{
	__le32 instr = 0;
	int fault;

	fault = get_user(instr, ip);
	if (fault)
		return fault;

	*inst = __le32_to_cpu(instr);
	return 0;
}

static int alignment_get_thumb(struct pt_regs *regs, __le16 __user *ip, u16 *inst)
{
	__le16 instr = 0;
	int fault;

	fault = get_user(instr, ip);
	if (fault)
		return fault;

	*inst = __le16_to_cpu(instr);
	return 0;
}

int do_compat_alignment_fixup(unsigned long addr, struct pt_regs *regs)
{
	union offset_union offset;
	unsigned long instrptr;
	int (*handler)(unsigned long addr, u32 instr, struct pt_regs *regs);
	unsigned int type;
	u32 instr = 0;
	int isize = 4;
	int thumb2_32b = 0;

	instrptr = instruction_pointer(regs);
	printk("Alignment fixup\n");
	if (compat_thumb_mode(regs)) {
		__le16 __user *ptr = (__le16 __user *)(instrptr & ~1);
		u16 tinstr, tinst2;

		if (alignment_get_thumb(regs, ptr, &tinstr))
			return 1;

		if (IS_T32(tinstr)) { /* Thumb-2 32-bit */
			if (alignment_get_thumb(regs, ptr + 1, &tinst2))
				return 1;
			instr = ((u32)tinstr << 16) | tinst2;
			thumb2_32b = 1;
		} else {
			isize = 2;
			instr = thumb2arm(tinstr);
		}
	} else {
		if (alignment_get_arm(regs, (__le32 __user *)instrptr, &instr))
			return 1;
	}

	switch (CODING_BITS(instr)) {
	case 0x00000000:	/* 3.13.4 load/store instruction extensions */
		if (LDSTHD_I_BIT(instr))
			offset.un = (instr & 0xf00) >> 4 | (instr & 15);
		else
			offset.un = regs->regs[RM_BITS(instr)];

		if ((instr & 0x001000f0) == 0x000000d0 || /* LDRD */
		    (instr & 0x001000f0) == 0x000000f0)   /* STRD */
			handler = do_alignment_ldrdstrd;
		else
			return 1;
		break;

	case 0x08000000:	/* ldm or stm, or thumb-2 32bit instruction */
		if (thumb2_32b) {
			offset.un = 0;
			handler = do_alignment_t32_to_handler(&instr, regs, &offset);
		} else {
			offset.un = 0;
			handler = do_alignment_ldmstm;
		}
		break;

	default:
		return 1;
	}

	type = handler(addr, instr, regs);

	if (type == TYPE_ERROR || type == TYPE_FAULT)
		return 1;

	if (type == TYPE_LDST)
		do_alignment_finish_ldst(addr, instr, regs, offset);

	perf_sw_event(PERF_COUNT_SW_ALIGNMENT_FAULTS, 1, regs, regs->pc);
	arm64_skip_faulting_instruction(regs, isize);

	return 0;
}

// arm64#

/*
 *Happens with The Long Dark (also with steam)
 *
 *[ 6012.660803] Faulting instruction: 0x3d800020
[ 6012.660813] Load/Store: op0 0x3 op1 0x1 op2 0x3 op3 0x0 op4 0x0
 *
 *[  555.449651] Load/Store: op0 0x3 op1 0x1 op2 0x1 op3 0x1 op4 0x0
[  555.449654] Faulting instruction: 0x3c810021
 *
 *
 *[  555.449663] Load/Store: op0 0x3 op1 0x1 op2 0x1 op3 0x2 op4 0x0
[  555.449666] Faulting instruction: 0x3c820020
 *
 *[  555.449674] Load/Store: op0 0x3 op1 0x1 op2 0x1 op3 0x3 op4 0x0
[  555.449677] Faulting instruction: 0x3c830021

stur	q1, [x1, #16]
potentially also ldur	q0, [x1, #32] and ldur	q1, [x1, #48]
 *
 *
 *
 */

struct fixupDescription{
	void* addr;

	// datax_simd has to be located directly after datax in memory
	/*u64 data1;
	u64 data1_simd;
	u64 data2;
	u64 data2_simd;*/

	int reg1;
	int reg2;

	int Rs;		// used for atomics (which don't get handled atomically)

	int simd;	// wether or not this is a vector instruction
	int load;	// 1 is it's a load, 0 if it's a store
	int pair;	// 1 if it's a l/s pair instruction
	int width;	// width of the access in bits
	int extendSign;
	int extend_width;
};

static int alignment_get_arm64(struct pt_regs *regs, __le64 __user *ip, u32 *inst)
{
	__le32 instr = 0;
	int fault;

	fault = get_user(instr, ip);
	if (fault)
		return fault;

	*inst = __le32_to_cpu(instr);
	return 0;
}

/*int ldpstp_offset_fixup(u32 instr, struct pt_regs *regs){
	uint8_t load = (instr >> 22) & 1;
	uint8_t simd = (instr >> 26) & 1;
	uint16_t imm7 = (instr >> 15) & 0x7f;
	uint8_t Rt2 = (instr >> 10) & 0x1f;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;

	int16_t imm = 0xffff & imm7;
	printk("Variant: 0x%x Load: %x SIMD: %x IMM: 0x%x Rt: 0x%x Rt2: 0x%x Rn: 0x%x\n", ((instr >> 30) & 3),load, simd, imm, Rt, Rt2, Rn);
	if(((instr >> 30) & 3) == 2){
		// 64bit
		if(!load){
			if(!simd){
				// 64bit store
				u64 val1, val2;
				val1 = regs->regs[Rt];
				val2 = regs->regs[Rt2];
				u64 addr = regs->regs[Rn] + imm;
				printk("STP 64bit storing 0x%llx 0x%llx at 0x%llx\n", val1, val2, addr);
				// for the first reg. Byte by byte to avoid any alignment issues
				for(int i = 0; i < 8; i++){
					uint8_t v = (val1 >> (i*8)) & 0xff;
					put_user(v, (uint8_t __user *)addr);
					addr++;
				}
				// second reg
				for(int i = 0; i < 8; i++){
					uint8_t v = (val2 >> (i*8)) & 0xff;
					put_user(v, (uint8_t __user *)addr);
					addr++;
				}
				arm64_skip_faulting_instruction(regs, 4);
			}
		}
	}
	return 0;
}*/

// saves the contents of the simd register reg to dst
void read_simd_reg(int reg, u64 dst[2]){
	struct user_fpsimd_state st = {0};
	//fpsimd_save_state(&st);

	if(!may_use_simd()){
		printk("may_use_simd returned false!\n");
	}
	kernel_neon_begin();
	if(current->thread.sve_state){
		printk("SVE state is not NULL!\n");
	}

	dst[0] = *((u64*)(&current->thread.uw.fpsimd_state.vregs[reg]));
	dst[1] = *(((u64*)(&current->thread.uw.fpsimd_state.vregs[reg])) + 1);

	kernel_neon_end();
}

int do_ls_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	int r;
	u64 data1[2];
	u64 data2[2];

	// the reg indices have to always be valid, even if the reg isn't being used
	if(desc->simd){
		// At least currently, there aren't any simd instructions supported that use more than one data register
		//__uint128_t tmp;
		read_simd_reg(desc->reg1, data1);
		//data1[0] = tmp;
		//data1[1] = *(((u64*)&tmp) + 1);
		printk("SIMD: storing 0x%llx %llx (%d bits) at 0x%px", data1[1], data1[0], desc->width, desc->addr);
	} else {
		data1[0] = regs->regs[desc->reg1];
		data2[0] = regs->regs[desc->reg2];
	}

	/*if(desc->width > 64){
		printk("Currently cannot process ls_fixup with a size of %d bits\n", desc->width);
		return 1;
	}*/
	if(!desc->load){
		uint8_t* addr = desc->addr;
		int bcount = desc->width / 8;	// since the field stores the width in bits. Honestly, there's no particular reason for that

		//printk("Storing %d bytes (pair: %d) to 0x%llx",bcount, desc->pair, desc->addr);
		int addrIt = 0;
		for(int i = 0; i < bcount; i++){
			if((r=put_user( (*(((uint8_t*)(data1)) + addrIt) & 0xff), (uint8_t __user *)addr))){
				printk("Failed to write data at 0x%px (base was 0x%px)\n", addr, desc->addr);
				return r;
			}
			//desc->data1 >>= 8;
			addrIt++;
			addr++;
		}

		addrIt = 0;
		if(desc->pair){
			for(int i = 0; i < bcount; i++){
				if((r=put_user((*(((uint8_t*)(data2)) + addrIt) & 0xff) & 0xff, (uint8_t __user *)addr))){
					printk("Failed to write data at 0x%px (base was 0x%px)\n", addr, desc->addr);
					return r;
				}
				//desc->data2 >>= 8;
				addrIt++;
				addr++;
			}
		}
		arm64_skip_faulting_instruction(regs, 4);
	} else {
		printk("Loading is currently not implemented (addr 0x%px)\n", desc->addr);
		return -1;
	}
	return 0;
}

int ls_cas_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t size = (instr >> 30) & 3;
	uint8_t load = (instr >> 22) & 1;	// acquire semantics, has no effect here, since it's not atomic anymore
	uint8_t Rs = (instr >> 16) & 0x1f;
	uint8_t Rt2 = (instr >> 10) & 0x1f;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;

	uint8_t o0 = (instr >> 15) & 1;	// L, release semantics, has no effect here, since it's not atomic anymore

	if(Rt2 != 0x1f){
		return -1;
	}

	switch(size){
	case 0:
		desc->width = 8;
		break;
	case 1:
			desc->width = 16;
			break;
	case 2:
			desc->width = 32;
			break;
	case 3:
			desc->width = 64;
			break;
	}

	desc->addr = (void*)regs->regs[Rn];
	u64 data1 = regs->regs[Rt];

	// nearly everything from here on could be moved into another function if needed
	u64 cmpmask = (1 << desc->width) - 1;
	u64 cmpval = regs->regs[Rs] & cmpmask;

	u64 readval = 0;
	int bcount = desc->width / 8;
	u64 addr = desc->addr;
	int r;
	uint8_t  tmp;

	printk("Atomic CAS not being done atomically at 0x%px, size %d\n",desc->addr, desc->width);

	for(int i = 0; i < bcount; i++){
		if((r=get_user(tmp, (uint8_t __user *)addr)))
			return r;
		readval |= tmp;
		readval <<= 8;	// maybe this could be read directly into regs->regs[Rs]
		addr++;
	}

	if((readval & cmpmask) == cmpval){
		// swap
		addr = (u64)desc->addr;

		for(int i = 0; i < bcount; i++){
			if((r=put_user(data1 & 0xff, (uint8_t __user *)addr)))
				return r;
			data1 >>= 8;
			addr++;
		}

		regs->regs[Rs] = readval;
	}

	arm64_skip_faulting_instruction(regs, 4);

	return 0;
}

int ls_pair_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t op2;
	uint8_t opc;
	op2 = (instr >> 23) & 3;
	opc = (instr >> 30) & 3;

	uint8_t load = (instr >> 22) & 1;
	uint8_t simd = (instr >> 26) & 1;
	uint16_t imm7 = (instr >> 15) & 0x7f;
	uint8_t Rt2 = (instr >> 10) & 0x1f;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;

	int16_t imm = 0xffff & imm7;

	desc->load = load;
	desc->simd = simd;

	// opc controls the width
	switch(opc){
	case 0:
		desc->width = 32;
		imm <<= 2;
		break;
	case 2:
		desc->width = 64;
		imm <<= 3;
		break;
	default:
		return -1;
	}

	// op2 controls the indexing
	switch(op2){
	case 2:
		// offset
		desc->addr = (void*)(regs->regs[Rn] + imm);
		break;
	default:
			return -1;
	}
	//desc->data1 = regs->regs[Rt];
	//desc->data2 = regs->regs[Rt2];
	desc->reg1 = Rt;
	desc->reg2 = Rt2;

	return do_ls_fixup(instr, regs, desc);

}

int ls_reg_unsigned_imm(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t size = (instr >> 30) & 3;
	uint8_t simd = (instr >> 26) & 1;
	uint8_t opc = (instr >> 22) & 3;
	uint16_t imm12 = (instr >> 10) & 0xfff;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;

	uint8_t load = opc & 1;
	uint8_t extend_sign = ((opc & 2) >> 1 ) & !simd;
	printk("size: %d simd: %d opc: %d imm12: 0x%x Rn: %d Rt: %d\n", size, simd, opc, imm12, Rn, Rt);
	// when in simd mode, opc&2 is a third size bit. Otherwise, it's there for sign extension
	int width_shift = (size | (((opc & 2) & (simd << 1)) << 1));
	desc->width = 8 << width_shift;

	if((size & 1) && simd && (opc & 2)){
		return 1;
	}

	desc->reg1 = Rt;
	desc->simd = simd;
	desc->extendSign = extend_sign;
	u64 addr = regs->regs[Rn];
	desc->addr = addr + (imm12 << width_shift);
	printk("unsigned imm\n");

	return do_ls_fixup(instr, regs, desc);
}


u64 extend_reg(u64 reg, int type, int shift){

	uint8_t is_signed = (type & 4) >> 2;
	uint8_t input_width = type & 1;

	u64 tmp;
	if(!is_signed){
		tmp = reg;
	} else {
		if(input_width == 0){
			// 32bit, needs to be extended to 64
			// I hope the compiler just does this kind of automatically with these types
			int32_t stmpw = reg;
			int64_t stmpdw = stmpw;
			tmp = (u64)stmpdw;
		}
	}

	return tmp << shift;
}

int lsr_offset_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t size = (instr >> 30) & 3;
	uint8_t simd = (instr >> 26) & 1;
	uint8_t opc = (instr >> 22) & 3;
	uint8_t option = (instr >> 13) & 5;
	uint8_t Rm = (instr >> 16) & 0x1f;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;
	uint8_t S = (instr >> 12) & 1;
	int width_shift = (size | (((opc & 2) & (simd << 1)) << 1));
	// size==0 seems to be a bit special
	// opc&2 is sign, opc&1 is load	(for most instructions anyways)

	uint8_t load = opc & 1;
	uint8_t extend_sign = ((opc & 2) >> 1 ) & !simd;
	desc->pair = 0;

	desc->simd = simd;
	desc->width = 8 << width_shift;

	// the simd instructions make this a bit weird
	if(extend_sign){
		if(load){
			desc->extend_width = 32;
		} else {
			desc->extend_width = 64;
		}
		desc->load = 1;
	} else {
		desc->load = load;
	}

	desc->extendSign = extend_sign;	// needed for load, which isn't implemented yet

	u64 offset = 0;
	u64 addr = 0;
	addr = regs->regs[Rn];
	if(simd){
		int shift = 0;
		if(S) shift = width_shift;
		offset = extend_reg(regs->regs[Rm], option, shift);
	} else {
		int shift = 0;
		if(S) shift = 2 << ((size & 1) & ((size >> 1) & 1));

		offset = extend_reg(regs->regs[Rm], option, shift);
	}

	addr += offset;

	//desc->data1 = regs->regs[Rt];
	desc->reg1 = Rt;
	desc->addr = (void*)addr;

	return do_ls_fixup(instr, regs, desc);
	return 0;
}

int lsr_unscaled_immediate_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t size = (instr >> 30) & 3;
	uint8_t simd = (instr >> 26) & 1;
	uint8_t opc = (instr >> 22) & 3;
	uint16_t imm9 = (instr >> 12) & 0x1ff;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;

	int16_t fullImm = 0;
	// sign extend it
	if(imm9 & 0x100){
		fullImm = 0xfe00 | imm9;
	} else {
		fullImm = imm9;
	}
	u64 addr = regs->regs[Rn];
	desc->addr = addr + fullImm;
	desc->pair = 0;

	int load = opc & 1;
	if(load){
		return 1;
	}
	desc->reg1 = Rt;
	if(simd){
		desc->simd = 1;
		desc->width = 8 << (size | ((opc & 2) << 1));
		// assuming store
		/*__uint128_t tmp;
		read_simd_reg(Rt, &tmp);
		desc->data1 = tmp;
		desc->data1_simd = *(((u64*)&tmp) + 1);*/
		return do_ls_fixup(instr, regs, desc);
	}
	printk("SIMD: %d\n", simd);
	return 1;
}

int ls_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t op0;
	uint8_t op1;
	uint8_t op2;
	uint8_t op3;
	uint8_t op4;

	int r = 1;

	op0 = (instr >> 28) & 0xf;
	op1 = (instr >> 26) & 1;
	op2 = (instr >> 23) & 3;
	op3 = (instr >> 16) & 0x3f;
	op4 = (instr >> 10) & 3;

	if((op0 & 3) == 2){
		desc->pair = 1;
		r = ls_pair_fixup(instr, regs, desc);
	}
	if((op0 & 3) == 0 && op1 == 0 && op2 == 1 && (op3 & 0x20) == 0x20){
		// compare and swap
		r = ls_cas_fixup(instr, regs, desc);
	}
	if((op0 & 3) == 3 && (op2 & 3) == 3){
		//load/store unsigned immediate
		desc->pair = 0;

	}
	if((op0 & 3) == 3 && ((op2 & 2) == 2)){
		// register unsigned immediate
		r = ls_reg_unsigned_imm(instr, regs, desc);
	}
	if((op0 & 3) == 3 && (op2 & 2) == 0 && (op3 & 0x20) == 0x20 && op4 == 2){
		// register offset load/store
		r = lsr_offset_fixup(instr, regs, desc);
	}
	if((op0 & 3) == 3 && (op2 & 2) == 0 && (op3 & 0x20) == 0x0 && op4 == 0){
		// register load/store unscaled immediate
		r = lsr_unscaled_immediate_fixup(instr, regs, desc);
	}
	if(r){
		printk("Load/Store: op0 0x%x op1 0x%x op2 0x%x op3 0x%x op4 0x%x\n", op0, op1, op2, op3, op4);
	}
	return r;
}

int do_alignment_fixup(unsigned long addr, struct pt_regs *regs){
	unsigned long long instrptr;
	u32 instr = 0;

	instrptr = instruction_pointer(regs);
	//printk("Alignment fixup\n");

	if (alignment_get_arm64(regs, (__le64 __user *)instrptr, &instr)){
		printk("Failed to get aarch64 instruction\n");
		return 1;
	}

	/**
	 * List of seen faults: 020c00a9 (0xa9000c02) stp x2, x3, [x0]
	 *
	 */

	uint8_t op0;
	int r;
	struct fixupDescription desc = {0};

	op0 = ((instr & 0x1E000000) >> 25);
	if((op0 & 5) == 0x4){
		//printk("Load/Store\n");
		r = ls_fixup(instr, regs, &desc);
		if(r){
			printk("Faulting instruction: 0x%lx\n", instr);
		}
		return r;
	} else {
		printk("Not handling instruction with op0 0x%x ",op0);
	}
	return -1;
}
