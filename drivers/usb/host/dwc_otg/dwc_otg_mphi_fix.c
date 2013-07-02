#include "dwc_otg_regs.h"
#include "dwc_otg_dbg.h"

void dwc_debug_print_core_int_reg(gintsts_data_t gintsts, const char* function_name)
{
	DWC_DEBUGPL(DBG_USER,   "*** Debugging from within the %s  function: ***\n"
				"curmode:     %1i    Modemismatch: %1i    otgintr:    %1i    sofintr:    %1i\n"
				"rxstsqlvl:   %1i    nptxfempty  : %1i    ginnakeff:  %1i    goutnakeff: %1i\n"
				"ulpickint:   %1i    i2cintr:      %1i    erlysuspend:%1i    usbsuspend: %1i\n"
				"usbreset:    %1i    enumdone:     %1i    isooutdrop: %1i    eopframe:   %1i\n"
				"restoredone: %1i    epmismatch:   %1i    inepint:    %1i    outepintr:  %1i\n"
				"incomplisoin:%1i    incomplisoout:%1i    fetsusp:    %1i    resetdet:   %1i\n"
				"portintr:    %1i    hcintr:       %1i    ptxfempty:  %1i    lpmtranrcvd:%1i\n"
				"conidstschng:%1i    disconnect:   %1i    sessreqintr:%1i    wkupintr:   %1i\n",
				function_name,
				gintsts.b.curmode,
				gintsts.b.modemismatch,
				gintsts.b.otgintr,
				gintsts.b.sofintr,
				gintsts.b.rxstsqlvl,
				gintsts.b.nptxfempty,
				gintsts.b.ginnakeff,
				gintsts.b.goutnakeff,
				gintsts.b.ulpickint,
				gintsts.b.i2cintr,
				gintsts.b.erlysuspend,
				gintsts.b.usbsuspend,
				gintsts.b.usbreset,
				gintsts.b.enumdone,
				gintsts.b.isooutdrop,
				gintsts.b.eopframe,
				gintsts.b.restoredone,
				gintsts.b.epmismatch,
				gintsts.b.inepint,
				gintsts.b.outepintr,
				gintsts.b.incomplisoin,
				gintsts.b.incomplisoout,
				gintsts.b.fetsusp,
				gintsts.b.resetdet,
				gintsts.b.portintr,
				gintsts.b.hcintr,
				gintsts.b.ptxfempty,
				gintsts.b.lpmtranrcvd,
				gintsts.b.conidstschng,
				gintsts.b.disconnect,
				gintsts.b.sessreqintr,
				gintsts.b.wkupintr);
	return;
}

void dwc_debug_core_int_mask(gintmsk_data_t gintmsk, const char* function_name)
{
	DWC_DEBUGPL(DBG_USER,	"Interrupt Mask status (called from %s) :\n"
				"modemismatch: %1i     otgintr:    %1i    sofintr:    %1i    rxstsqlvl:   %1i\n"
				"nptxfempty:   %1i     ginnakeff:  %1i    goutnakeff: %1i    ulpickint:   %1i\n"
				"i2cintr:      %1i     erlysuspend:%1i    usbsuspend: %1i    usbreset:    %1i\n"
				"enumdone:     %1i     isooutdrop: %1i    eopframe:   %1i    restoredone: %1i\n"
				"epmismatch:   %1i     inepintr:   %1i    outepintr:  %1i    incomplisoin:%1i\n"
				"incomplisoout:%1i     fetsusp:    %1i    resetdet:   %1i    portintr:    %1i\n"
				"hcintr:       %1i     ptxfempty:  %1i    lpmtranrcvd:%1i    conidstschng:%1i\n"
				"disconnect:   %1i     sessreqintr:%1i    wkupintr:   %1i\n",
				function_name,
				gintmsk.b.modemismatch,
				gintmsk.b.otgintr,
				gintmsk.b.sofintr,
				gintmsk.b.rxstsqlvl,
				gintmsk.b.nptxfempty,
				gintmsk.b.ginnakeff,
				gintmsk.b.goutnakeff,
				gintmsk.b.ulpickint,
				gintmsk.b.i2cintr,
				gintmsk.b.erlysuspend,
				gintmsk.b.usbsuspend,
				gintmsk.b.usbreset,
				gintmsk.b.enumdone,
				gintmsk.b.isooutdrop,
				gintmsk.b.eopframe,
				gintmsk.b.restoredone,
				gintmsk.b.epmismatch,
				gintmsk.b.inepintr,
				gintmsk.b.outepintr,
				gintmsk.b.incomplisoin,
				gintmsk.b.incomplisoout,
				gintmsk.b.fetsusp,
				gintmsk.b.resetdet,
				gintmsk.b.portintr,
				gintmsk.b.hcintr,
				gintmsk.b.ptxfempty,
				gintmsk.b.lpmtranrcvd,
				gintmsk.b.conidstschng,
				gintmsk.b.disconnect,
				gintmsk.b.sessreqintr,
				gintmsk.b.wkupintr);
	return;
}

void dwc_debug_otg_int(gotgint_data_t gotgint, const char* function_name)
{
	DWC_DEBUGPL(DBG_USER,	"otg int register (from %s function):\n"
				"sesenddet:%1i    sesreqsucstschung:%2i    hstnegsucstschng:%1i\n"
				"hstnegdet:%1i    adevtoutchng:     %2i    debdone:         %1i\n"
				"mvic:     %1i\n",
				function_name,
				gotgint.b.sesenddet,
				gotgint.b.sesreqsucstschng,
				gotgint.b.hstnegsucstschng,
				gotgint.b.hstnegdet,
				gotgint.b.adevtoutchng,
				gotgint.b.debdone,
				gotgint.b.mvic);

	return;
}
