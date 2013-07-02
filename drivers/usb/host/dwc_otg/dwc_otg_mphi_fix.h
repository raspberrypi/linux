#ifndef __DWC_OTG_MPHI_FIX_H__
#define __DWC_OTG_MPHI_FIX_H__
#define FIQ_WRITE(_addr_,_data_) (*(volatile uint32_t *) (_addr_) = (_data_))
#define FIQ_READ(_addr_) (*(volatile uint32_t *) (_addr_))

typedef struct {
	volatile void* base;
	volatile void* ctrl;
	volatile void* outdda;
	volatile void* outddb;
	volatile void* intstat;
} mphi_regs_t;

void dwc_debug_print_core_int_reg(gintsts_data_t gintsts, const char* function_name);
void dwc_debug_core_int_mask(gintsts_data_t gintmsk, const char* function_name);
void dwc_debug_otg_int(gotgint_data_t gotgint, const char* function_name);

extern gintsts_data_t gintsts_saved;

#ifdef DEBUG
#define DWC_DBG_PRINT_CORE_INT(_arg_) dwc_debug_print_core_int_reg(_arg_,__func__)
#define DWC_DBG_PRINT_CORE_INT_MASK(_arg_) dwc_debug_core_int_mask(_arg_,__func__)
#define DWC_DBG_PRINT_OTG_INT(_arg_) dwc_debug_otg_int(_arg_,__func__)

#else
#define DWC_DBG_PRINT_CORE_INT(_arg_)
#define DWC_DBG_PRINT_CORE_INT_MASK(_arg_)
#define DWC_DBG_PRINT_OTG_INT(_arg_)

#endif

typedef enum {
	FIQDBG_SCHED = (1 << 0),
	FIQDBG_INT   = (1 << 1),
	FIQDBG_ERR   = (1 << 2),
	FIQDBG_PORTHUB = (1 << 3),
} FIQDBG_T;

void _fiq_print(FIQDBG_T dbg_lvl, char *fmt, ...);
#ifdef FIQ_DEBUG
#define fiq_print _fiq_print
#else
#define fiq_print(x, y, ...)
#endif

extern bool fiq_fix_enable, nak_holdoff_enable, fiq_split_enable;

#endif
