// SPDX-License-Identifier: GPL-2.0-only
/*
 * Branch Record Buffer Extension Driver.
 *
 * Copyright (C) 2022-2023 ARM Limited
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#include "arm_pmuv3_branch.h"

#define BRBFCR_EL1_BRANCH_FILTERS (BRBFCR_EL1_DIRECT   | \
				   BRBFCR_EL1_INDIRECT | \
				   BRBFCR_EL1_RTN      | \
				   BRBFCR_EL1_INDCALL  | \
				   BRBFCR_EL1_DIRCALL  | \
				   BRBFCR_EL1_CONDDIR)

#define BRBFCR_EL1_CONFIG_MASK    (BRBFCR_EL1_BANK_MASK | \
				   BRBFCR_EL1_PAUSED    | \
				   BRBFCR_EL1_EnI       | \
				   BRBFCR_EL1_BRANCH_FILTERS)

/*
 * BRBTS_EL1 is currently not used for branch stack implementation
 * purpose but BRBCR_ELx.TS needs to have a valid value from all
 * available options. BRBCR_ELx_TS_VIRTUAL is selected for this.
 */
#define BRBCR_ELx_DEFAULT_TS      FIELD_PREP(BRBCR_ELx_TS_MASK, BRBCR_ELx_TS_VIRTUAL)

#define BRBCR_ELx_CONFIG_MASK     (BRBCR_ELx_EXCEPTION | \
				   BRBCR_ELx_ERTN      | \
				   BRBCR_ELx_CC        | \
				   BRBCR_ELx_MPRED     | \
				   BRBCR_ELx_ExBRE     | \
				   BRBCR_ELx_E0BRE     | \
				   BRBCR_ELx_FZP       | \
				   BRBCR_ELx_TS_MASK)

/*
 * BRBE Buffer Organization
 *
 * BRBE buffer is arranged as multiple banks of 32 branch record
 * entries each. An individual branch record in a given bank could
 * be accessed, after selecting the bank in BRBFCR_EL1.BANK and
 * accessing the registers i.e [BRBSRC, BRBTGT, BRBINF] set with
 * indices [0..31].
 *
 * Bank 0
 *
 *	---------------------------------	------
 *	| 00 | BRBSRC | BRBTGT | BRBINF |	| 00 |
 *	---------------------------------	------
 *	| 01 | BRBSRC | BRBTGT | BRBINF |	| 01 |
 *	---------------------------------	------
 *	| .. | BRBSRC | BRBTGT | BRBINF |	| .. |
 *	---------------------------------	------
 *	| 31 | BRBSRC | BRBTGT | BRBINF |	| 31 |
 *	---------------------------------	------
 *
 * Bank 1
 *
 *	---------------------------------	------
 *	| 32 | BRBSRC | BRBTGT | BRBINF |	| 00 |
 *	---------------------------------	------
 *	| 33 | BRBSRC | BRBTGT | BRBINF |	| 01 |
 *	---------------------------------	------
 *	| .. | BRBSRC | BRBTGT | BRBINF |	| .. |
 *	---------------------------------	------
 *	| 63 | BRBSRC | BRBTGT | BRBINF |	| 31 |
 *	---------------------------------	------
 */
#define BRBE_BANK_MAX_ENTRIES 32
#define BRBE_MAX_BANK 2
#define BRBE_MAX_ENTRIES (BRBE_BANK_MAX_ENTRIES * BRBE_MAX_BANK)

#define BRBE_BANK0_IDX_MIN 0
#define BRBE_BANK0_IDX_MAX 31
#define BRBE_BANK1_IDX_MIN 32
#define BRBE_BANK1_IDX_MAX 63

struct brbe_regset {
	unsigned long brbsrc;
	unsigned long brbtgt;
	unsigned long brbinf;
};

#define PERF_BR_ARM64_MAX (PERF_BR_MAX + PERF_BR_NEW_MAX)

struct arm64_perf_task_context {
	struct brbe_regset store[BRBE_MAX_ENTRIES];
	int nr_brbe_records;

	/*
	 * Branch Filter Mask
	 *
	 * This mask represents all branch record types i.e PERF_BR_XXX
	 * (as defined in core perf ABI) that can be generated with the
	 * event's branch_sample_type request. The mask layout could be
	 * found here. Although the bit 15 i.e PERF_BR_EXTEND_ABI never
	 * gets set in the mask.
	 *
	 * 23 (PERF_BR_MAX + PERF_BR_NEW_MAX)                      0
	 * |                                                       |
	 * ---------------------------------------------------------
	 * | Extended ABI section  | X |    ABI section            |
	 * ---------------------------------------------------------
	 */
	DECLARE_BITMAP(br_type_mask, PERF_BR_ARM64_MAX);
};

static void branch_mask_set_all(unsigned long *event_type_mask)
{
	int idx;

	for (idx = PERF_BR_UNKNOWN; idx < PERF_BR_EXTEND_ABI; idx++)
		set_bit(idx, event_type_mask);

	for (idx = PERF_BR_NEW_FAULT_ALGN; idx < PERF_BR_NEW_MAX; idx++)
		set_bit(PERF_BR_MAX + idx, event_type_mask);
}

static void branch_mask_set_arch(unsigned long *event_type_mask)
{
	set_bit(PERF_BR_MAX + PERF_BR_NEW_FAULT_ALGN, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_NEW_FAULT_DATA, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_NEW_FAULT_INST, event_type_mask);

	set_bit(PERF_BR_MAX + PERF_BR_ARM64_FIQ, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_HALT, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_EXIT, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_INST, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_DATA, event_type_mask);
}

