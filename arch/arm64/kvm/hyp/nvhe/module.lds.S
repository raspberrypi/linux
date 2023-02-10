/* SPDX-License-Identifier: GPL-2.0 */

#include <asm/hyp_image.h>
#include <asm/page-def.h>

SECTIONS {
	.hyp.text : {
		HYP_SECTION_SYMBOL_NAME(.text) = .;
		*(.text .text.*)
	}

	.hyp.bss : {
		HYP_SECTION_SYMBOL_NAME(.bss) = .;
		*(.bss .bss.*)
	}

	.hyp.rodata : {
		HYP_SECTION_SYMBOL_NAME(.rodata) = .;
		*(.rodata .rodata.*)
	}

	.hyp.event_ids : {
		HYP_SECTION_SYMBOL_NAME(.event_ids) = .;
		__hyp_event_ids_start = .;
		*(HYP_SECTION_NAME(.event_ids))
		__hyp_event_ids_end = .;
	}

	.hyp.data : {
		HYP_SECTION_SYMBOL_NAME(.data) = .;
		*(.data .data.*)
	}
}
