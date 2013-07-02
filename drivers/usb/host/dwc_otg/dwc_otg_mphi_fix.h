#ifndef __DWC_OTG_MPHI_FIX_H__
#define __DWC_OTG_MPHI_FIX_H__

#define FIQ_WRITE_IO_ADDRESS(_addr_,_data_) *(volatile uint32_t *) IO_ADDRESS(_addr_) = _data_
#define FIQ_READ_IO_ADDRESS(_addr_) *(volatile uint32_t *) IO_ADDRESS(_addr_)
#define FIQ_MODIFY_IO_ADDRESS(_addr_,_clear_,_set_) FIQ_WRITE_IO_ADDRESS(_addr_ , (FIQ_READ_IO_ADDRESS(_addr_)&~_clear_)|_set_)
#define FIQ_WRITE(_addr_,_data_) *(volatile uint32_t *) _addr_ = _data_

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



#ifdef DEBUG
#define DWC_DBG_PRINT_CORE_INT(_arg_) dwc_debug_print_core_int_reg(_arg_,__func__)
#define DWC_DBG_PRINT_CORE_INT_MASK(_arg_) dwc_debug_core_int_mask(_arg_,__func__)
#define DWC_DBG_PRINT_OTG_INT(_arg_) dwc_debug_otg_int(_arg_,__func__)

#else
#define DWC_DBG_PRINT_CORE_INT(_arg_)
#define DWC_DBG_PRINT_CORE_INT_MASK(_arg_)
#define DWC_DBG_PRINT_OTG_INT(_arg_)


#endif

#endif