static void branch_entry_mask(struct perf_branch_entry *entry,
			      unsigned long *event_type_mask)
{
	u64 idx;

	bitmap_zero(event_type_mask, PERF_BR_ARM64_MAX);
	for (idx = PERF_BR_UNKNOWN; idx < PERF_BR_EXTEND_ABI; idx++) {
		if (entry->type == idx)
			set_bit(idx, event_type_mask);
	}

	if (entry->type == PERF_BR_EXTEND_ABI) {
		for (idx = PERF_BR_NEW_FAULT_ALGN; idx < PERF_BR_NEW_MAX; idx++) {
			if (entry->new_type == idx)
				set_bit(PERF_BR_MAX + idx, event_type_mask);
		}
	}
}

static void prepare_event_branch_type_mask(struct perf_event *event,
					   unsigned long *event_type_mask)
{
	u64 branch_sample = event->attr.branch_sample_type;

	bitmap_zero(event_type_mask, PERF_BR_ARM64_MAX);

	/*
	 * The platform specific branch types might not follow event's
	 * branch filter requests accurately. Let's add all of them as
	 * acceptible branch types during the filtering process.
	 */
	branch_mask_set_arch(event_type_mask);

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY) {
		branch_mask_set_all(event_type_mask);
		return;
	}

	if (branch_sample & PERF_SAMPLE_BRANCH_IND_JUMP)
		set_bit(PERF_BR_IND, event_type_mask);

	set_bit(PERF_BR_UNCOND, event_type_mask);
	if (branch_sample & PERF_SAMPLE_BRANCH_COND) {
		clear_bit(PERF_BR_UNCOND, event_type_mask);
		set_bit(PERF_BR_COND, event_type_mask);
	}

	if (branch_sample & PERF_SAMPLE_BRANCH_CALL)
		set_bit(PERF_BR_CALL, event_type_mask);

	if (branch_sample & PERF_SAMPLE_BRANCH_IND_CALL)
		set_bit(PERF_BR_IND_CALL, event_type_mask);

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY_CALL) {
		set_bit(PERF_BR_CALL, event_type_mask);
		set_bit(PERF_BR_IRQ, event_type_mask);
		set_bit(PERF_BR_SYSCALL, event_type_mask);
		set_bit(PERF_BR_SERROR, event_type_mask);

		if (branch_sample & PERF_SAMPLE_BRANCH_COND)
			set_bit(PERF_BR_COND_CALL, event_type_mask);
	}

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY_RETURN) {
		set_bit(PERF_BR_RET, event_type_mask);
		set_bit(PERF_BR_ERET, event_type_mask);
		set_bit(PERF_BR_SYSRET, event_type_mask);

		if (branch_sample & PERF_SAMPLE_BRANCH_COND)
			set_bit(PERF_BR_COND_RET, event_type_mask);
	}
}

struct brbe_hw_attr {
	int	brbe_version;
	int	brbe_cc;
	int	brbe_nr;
	int	brbe_format;
};

#define RETURN_READ_BRBSRCN(n) \
	read_sysreg_s(SYS_BRBSRC_EL1(n))

#define RETURN_READ_BRBTGTN(n) \
	read_sysreg_s(SYS_BRBTGT_EL1(n))

#define RETURN_READ_BRBINFN(n) \
	read_sysreg_s(SYS_BRBINF_EL1(n))

#define BRBE_REGN_CASE(n, case_macro) \
	case n: return case_macro(n); break

#define BRBE_REGN_SWITCH(x, case_macro)				\
	do {							\
		switch (x) {					\
		BRBE_REGN_CASE(0, case_macro);			\
		BRBE_REGN_CASE(1, case_macro);			\
		BRBE_REGN_CASE(2, case_macro);			\
		BRBE_REGN_CASE(3, case_macro);			\
		BRBE_REGN_CASE(4, case_macro);			\
		BRBE_REGN_CASE(5, case_macro);			\
		BRBE_REGN_CASE(6, case_macro);			\
		BRBE_REGN_CASE(7, case_macro);			\
		BRBE_REGN_CASE(8, case_macro);			\
		BRBE_REGN_CASE(9, case_macro);			\
		BRBE_REGN_CASE(10, case_macro);			\
		BRBE_REGN_CASE(11, case_macro);			\
		BRBE_REGN_CASE(12, case_macro);			\
		BRBE_REGN_CASE(13, case_macro);			\
		BRBE_REGN_CASE(14, case_macro);			\
		BRBE_REGN_CASE(15, case_macro);			\
		BRBE_REGN_CASE(16, case_macro);			\
		BRBE_REGN_CASE(17, case_macro);			\
		BRBE_REGN_CASE(18, case_macro);			\
		BRBE_REGN_CASE(19, case_macro);			\
		BRBE_REGN_CASE(20, case_macro);			\
		BRBE_REGN_CASE(21, case_macro);			\
		BRBE_REGN_CASE(22, case_macro);			\
		BRBE_REGN_CASE(23, case_macro);			\
		BRBE_REGN_CASE(24, case_macro);			\
		BRBE_REGN_CASE(25, case_macro);			\
		BRBE_REGN_CASE(26, case_macro);			\
		BRBE_REGN_CASE(27, case_macro);			\
		BRBE_REGN_CASE(28, case_macro);			\
		BRBE_REGN_CASE(29, case_macro);			\
		BRBE_REGN_CASE(30, case_macro);			\
		BRBE_REGN_CASE(31, case_macro);			\
		default:					\
			pr_warn("unknown register index\n");	\
			return -1;				\
		}						\
	} while (0)

