
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include <asm/fpsimd.h>
#include <asm/neon.h>
#include <asm/simd.h>
#include <asm/ptrace.h>
#include <asm/traps.h>

#include <generated/asm/sysreg-defs.h>

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

	// profiling
	u64 starttime;
	u64 decodedtime;
	u64 endtime;
};

__attribute__((always_inline)) inline static int alignment_get_arm64(struct pt_regs *regs, __le64 __user *ip, u32 *inst)
{
	__le32 instr = 0;
	int fault;

	fault = get_user(instr, ip);
	if (fault)
		return fault;

	*inst = __le32_to_cpu(instr);
	return 0;
}

__attribute__((always_inline)) inline int64_t extend_sign(int64_t in, int bits){
	bits--;
	if(in & (1 << bits)){
		// extend sign
		return (0xffffffffffffffff << bits) | in;
	}
	return in;
}

// saves the contents of the simd register reg to dst
__attribute__((always_inline)) inline void read_simd_reg(int reg, u64 dst[2]){
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


__attribute__((always_inline)) inline void write_simd_reg(int reg, u64 src[2]){

	if(!may_use_simd()){
		printk("may_use_simd returned false!\n");
	}
	kernel_neon_begin();
	if(current->thread.sve_state){
		printk("SVE state is not NULL!\n");
	}

	*((u64*)(&current->thread.uw.fpsimd_state.vregs[reg])) = src[0];
	*(((u64*)(&current->thread.uw.fpsimd_state.vregs[reg])) + 1) = src[1];

	kernel_neon_end();
}

// these try to use larger access widths than single bytes. Slower for small loads/stores, but it might speed larger ones up

__attribute__((always_inline)) inline int put_data2(int size, uint8_t* data, void* addr){
	int r = 0;

	while(size){
		if(size >= 4 && (((u64)addr % 4) == 0)){
			if((r=put_user( (*(((uint32_t*)(data)))), (uint32_t __user *)addr))){
				printk("Failed to write data at 0x%px (%d)\n", addr,r);
				return r;
			}
			addr += 4;
			data += 4;
			size -= 4;
			continue;
		}
		if(size >= 2 && (((u64)addr % 2) == 0)){
			if((r=put_user( (*(((uint16_t*)(data)))), (uint16_t __user *)addr))){
				printk("Failed to write data at 0x%px (%d)\n", addr,r);
				return r;
			}
			addr += 2;
			data += 2;
			size -= 2;
			continue;
		}
		// I guess the if is redundant here
		if(size >= 1){
			if((r=put_user( (*(((uint8_t*)(data)))), (uint8_t __user *)addr))){
				printk("Failed to write data at 0x%px (%d)\n", addr,r);
				return r;
			}
			addr += 1;
			data += 1;
			size -= 1;
			continue;
		}

	}

	return r;
}

__attribute__((always_inline)) inline int get_data2(int size, uint8_t* data, void* addr){
	int r = 0;
	uint32_t val32;
	uint16_t val16;
	uint8_t val8;
	while(size){
		if(size >= 4 && (((u64)addr % 4) == 0)){
			if((r=get_user( val32, (uint32_t __user *)addr))){
				printk("Failed to read data at 0x%px\n", addr);
				return r;
			}
			*((uint32_t*)data) = val32;
			addr += 4;
			data += 4;
			size -= 4;
			continue;
		}
		if(size >= 2 && (((u64)addr % 2) == 0)){
			if((r=get_user( val16, (uint16_t __user *)addr))){
				printk("Failed to read data at 0x%px\n", addr);
				return r;
			}
			*((uint16_t*)data) = val16;
			addr += 2;
			data += 2;
			size -= 2;
			continue;
		}
		// I guess the if is redundant here
		if(size >= 1){
			if((r=get_user( val8, (uint8_t __user *)addr))){
				printk("Failed to read data at 0x%px\n", addr);
				return r;
			}
			*((uint8_t*)data) = val8;
			addr += 1;
			data += 1;
			size -= 1;
			continue;
		}

	}

	return r;
}


// these should avoid some branching, but still use single byte accesses
__attribute__((always_inline)) inline int put_data(int size, uint8_t* data, void* addr){
	int r = 0;
	int addrIt = 0;

	// with the fixed size loops, the compiler should be able to unroll them
	// this should mean a lot less branching
	switch(size){
	case 16:
		for(int i = 0; i < 8; i++){
			if((r=put_user( (*(((uint8_t*)(data)) + addrIt) & 0xff), (uint8_t __user *)addr))){
				printk("Failed to write data at 0x%px\n", addr);
				return r;
			}
			addrIt++;
			addr++;
		}
		//__attribute__((fallthrough));
	case 8:
		for(int i = 0; i < 4; i++){
			if((r=put_user( (*(data + addrIt) & 0xff), (uint8_t __user *)addr))){
				printk("Failed to write data at 0x%px\n", addr);
				return r;
			}
			addrIt++;
			addr++;
		}
		//__attribute__((fallthrough));
	case 4:
		for(int i = 0; i < 2; i++){
			if((r=put_user( (*(data + addrIt) & 0xff), (uint8_t __user *)addr))){
				printk("Failed to write data at 0x%px\n", addr);
				return r;
			}
			addrIt++;
			addr++;
		}
		//__attribute__ ((fallthrough));
	case 2:
		if((r=put_user( (*(data + addrIt) & 0xff), (uint8_t __user *)addr))){
			printk("Failed to write data at 0x%px\n", addr);
			return r;
		}
		addrIt++;
		addr++;
		//__attribute__ ((fallthrough));
	case 1:
		if((r=put_user( (*(data + addrIt) & 0xff), (uint8_t __user *)addr))){
			printk("Failed to write data at 0x%px\n", addr);
			return r;
		}
		addrIt++;
		addr++;
		break;
	default:
		printk("unsupported size %d\n", size);
	}

	return r;
}

__attribute__((always_inline)) inline int get_data(int size, uint8_t* data, void* addr){
	int r = 0;
	int addrIt = 0;

	// with the fixed size loops, the compiler should be able to unroll them
	// this should mean a lot less branching
	uint8_t val;
	switch(size){
	case 16:
		for(int i = 0; i < 8; i++){
			if((r=get_user( val, (uint8_t __user *)addr))){
				printk("Failed to read data at 0x%px\n", addr);
				return r;
			}
			*(data + addrIt) = val;
			addrIt++;
			addr++;
		}
		// fall through
	case 8:
		for(int i = 0; i < 4; i++){
			if((r=get_user( val, (uint8_t __user *)addr))){
				printk("Failed to read data at 0x%px\n", addr);
				return r;
			}
			*(data + addrIt) = val;
			addrIt++;
			addr++;
		}
		// fall through
	case 4:
		for(int i = 0; i < 2; i++){
			if((r=get_user( val, (uint8_t __user *)addr))){
				printk("Failed to read data at 0x%px\n", addr);
				return r;
			}
			*(data + addrIt) = val;
			addrIt++;
			addr++;
		}
		// fall through
	case 2:
		if((r=get_user( val, (uint8_t __user *)addr))){
			printk("Failed to read data at 0x%px\n", addr);
			return r;
		}
		*(data + addrIt) = val;
		addrIt++;
		addr++;
		// fall through
	case 1:
		if((r=get_user( val, (uint8_t __user *)addr))){
			printk("Failed to read data at 0x%px\n", addr);
			return r;
		}
		*(data + addrIt) = val;
		addrIt++;
		addr++;
		break;
	default:
		printk("unsupported size %d\n", size);
	}

	return r;
}

int memset_io_user(uint64_t size, uint8_t c, void* addr){
	int r = 0;
	uint64_t pattern = c;
	pattern |= pattern << 8;
	pattern |= pattern << 16;
	pattern |= pattern << 32;
	uint64_t cnt = 0;
	while(cnt < size){
		if((uint64_t)(addr + cnt) % 8){
			if((r = put_user(c, (uint8_t __user*) addr))){
				printk("Failed to write data at 0x%px (%d)(base was 0x%px)\n", addr + cnt, r, addr);
				return r;
			}
			cnt++;
		} else if(size - cnt >= 8){
			if((r = put_user(pattern, (uint64_t __user*) addr))){
				printk("Failed to write data at 0x%px (%d)(base was 0x%px)\n", addr + cnt, r, addr);
				return r;
			}
			cnt += 8;
		} else{
			if((r = put_user(c, (uint8_t __user*) addr))){
				printk("Failed to write data at 0x%px (%d)(base was 0x%px)\n", addr + cnt, r, addr);
				return r;
			}
			cnt++;
		}

	}
	return r;
}

int do_ls_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	int r;
	u64 data1[2] = {0,0};
	u64 data2[2] = {0,0};
	//desc->decodedtime = ktime_get_ns();
	// the reg indices have to always be valid, even if the reg isn't being used
	if(!desc->load){
		if(desc->simd){
			// At least currently, there aren't any simd instructions supported that use more than one data register
			//__uint128_t tmp;

			// probably better for performance to read both registers with one function to kernel_neon_* doesn't have to be called more than once
			read_simd_reg(desc->reg1, data1);
			read_simd_reg(desc->reg2, data2);
			//data1[0] = tmp;
			//data1[1] = *(((u64*)&tmp) + 1);
			///printk("SIMD: storing 0x%llx %llx (%d bits) at 0x%px", data1[1], data1[0], desc->width, desc->addr);
			/*if(desc->width < 128){
				return -1;
			}*/
		} else {
			data1[0] = regs->regs[desc->reg1];
			data2[0] = regs->regs[desc->reg2];
		}
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
				printk("Failed to write data at 0x%px (%d)(base was 0x%px)\n", addr, r, desc->addr);
				return r;
			}
			//desc->data1 >>= 8;
			addrIt++;
			addr++;
		}
		//put_data2(bcount, (uint8_t*)data1, addr);
		//addr += bcount;
		addrIt = 0;
		if(desc->pair){
			for(int i = 0; i < bcount; i++){
				if((r=put_user((*(((uint8_t*)(data2)) + addrIt) & 0xff) & 0xff, (uint8_t __user *)addr))){
					printk("Failed to write data at 0x%px (%d)(base was 0x%px)\n", addr, r, desc->addr);
					return r;
				}
				//desc->data2 >>= 8;
				addrIt++;
				addr++;
			}
			//put_data2(bcount, (uint8_t*)data2, addr);
			addr += bcount;
		}
		arm64_skip_faulting_instruction(regs, 4);
	} else {
		//printk("Loading is currently not implemented (addr 0x%px)\n", desc->addr);

		uint8_t* addr = desc->addr;
		int bcount = desc->width / 8;	// since the field stores the width in bits. Honestly, there's no particular reason for that

		//printk("Storing %d bytes (pair: %d) to 0x%llx",bcount, desc->pair, desc->addr);
		int addrIt = 0;
		/*for(int i = 0; i < bcount; i++){
			uint8_t val;
			if((r=get_user( val, (uint8_t __user *)addr))){
				printk("Failed to write data at 0x%px (base was 0x%px)\n", addr, desc->addr);
				return r;
			}
			*(((uint8_t*)data1) + addrIt) = val;
			//desc->data1 >>= 8;
			addrIt++;
			addr++;
		}*/
		get_data2(bcount, (uint8_t*)data1, addr);
		addr += bcount;

		if(desc->simd){
			write_simd_reg(desc->reg1, data1);
		} else {
			regs->regs[desc->reg1] = data1[0];
		}

		addrIt = 0;
		if(desc->pair){
			/*for(int i = 0; i < bcount; i++){
				uint8_t val;
				if((r=get_user(val, (uint8_t __user *)addr))){
					printk("Failed to write data at 0x%px (base was 0x%px)\n", addr, desc->addr);
					return r;
				}
				*(((uint8_t*)data2) + addrIt) = val;
				//desc->data2 >>= 8;
				addrIt++;
				addr++;
			}*/

			get_data2(bcount, (uint8_t*)data2, addr);
			addr += bcount;
			if(desc->simd){
				write_simd_reg(desc->reg2, data1);
			} else {
				regs->regs[desc->reg2] = data1[0];
			}
		}
		arm64_skip_faulting_instruction(regs, 4);


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

__attribute__((always_inline)) inline int ls_pair_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
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

	int64_t imm = extend_sign(imm7, 7);
	//int immshift = 0;
	desc->load = load;
	desc->simd = simd;

	// opc controls the width
	if(simd){
		desc->width = 32 << opc;
		//immshift = 4 << opc;
		imm <<= 2;
		imm <<= opc;
	} else {
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

__attribute__((always_inline)) inline int ls_reg_unsigned_imm(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t size = (instr >> 30) & 3;
	uint8_t simd = (instr >> 26) & 1;
	uint8_t opc = (instr >> 22) & 3;
	uint64_t imm12 = (instr >> 10) & 0xfff;
	uint8_t Rn = (instr >> 5) & 0x1f;
	uint8_t Rt = instr & 0x1f;

	uint8_t load = opc & 1;
	uint8_t extend_sign = 0;// = ((opc & 2) >> 1 ) & !simd;
	int width_shift = 0;

	if(simd){
		extend_sign = 0;
		width_shift = size | ((opc & 2) << 1);
	} else {
		extend_sign = ((opc & 2) >> 1 );
		width_shift = size;
	}

	///printk("size: %d simd: %d opc: %d imm12: 0x%x Rn: %d Rt: %d\n", size, simd, opc, imm12, Rn, Rt);
	// when in simd mode, opc&2 is a third size bit. Otherwise, it's there for sign extension
	//width_shift = (size | (((opc & 2) & (simd << 1)) << 1));
	desc->width = 8 << width_shift;

	if((size & 1) && simd && (opc & 2)){
		return 1;
	}
	desc->load = load;
	desc->reg1 = Rt;
	desc->simd = simd;
	desc->extendSign = extend_sign;
	u64 addr = regs->regs[Rn];
	desc->addr = addr + (imm12 << width_shift);
	///printk("unsigned imm\n");

	return do_ls_fixup(instr, regs, desc);
}


__attribute__((always_inline)) inline u64 extend_reg(u64 reg, int type, int shift){

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
		} else {
			printk("Other branch I forgor about previously!\n");
			tmp = reg;	// since the size stays the same, I don't think this makes a difference
		}
	}

	///printk("extend_reg: reg 0x%lx out (before shift) 0x%lx signed: %x\n", reg, tmp, is_signed);

	return tmp << shift;
}