static inline int buffer_to_brbe_idx(int buffer_idx)
{
	return buffer_idx % BRBE_BANK_MAX_ENTRIES;
}

static inline u64 get_brbsrc_reg(int buffer_idx)
{
	int brbe_idx = buffer_to_brbe_idx(buffer_idx);

	BRBE_REGN_SWITCH(brbe_idx, RETURN_READ_BRBSRCN);
}

static inline u64 get_brbtgt_reg(int buffer_idx)
{
	int brbe_idx = buffer_to_brbe_idx(buffer_idx);

	BRBE_REGN_SWITCH(brbe_idx, RETURN_READ_BRBTGTN);
}

static inline u64 get_brbinf_reg(int buffer_idx)
{
	int brbe_idx = buffer_to_brbe_idx(buffer_idx);

	BRBE_REGN_SWITCH(brbe_idx, RETURN_READ_BRBINFN);
}

static inline u64 brbe_record_valid(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_VALID_MASK, brbinf);
}

static inline bool brbe_invalid(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_NONE;
}

static inline bool brbe_record_is_complete(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_FULL;
}

static inline bool brbe_record_is_source_only(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_SOURCE;
}

static inline bool brbe_record_is_target_only(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_TARGET;
}

static inline int brbe_get_in_tx(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_T_MASK, brbinf);
}

static inline int brbe_get_mispredict(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_MPRED_MASK, brbinf);
}

static inline int brbe_get_lastfailed(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_LASTFAILED_MASK, brbinf);
}

static inline int brbe_get_cycles(u64 brbinf)
{
	/*
	 * Captured cycle count is unknown and hence
	 * should not be passed on to the user space.
	 */
	if (brbinf & BRBINFx_EL1_CCU)
		return 0;

	return FIELD_GET(BRBINFx_EL1_CC_MASK, brbinf);
}

static inline int brbe_get_type(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_TYPE_MASK, brbinf);
}

static inline int brbe_get_el(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_EL_MASK, brbinf);
}

static inline int brbe_get_numrec(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_NUMREC_MASK, brbidr);
}

static inline int brbe_get_format(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_FORMAT_MASK, brbidr);
}

static inline int brbe_get_cc_bits(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_CC_MASK, brbidr);
}

void armv8pmu_branch_stack_reset(void)
{
	asm volatile(BRB_IALL_INSN);
	isb();
}

void armv8pmu_branch_stack_add(struct perf_event *event, struct pmu_hw_events *hw_events)
{
	struct arm64_perf_task_context *task_ctx = event->pmu_ctx->task_ctx_data;

	if (event->ctx->task)
		prepare_event_branch_type_mask(event, task_ctx->br_type_mask);

	/*
	 * Reset branch records buffer if a new CPU bound event
	 * gets scheduled on a PMU. Otherwise existing branch
	 * records present in the buffer might just leak into
	 * such events.
	 *
	 * Also reset current 'hw_events->branch_context' because
	 * any previous task bound event now would have lost an
	 * opportunity for continuous branch records.
	 */
	if (!event->ctx->task) {
		hw_events->branch_context = NULL;
		armv8pmu_branch_stack_reset();
	}

	/*
	 * Reset branch records buffer if a new task event gets
	 * scheduled on a PMU which might have existing records.
	 * Otherwise older branch records present in the buffer
	 * might leak into the new task event.
	 */
	if (event->ctx->task && hw_events->branch_context != event->ctx) {
		hw_events->branch_context = event->ctx;
		armv8pmu_branch_stack_reset();
	}
	hw_events->branch_users++;
}

void armv8pmu_branch_stack_del(struct perf_event *event, struct pmu_hw_events *hw_events)
{
	WARN_ON_ONCE(!hw_events->branch_users);
	hw_events->branch_users--;
	if (!hw_events->branch_users) {
		hw_events->branch_context = NULL;
		hw_events->branch_sample_type = 0;
	}
}

static bool valid_brbe_nr(int brbe_nr)
{
	return brbe_nr == BRBIDR0_EL1_NUMREC_8 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_16 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_32 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_64;
}

static bool valid_brbe_cc(int brbe_cc)
{
	return brbe_cc == BRBIDR0_EL1_CC_20_BIT;
}

static bool valid_brbe_format(int brbe_format)
{
	return brbe_format == BRBIDR0_EL1_FORMAT_FORMAT_0;
}

static bool valid_brbe_version(int brbe_version)
{
	return brbe_version == ID_AA64DFR0_EL1_BRBE_IMP ||
	       brbe_version == ID_AA64DFR0_EL1_BRBE_BRBE_V1P1;
}

static void select_brbe_bank(int bank)
{
	u64 brbfcr;

	WARN_ON(bank > 1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbfcr &= ~BRBFCR_EL1_BANK_MASK;
	brbfcr |= SYS_FIELD_PREP(BRBFCR_EL1, BANK, bank);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();
}

static bool __read_brbe_regset(struct brbe_regset *entry, int idx)
{
	entry->brbinf = get_brbinf_reg(idx);

	if (brbe_invalid(entry->brbinf))
		return false;

	entry->brbsrc = get_brbsrc_reg(idx);
	entry->brbtgt = get_brbtgt_reg(idx);
	return true;
}

/*
 * Read all BRBE entries in HW until the first invalid entry.
 *
 * The caller must ensure that the BRBE is not concurrently modifying these
 * branch entries.
 */
static int capture_brbe_regset(struct brbe_regset *buf, int nr_hw_entries)
{
	int idx = 0;

	select_brbe_bank(0);
	while (idx < nr_hw_entries && idx <= BRBE_BANK0_IDX_MAX) {
		if (!__read_brbe_regset(&buf[idx], idx))
			return idx;
		idx++;
	}

	select_brbe_bank(1);
	while (idx < nr_hw_entries && idx <= BRBE_BANK1_IDX_MAX) {
		if (!__read_brbe_regset(&buf[idx], idx))
			return idx;
		idx++;
	}
	return idx;
}

/*
 * This function concatenates branch records from stored and live buffer
 * up to maximum nr_max records and the stored buffer holds the resultant
 * buffer. The concatenated buffer contains all the branch records from
 * the live buffer but might contain some from stored buffer considering
 * the maximum combined length does not exceed 'nr_max'.
 *
 *	Stored records	Live records
 *	------------------------------------------------^
 *	|	S0	|	L0	|	Newest	|
 *	---------------------------------		|
 *	|	S1	|	L1	|		|
 *	---------------------------------		|
 *	|	S2	|	L2	|		|
 *	---------------------------------		|
 *	|	S3	|	L3	|		|
 *	---------------------------------		|
 *	|	S4	|	L4	|		nr_max
 *	---------------------------------		|
 *	|		|	L5	|		|
 *	---------------------------------		|
 *	|		|	L6	|		|
 *	---------------------------------		|
 *	|		|	L7	|		|
 *	---------------------------------		|
 *	|		|		|		|
 *	---------------------------------		|
 *	|		|		|	Oldest	|
 *	------------------------------------------------V
 *
 *
 * S0 is the newest in the stored records, where as L7 is the oldest in
 * the live records. Unless the live buffer is detected as being full
 * thus potentially dropping off some older records, L7 and S0 records
 * are contiguous in time for a user task context. The stitched buffer
 * here represents maximum possible branch records, contiguous in time.
 *
 *	Stored records  Live records
 *	------------------------------------------------^
 *	|	L0	|	L0	|	Newest	|
 *	---------------------------------		|
 *	|	L1	|	L1	|		|
 *	---------------------------------		|
 *	|	L2	|	L2	|		|
 *	---------------------------------		|
 *	|	L3	|	L3	|		|
 *	---------------------------------		|
 *	|	L4	|	L4	|	      nr_max
 *	---------------------------------		|
 *	|	L5	|	L5	|		|
 *	---------------------------------		|
 *	|	L6	|	L6	|		|
 *	---------------------------------		|
 *	|	L7	|	L7	|		|
 *	---------------------------------		|
 *	|	S0	|		|		|
 *	---------------------------------		|
 *	|	S1	|		|    Oldest	|
 *	------------------------------------------------V
 *	|	S2	| <----|
 *	-----------------      |
 *	|	S3	| <----| Dropped off after nr_max
 *	-----------------      |
 *	|	S4	| <----|
 *	-----------------
 */
static int stitch_stored_live_entries(struct brbe_regset *stored,
				      struct brbe_regset *live,
				      int nr_stored, int nr_live,
				      int nr_max)
{
	int nr_move = min(nr_stored, nr_max - nr_live);

	/* Move the tail of the buffer to make room for the new entries */
	memmove(&stored[nr_live], &stored[0], nr_move * sizeof(*stored));

	/* Copy the new entries into the head of the buffer */
	memcpy(&stored[0], &live[0], nr_live * sizeof(*stored));

	/* Return the number of entries in the stitched buffer */
	return min(nr_live + nr_stored, nr_max);
}

static int brbe_branch_save(struct brbe_regset *live, int nr_hw_entries)
{
	u64 brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	int nr_live;

	write_sysreg_s(brbfcr | BRBFCR_EL1_PAUSED, SYS_BRBFCR_EL1);
	isb();

	nr_live = capture_brbe_regset(live, nr_hw_entries);

	write_sysreg_s(brbfcr & ~BRBFCR_EL1_PAUSED, SYS_BRBFCR_EL1);
	isb();

	return nr_live;
}

void armv8pmu_branch_save(struct arm_pmu *arm_pmu, void *ctx)
{
	struct arm64_perf_task_context *task_ctx = ctx;
	struct brbe_regset live[BRBE_MAX_ENTRIES];
	int nr_live, nr_store, nr_hw_entries;

	nr_hw_entries = brbe_get_numrec(arm_pmu->reg_brbidr);
	nr_live = brbe_branch_save(live, nr_hw_entries);
	nr_store = task_ctx->nr_brbe_records;
	nr_store = stitch_stored_live_entries(task_ctx->store, live, nr_store,
					      nr_live, nr_hw_entries);
	task_ctx->nr_brbe_records = nr_store;
}

/*
 * Generic perf branch filters supported on BRBE
 *
 * New branch filters need to be evaluated whether they could be supported on
 * BRBE. This ensures that such branch filters would not just be accepted, to
 * fail silently. PERF_SAMPLE_BRANCH_HV is a special case that is selectively
 * supported only on platforms where kernel is in hyp mode.
 */
#define BRBE_EXCLUDE_BRANCH_FILTERS (PERF_SAMPLE_BRANCH_ABORT_TX	| \
				     PERF_SAMPLE_BRANCH_IN_TX		| \
				     PERF_SAMPLE_BRANCH_NO_TX		| \
				     PERF_SAMPLE_BRANCH_CALL_STACK	| \
				     PERF_SAMPLE_BRANCH_COUNTERS)