__attribute__((always_inline)) inline int lsr_offset_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
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

__attribute__((always_inline)) inline int lsr_unscaled_immediate_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
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
	desc->load = load;
	/*if(load){
		return 1;
	}*/
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
	} else {
		desc->simd = 0;
		desc->width = 8 << size;
		return do_ls_fixup(instr, regs, desc);
	}
	///printk("SIMD: %d\n", simd);
	return 1;
}

__attribute__((always_inline)) inline int ls_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
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

__attribute__((always_inline)) inline int system_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t op1;
	uint8_t op2;
	uint8_t CRn;
	uint8_t CRm;
	uint8_t Rt;
	bool L;
	int r = 0;

	op1 = (instr >> 16) & 0x7;
	op2 = (instr >> 5) & 0x7;
	CRn = (instr >> 12) & 0xf;
	CRm = (instr >> 8) & 0xf;
	L = (instr >> 21) & 1;
	Rt = instr & 0x1f;

	if(!L){
		// SYS
		// proper decoding would be nicer here, but I don't expect to see too many system instructions
		if((op1 == 0x3) && (op2 == 1) && (CRn = 0x7) && (CRm == 4)){
			// dc zva
			uint64_t dczid_el0 = read_sysreg_s(SYS_DCZID_EL0);
			if(!((dczid_el0 >> DCZID_EL0_DZP_SHIFT) & 1)){
				uint16_t blksize = 4 << (dczid_el0 & 0xf);
				r = memset_io_user(blksize, 0, regs->user_regs.regs[Rt]);
				arm64_skip_faulting_instruction(regs, 4);
				return r;
			} else {
				printk("DC ZVA is not allowed!\n");
				return 1;
			}
		}
	}

	printk("Unhandled system instruction. op1=0x%x op2=0x%x CRn=0x%x CRm=0x%x\n", op1, op2, CRn, CRm);
	return 1;
}