#define BRBE_ALLOWED_BRANCH_FILTERS (PERF_SAMPLE_BRANCH_USER		| \
				     PERF_SAMPLE_BRANCH_KERNEL		| \
				     PERF_SAMPLE_BRANCH_HV		| \
				     PERF_SAMPLE_BRANCH_ANY		| \
				     PERF_SAMPLE_BRANCH_ANY_CALL	| \
				     PERF_SAMPLE_BRANCH_ANY_RETURN	| \
				     PERF_SAMPLE_BRANCH_IND_CALL	| \
				     PERF_SAMPLE_BRANCH_COND		| \
				     PERF_SAMPLE_BRANCH_IND_JUMP	| \
				     PERF_SAMPLE_BRANCH_CALL		| \
				     PERF_SAMPLE_BRANCH_NO_FLAGS	| \
				     PERF_SAMPLE_BRANCH_NO_CYCLES	| \
				     PERF_SAMPLE_BRANCH_TYPE_SAVE	| \
				     PERF_SAMPLE_BRANCH_HW_INDEX	| \
				     PERF_SAMPLE_BRANCH_PRIV_SAVE)

#define BRBE_PERF_BRANCH_FILTERS    (BRBE_ALLOWED_BRANCH_FILTERS	| \
				     BRBE_EXCLUDE_BRANCH_FILTERS)

bool armv8pmu_branch_attr_valid(struct perf_event *event)
{
	u64 branch_type = event->attr.branch_sample_type;

	/*
	 * Ensure both perf branch filter allowed and exclude
	 * masks are always in sync with the generic perf ABI.
	 */
	BUILD_BUG_ON(BRBE_PERF_BRANCH_FILTERS != (PERF_SAMPLE_BRANCH_MAX - 1));

	if (branch_type & ~BRBE_ALLOWED_BRANCH_FILTERS) {
		pr_debug_once("requested branch filter not supported 0x%llx\n", branch_type);
		return false;
	}

	/*
	 * If the event does not have at least one of the privilege
	 * branch filters as in PERF_SAMPLE_BRANCH_PLM_ALL, the core
	 * perf will adjust its value based on perf event's existing
	 * privilege level via attr.exclude_[user|kernel|hv].
	 *
	 * As event->attr.branch_sample_type might have been changed
	 * when the event reaches here, it is not possible to figure
	 * out whether the event originally had HV privilege request
	 * or got added via the core perf. Just report this situation
	 * once and continue ignoring if there are other instances.
	 */
	if ((branch_type & PERF_SAMPLE_BRANCH_HV) && !is_kernel_in_hyp_mode())
		pr_debug_once("hypervisor privilege filter not supported 0x%llx\n", branch_type);

	return true;
}

int armv8pmu_task_ctx_cache_alloc(struct arm_pmu *arm_pmu)
{
	size_t size = sizeof(struct arm64_perf_task_context);

	arm_pmu->pmu.task_ctx_cache = kmem_cache_create("arm64_brbe_task_ctx", size, 0, 0, NULL);
	if (!arm_pmu->pmu.task_ctx_cache)
		return -ENOMEM;
	return 0;
}

void armv8pmu_task_ctx_cache_free(struct arm_pmu *arm_pmu)
{
	kmem_cache_destroy(arm_pmu->pmu.task_ctx_cache);
}

static int brbe_attributes_probe(struct arm_pmu *armpmu, u32 brbe)
{
	u64 brbidr = read_sysreg_s(SYS_BRBIDR0_EL1);
	int brbe_version, brbe_format, brbe_cc, brbe_nr;

	brbe_version = brbe;
	brbe_format = brbe_get_format(brbidr);
	brbe_cc = brbe_get_cc_bits(brbidr);
	brbe_nr = brbe_get_numrec(brbidr);
	armpmu->reg_brbidr = brbidr;

	if (!valid_brbe_version(brbe_version) ||
	   !valid_brbe_format(brbe_format) ||
	   !valid_brbe_cc(brbe_cc) ||
	   !valid_brbe_nr(brbe_nr))
		return -EOPNOTSUPP;
	return 0;
}

void armv8pmu_branch_probe(struct arm_pmu *armpmu)
{
	u64 aa64dfr0 = read_sysreg_s(SYS_ID_AA64DFR0_EL1);
	u32 brbe;

	/*
	 * BRBE implementation's branch entries cannot exceed maximum
	 * branch records supported at the ARM PMU level abstraction.
	 * Otherwise there is always a possibility of array overflow,
	 * while processing BRBE branch records.
	 */
	BUILD_BUG_ON(BRBE_BANK_MAX_ENTRIES > MAX_BRANCH_RECORDS);

	brbe = cpuid_feature_extract_unsigned_field(aa64dfr0, ID_AA64DFR0_EL1_BRBE_SHIFT);
	if (!brbe)
		return;

	if (brbe_attributes_probe(armpmu, brbe))
		return;

	armpmu->has_branch_stack = 1;
}

/*
 * BRBE supports the following functional branch type filters while
 * generating branch records. These branch filters can be enabled,
 * either individually or as a group i.e ORing multiple filters
 * with each other.
 *
 * BRBFCR_EL1_CONDDIR  - Conditional direct branch
 * BRBFCR_EL1_DIRCALL  - Direct call
 * BRBFCR_EL1_INDCALL  - Indirect call
 * BRBFCR_EL1_INDIRECT - Indirect branch
 * BRBFCR_EL1_DIRECT   - Direct branch
 * BRBFCR_EL1_RTN      - Subroutine return
 */
static u64 branch_type_to_brbfcr(int branch_type)
{
	u64 brbfcr = 0;

	if (branch_type & PERF_SAMPLE_BRANCH_ANY) {
		brbfcr |= BRBFCR_EL1_BRANCH_FILTERS;
		return brbfcr;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_CALL) {
		brbfcr |= BRBFCR_EL1_INDCALL;
		brbfcr |= BRBFCR_EL1_DIRCALL;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_RETURN)
		brbfcr |= BRBFCR_EL1_RTN;

	if (branch_type & PERF_SAMPLE_BRANCH_IND_CALL)
		brbfcr |= BRBFCR_EL1_INDCALL;

	if (branch_type & PERF_SAMPLE_BRANCH_COND)
		brbfcr |= BRBFCR_EL1_CONDDIR;

	if (branch_type & PERF_SAMPLE_BRANCH_IND_JUMP)
		brbfcr |= BRBFCR_EL1_INDIRECT;

	if (branch_type & PERF_SAMPLE_BRANCH_CALL)
		brbfcr |= BRBFCR_EL1_DIRCALL;

	return brbfcr & BRBFCR_EL1_CONFIG_MASK;
}

/*
 * BRBE supports the following privilege mode filters while generating
 * branch records.
 *
 * BRBCR_ELx_E0BRE - EL0 branch records
 * BRBCR_ELx_ExBRE - EL1/EL2 branch records
 *
 * BRBE also supports the following additional functional branch type
 * filters while generating branch records.
 *
 * BRBCR_ELx_EXCEPTION - Exception
 * BRBCR_ELx_ERTN     -  Exception return
 */
static u64 branch_type_to_brbcr(int branch_type)
{
	u64 brbcr = BRBCR_ELx_DEFAULT_TS;

	/*
	 * BRBE should be paused on PMU interrupt while tracing kernel
	 * space to stop capturing further branch records. Otherwise
	 * interrupt handler branch records might get into the samples
	 * which is not desired.
	 *
	 * BRBE need not be paused on PMU interrupt while tracing only
	 * the user space, because it will automatically be inside the
	 * prohibited region. But even after PMU overflow occurs, the
	 * interrupt could still take much more cycles, before it can
	 * be taken and by that time BRBE will have been overwritten.
	 * Hence enable pause on PMU interrupt mechanism even for user
	 * only traces as well.
	 */
	brbcr |= BRBCR_ELx_FZP;

	if (branch_type & PERF_SAMPLE_BRANCH_USER)
		brbcr |= BRBCR_ELx_E0BRE;

	/*
	 * When running in the hyp mode, writing into BRBCR_EL1
	 * actually writes into BRBCR_EL2 instead. Field E2BRE
	 * is also at the same position as E1BRE.
	 */
	if (branch_type & PERF_SAMPLE_BRANCH_KERNEL)
		brbcr |= BRBCR_ELx_ExBRE;

	if (branch_type & PERF_SAMPLE_BRANCH_HV) {
		if (is_kernel_in_hyp_mode())
			brbcr |= BRBCR_ELx_ExBRE;
	}

	if (!(branch_type & PERF_SAMPLE_BRANCH_NO_CYCLES))
		brbcr |= BRBCR_ELx_CC;

	if (!(branch_type & PERF_SAMPLE_BRANCH_NO_FLAGS))
		brbcr |= BRBCR_ELx_MPRED;

	/*
	 * The exception and exception return branches could be
	 * captured, irrespective of the perf event's privilege.
	 * If the perf event does not have enough privilege for
	 * a given exception level, then addresses which falls
	 * under that exception level will be reported as zero
	 * for the captured branch record, creating source only
	 * or target only records.
	 */
	if (branch_type & PERF_SAMPLE_BRANCH_ANY) {
		brbcr |= BRBCR_ELx_EXCEPTION;
		brbcr |= BRBCR_ELx_ERTN;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_CALL)
		brbcr |= BRBCR_ELx_EXCEPTION;

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_RETURN)
		brbcr |= BRBCR_ELx_ERTN;

	return brbcr & BRBCR_ELx_CONFIG_MASK;
}

void armv8pmu_branch_enable(struct arm_pmu *arm_pmu)
{
	struct pmu_hw_events *cpuc = this_cpu_ptr(arm_pmu->hw_events);
	u64 brbfcr, brbcr;

	if (!(cpuc->branch_sample_type && cpuc->branch_users))
		return;

	/*
	 * BRBE gets configured with a new mismatched branch sample
	 * type request, overriding any previous branch filters.
	 */
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbfcr &= ~BRBFCR_EL1_CONFIG_MASK;
	brbfcr |= branch_type_to_brbfcr(cpuc->branch_sample_type);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();

	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbcr &= ~BRBCR_ELx_CONFIG_MASK;
	brbcr |= branch_type_to_brbcr(cpuc->branch_sample_type);
	write_sysreg_s(brbcr, SYS_BRBCR_EL1);
	isb();
}

void armv8pmu_branch_disable(void)
{
	u64 brbfcr, brbcr;

	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbcr &= ~(BRBCR_ELx_E0BRE | BRBCR_ELx_ExBRE);
	brbfcr |= BRBFCR_EL1_PAUSED;
	write_sysreg_s(brbcr, SYS_BRBCR_EL1);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();
}