__attribute__((always_inline)) inline int branch_except_system_fixup(u32 instr, struct pt_regs *regs, struct fixupDescription* desc){
	uint8_t op0;
	uint32_t op1;
	uint8_t op2;

	op0 = (instr >> 29) & 0x7;
	op1 = (instr >> 5) & 0x1fffff;
	op2 = instr & 0x1f;

	if((op0 == 0x6) && (op1 & 0x1ec000) == 0x84000){
		return system_fixup(instr, regs, desc);
	}
	printk("Unhandled Branch/Exception generating/System instruction. op0=0x%x op1=0x%x op2=0x%x\n", op0, op1, op2);
	return 1;
}

uint32_t*  seenCMDs;
size_t seenCMDCount = 0;
size_t seenCMDSize = 0;

void instrDBG(u32 instr){
	for(size_t i = 0; i < seenCMDCount; i++){
		if(seenCMDs[i] == instr){
			return;
		}
	}
	if(seenCMDSize == 0){
		seenCMDs = krealloc(seenCMDs, 1, GFP_KERNEL);
		seenCMDSize = 1;
	}

	if(seenCMDCount >= seenCMDSize){
		seenCMDs = krealloc(seenCMDs, seenCMDSize*2, GFP_KERNEL);
		seenCMDSize *= 2;
	}

	seenCMDs[seenCMDCount] = instr;
	seenCMDCount++;
	printk("New instruction: %x", instr);
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

	//instrDBG(instr);

	uint8_t op0;
	int r;
	struct fixupDescription desc = {0};
	//desc.starttime = ktime_get_ns();
	op0 = ((instr & 0x1E000000) >> 25);
	if((op0 & 5) == 0x4){
		//printk("Load/Store\n");
		r = ls_fixup(instr, regs, &desc);
		//desc.endtime = ktime_get_ns();
		/*printk("Trap timing: decoding: %ldns, mem ops: %ldns, total: %ldns\n", desc.decodedtime - desc.starttime,
				desc.endtime - desc.decodedtime, desc.endtime - desc.starttime);
				*/
		if(r){
			printk("Faulting instruction: 0x%lx\n", instr);
		}

		return r;
	} else if((op0 & 0xe) == 0xa){
		// System instructions, needed for dc zva
		return branch_except_system_fixup(instr, regs, &desc);
	}else {
		printk("Not handling instruction with op0 0x%x (instruction is 0x%08x)",op0, instr);
	}
	return -1;
}