static void brbe_set_perf_entry_type(struct perf_branch_entry *entry, u64 brbinf)
{
	int brbe_type = brbe_get_type(brbinf);

	switch (brbe_type) {
	case BRBINFx_EL1_TYPE_DIRECT_UNCOND:
		entry->type = PERF_BR_UNCOND;
		break;
	case BRBINFx_EL1_TYPE_INDIRECT:
		entry->type = PERF_BR_IND;
		break;
	case BRBINFx_EL1_TYPE_DIRECT_LINK:
		entry->type = PERF_BR_CALL;
		break;
	case BRBINFx_EL1_TYPE_INDIRECT_LINK:
		entry->type = PERF_BR_IND_CALL;
		break;
	case BRBINFx_EL1_TYPE_RET:
		entry->type = PERF_BR_RET;
		break;
	case BRBINFx_EL1_TYPE_DIRECT_COND:
		entry->type = PERF_BR_COND;
		break;
	case BRBINFx_EL1_TYPE_CALL:
		entry->type = PERF_BR_CALL;
		break;
	case BRBINFx_EL1_TYPE_TRAP:
		entry->type = PERF_BR_SYSCALL;
		break;
	case BRBINFx_EL1_TYPE_ERET:
		entry->type = PERF_BR_ERET;
		break;
	case BRBINFx_EL1_TYPE_IRQ:
		entry->type = PERF_BR_IRQ;
		break;
	case BRBINFx_EL1_TYPE_DEBUG_HALT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_HALT;
		break;
	case BRBINFx_EL1_TYPE_SERROR:
		entry->type = PERF_BR_SERROR;
		break;
	case BRBINFx_EL1_TYPE_INSN_DEBUG:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_INST;
		break;
	case BRBINFx_EL1_TYPE_DATA_DEBUG:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_DATA;
		break;
	case BRBINFx_EL1_TYPE_ALIGN_FAULT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_NEW_FAULT_ALGN;
		break;
	case BRBINFx_EL1_TYPE_INSN_FAULT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_NEW_FAULT_INST;
		break;
	case BRBINFx_EL1_TYPE_DATA_FAULT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_NEW_FAULT_DATA;
		break;
	case BRBINFx_EL1_TYPE_FIQ:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_FIQ;
		break;
	case BRBINFx_EL1_TYPE_DEBUG_EXIT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_EXIT;
		break;
	default:
		pr_warn_once("%d - unknown branch type captured\n", brbe_type);
		entry->type = PERF_BR_UNKNOWN;
		break;
	}
}

static int brbe_get_perf_priv(u64 brbinf)
{
	int brbe_el = brbe_get_el(brbinf);

	switch (brbe_el) {
	case BRBINFx_EL1_EL_EL0:
		return PERF_BR_PRIV_USER;
	case BRBINFx_EL1_EL_EL1:
		return PERF_BR_PRIV_KERNEL;
	case BRBINFx_EL1_EL_EL2:
		if (is_kernel_in_hyp_mode())
			return PERF_BR_PRIV_KERNEL;
		return PERF_BR_PRIV_HV;
	default:
		pr_warn_once("%d - unknown branch privilege captured\n", brbe_el);
		return PERF_BR_PRIV_UNKNOWN;
	}
}

static void capture_brbe_flags(struct perf_branch_entry *entry, struct perf_event *event,
			       u64 brbinf)
{
	brbe_set_perf_entry_type(entry, brbinf);

	if (!branch_sample_no_cycles(event))
		entry->cycles = brbe_get_cycles(brbinf);

	if (!branch_sample_no_flags(event)) {
		/*
		 * BRBINFx_EL1.LASTFAILED indicates that a TME transaction failed (or
		 * was cancelled) prior to this record, and some number of records
		 * prior to this one, may have been generated during an attempt to
		 * execute the transaction.
		 */
		entry->abort = brbe_get_lastfailed(brbinf);

		/*
		 * All these information (i.e transaction state and mispredicts)
		 * are available for source only and complete branch records.
		 */
		if (brbe_record_is_complete(brbinf) ||
		    brbe_record_is_source_only(brbinf)) {
			entry->mispred = brbe_get_mispredict(brbinf);
			entry->predicted = !entry->mispred;
			entry->in_tx = brbe_get_in_tx(brbinf);
		}

		/*
		 * Currently TME feature is neither implemented in any hardware
		 * nor it is being supported in the kernel. Just warn here once
		 * if TME related information shows up rather unexpectedly.
		 */
		if (entry->abort || entry->in_tx)
			pr_warn_once("Unknown transaction states %d %d\n",
				      entry->abort, entry->in_tx);
	}

	/*
	 * All these information (i.e branch privilege level) are
	 * available for target only and complete branch records.
	 */
	if (brbe_record_is_complete(brbinf) ||
	    brbe_record_is_target_only(brbinf))
		entry->priv = brbe_get_perf_priv(brbinf);
}

static void brbe_regset_branch_entries(struct pmu_hw_events *cpuc, struct perf_event *event,
				       struct brbe_regset *regset, int idx)
{
	struct perf_branch_entry *entry = &cpuc->branches->branch_entries[idx];
	u64 brbinf = regset[idx].brbinf;

	perf_clear_branch_entry_bitfields(entry);
	if (brbe_record_is_complete(brbinf)) {
		entry->from = regset[idx].brbsrc;
		entry->to = regset[idx].brbtgt;
	} else if (brbe_record_is_source_only(brbinf)) {
		entry->from = regset[idx].brbsrc;
		entry->to = 0;
	} else if (brbe_record_is_target_only(brbinf)) {
		entry->from = 0;
		entry->to = regset[idx].brbtgt;
	}
	capture_brbe_flags(entry, event, brbinf);
}

static void process_branch_entries(struct pmu_hw_events *cpuc, struct perf_event *event,
				   struct brbe_regset *regset, int nr_regset)
{
	int idx;

	for (idx = 0; idx < nr_regset; idx++)
		brbe_regset_branch_entries(cpuc, event, regset, idx);

	cpuc->branches->branch_stack.nr = nr_regset;
	cpuc->branches->branch_stack.hw_idx = -1ULL;
}

void armv8pmu_branch_read(struct pmu_hw_events *cpuc, struct perf_event *event)
{
	struct arm64_perf_task_context *task_ctx = event->pmu_ctx->task_ctx_data;
	struct brbe_regset live[BRBE_MAX_ENTRIES];
	int nr_live, nr_store, nr_hw_entries;

	nr_hw_entries = brbe_get_numrec(cpuc->percpu_pmu->reg_brbidr);
	nr_live = capture_brbe_regset(live, nr_hw_entries);
	if (event->ctx->task) {
		nr_store = task_ctx->nr_brbe_records;
		nr_store = stitch_stored_live_entries(task_ctx->store, live, nr_store,
						      nr_live, nr_hw_entries);
		process_branch_entries(cpuc, event, task_ctx->store, nr_store);
		task_ctx->nr_brbe_records = 0;
	} else {
		process_branch_entries(cpuc, event, live, nr_live);
	}
}

static bool filter_branch_privilege(struct perf_branch_entry *entry, u64 branch_sample_type)
{
	/*
	 * Retrieve the privilege level branch filter requests
	 * from the overall branch sample type.
	 */
	branch_sample_type &= PERF_SAMPLE_BRANCH_PLM_ALL;

	/*
	 * The privilege information do not always get captured
	 * successfully for given BRBE branch record. Hence the
	 * entry->priv could be analyzed for filtering when the
	 * information has really been captured.
	 */
	if (entry->priv) {
		if (entry->priv == PERF_BR_PRIV_USER) {
			if (!(branch_sample_type & PERF_SAMPLE_BRANCH_USER))
				return false;
		}

		if (entry->priv == PERF_BR_PRIV_KERNEL) {
			if (!(branch_sample_type & PERF_SAMPLE_BRANCH_KERNEL)) {
				if (!is_kernel_in_hyp_mode())
					return false;

				if (!(branch_sample_type & PERF_SAMPLE_BRANCH_HV))
					return false;
			}
		}

		if (entry->priv == PERF_BR_PRIV_HV) {
			/*
			 * PERF_SAMPLE_BRANCH_HV request actually gets configured
			 * similar to PERF_SAMPLE_BRANCH_KERNEL when kernel is in
			 * hyp mode. In that case PERF_BR_PRIV_KERNEL should have
			 * been reported for corresponding branch records.
			 */
			pr_warn_once("PERF_BR_PRIV_HV should not have been captured\n");
		}
		return true;
	}

	if (is_ttbr0_addr(entry->from) || is_ttbr0_addr(entry->to)) {
		if (!(branch_sample_type & PERF_SAMPLE_BRANCH_USER))
			return false;
	}

	if (is_ttbr1_addr(entry->from) || is_ttbr1_addr(entry->to)) {
		if (!(branch_sample_type & PERF_SAMPLE_BRANCH_KERNEL)) {
			if (!is_kernel_in_hyp_mode())
				return false;

			if (!(branch_sample_type & PERF_SAMPLE_BRANCH_HV))
				return false;
		}
	}
	return true;
}

static bool filter_branch_record(struct pmu_hw_events *cpuc,
				 struct perf_event *event,
				 struct perf_branch_entry *entry)
{
	struct arm64_perf_task_context *task_ctx = event->pmu_ctx->task_ctx_data;
	u64 branch_sample = event->attr.branch_sample_type;
	DECLARE_BITMAP(entry_type_mask, PERF_BR_ARM64_MAX);
	DECLARE_BITMAP(event_type_mask, PERF_BR_ARM64_MAX);

	if (!filter_branch_privilege(entry, branch_sample))
		return false;

	if (entry->type == PERF_BR_UNKNOWN)
		return true;

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY)
		return true;

	/*
	 * Both PMU and event branch filters match here except the privilege
	 * filters - which have already been tested earlier. Skip functional
	 * branch type test and just return success.
	 */
	if ((cpuc->branch_sample_type & ~PERF_SAMPLE_BRANCH_PLM_ALL) ==
	    event->attr.branch_sample_type)
		return true;

	branch_entry_mask(entry, entry_type_mask);
	if (task_ctx)
		return bitmap_subset(entry_type_mask, task_ctx->br_type_mask, PERF_BR_ARM64_MAX);

	prepare_event_branch_type_mask(event, event_type_mask);
	return bitmap_subset(entry_type_mask, event_type_mask, PERF_BR_ARM64_MAX);
}

void arm64_filter_branch_records(struct pmu_hw_events *cpuc,
				 struct perf_event *event,
				 struct branch_records *event_records)
{
	struct perf_branch_entry *entry;
	int idx, count = 0;

	memset(event_records, 0, sizeof(*event_records));
	for (idx = 0; idx < cpuc->branches->branch_stack.nr; idx++) {
		entry = &cpuc->branches->branch_entries[idx];
		if (!filter_branch_record(cpuc, event, entry))
			continue;

		memcpy(&event_records->branch_entries[count], entry, sizeof(*entry));
		count++;
	}
	event_records->branch_stack.nr = count;
}
