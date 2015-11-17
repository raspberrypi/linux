/*
 *  linux/arch/arm/mach-bcm2708/arm_control.h
 *
 *  Copyright (C) 2010 Broadcom
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

#ifndef __BCM2708_ARM_CONTROL_H
#define __BCM2708_ARM_CONTROL_H

/*
 * Definitions and addresses for the ARM CONTROL logic
 * This file is manually generated.
 */

#define ARM_BASE  0x7E00B000

/* Basic configuration */
#define ARM_CONTROL0  HW_REGISTER_RW(ARM_BASE+0x000)
#define ARM_C0_SIZ128M   0x00000000
#define ARM_C0_SIZ256M   0x00000001
#define ARM_C0_SIZ512M   0x00000002
#define ARM_C0_SIZ1G     0x00000003
#define ARM_C0_BRESP0    0x00000000
#define ARM_C0_BRESP1    0x00000004
#define ARM_C0_BRESP2    0x00000008
#define ARM_C0_BOOTHI    0x00000010
#define ARM_C0_UNUSED05  0x00000020 /* free */
#define ARM_C0_FULLPERI  0x00000040
#define ARM_C0_UNUSED78  0x00000180 /* free */
#define ARM_C0_JTAGMASK  0x00000E00
#define ARM_C0_JTAGOFF   0x00000000
#define ARM_C0_JTAGBASH  0x00000800 /* Debug on GPIO off */
#define ARM_C0_JTAGGPIO  0x00000C00 /* Debug on GPIO on */
#define ARM_C0_APROTMSK  0x0000F000
#define ARM_C0_DBG0SYNC  0x00010000 /* VPU0 halt sync */
#define ARM_C0_DBG1SYNC  0x00020000 /* VPU1 halt sync */
#define ARM_C0_SWDBGREQ  0x00040000 /* HW debug request */
#define ARM_C0_PASSHALT  0x00080000 /* ARM halt passed to debugger */
#define ARM_C0_PRIO_PER  0x00F00000 /* per priority mask */
#define ARM_C0_PRIO_L2   0x0F000000
#define ARM_C0_PRIO_UC   0xF0000000

#define ARM_C0_APROTPASS  0x0000A000 /* Translate 1:1 */
#define ARM_C0_APROTUSER  0x00000000 /* Only user mode */
#define ARM_C0_APROTSYST  0x0000F000 /* Only system mode */


#define ARM_CONTROL1  HW_REGISTER_RW(ARM_BASE+0x440)
#define ARM_C1_TIMER     0x00000001 /* re-route timer IRQ  to VC */
#define ARM_C1_MAIL      0x00000002 /* re-route Mail IRQ   to VC */
#define ARM_C1_BELL0     0x00000004 /* re-route Doorbell 0 to VC */
#define ARM_C1_BELL1     0x00000008 /* re-route Doorbell 1 to VC */
#define ARM_C1_PERSON    0x00000100 /* peripherals on */
#define ARM_C1_REQSTOP   0x00000200 /* ASYNC bridge request stop */

#define ARM_STATUS    HW_REGISTER_RW(ARM_BASE+0x444)
#define ARM_S_ACKSTOP    0x80000000 /* Bridge stopped */
#define ARM_S_READPEND   0x000003FF /* pending reads counter */
#define ARM_S_WRITPEND   0x000FFC00 /* pending writes counter */

#define ARM_ERRHALT   HW_REGISTER_RW(ARM_BASE+0x448)
#define ARM_EH_PERIBURST  0x00000001 /* Burst write seen on peri bus */
#define ARM_EH_ILLADDRS1  0x00000002 /* Address bits 25-27 error */
#define ARM_EH_ILLADDRS2  0x00000004 /* Address bits 31-28 error */
#define ARM_EH_VPU0HALT   0x00000008 /* VPU0 halted & in debug mode */
#define ARM_EH_VPU1HALT   0x00000010 /* VPU1 halted & in debug mode */
#define ARM_EH_ARMHALT    0x00000020 /* ARM in halted debug mode */

#define ARM_ID_SECURE HW_REGISTER_RW(ARM_BASE+0x00C)
#define ARM_ID        HW_REGISTER_RW(ARM_BASE+0x44C)
#define ARM_IDVAL        0x364D5241

/* Translation memory */
#define ARM_TRANSLATE HW_REGISTER_RW(ARM_BASE+0x100)
/* 32 locations: 0x100.. 0x17F */
/* 32 spare means we CAN go to 64 pages.... */


/* Interrupts */
#define ARM_IRQ_PEND0 HW_REGISTER_RW(ARM_BASE+0x200)        /* Top IRQ bits */
#define ARM_I0_TIMER    0x00000001 /* timer IRQ */
#define ARM_I0_MAIL     0x00000002 /* Mail IRQ */
#define ARM_I0_BELL0    0x00000004 /* Doorbell 0 */
#define ARM_I0_BELL1    0x00000008 /* Doorbell 1 */
#define ARM_I0_BANK1    0x00000100 /* Bank1 IRQ */
#define ARM_I0_BANK2    0x00000200 /* Bank2 IRQ */

#define ARM_IRQ_PEND1 HW_REGISTER_RW(ARM_BASE+0x204) /* All bank1 IRQ bits */
/* todo: all I1_interrupt sources */
#define ARM_IRQ_PEND2 HW_REGISTER_RW(ARM_BASE+0x208) /* All bank2 IRQ bits */
/* todo: all I2_interrupt sources */

#define ARM_IRQ_FAST  HW_REGISTER_RW(ARM_BASE+0x20C) /* FIQ control */
#define ARM_IF_INDEX    0x0000007F     /* FIQ select */
#define ARM_IF_ENABLE   0x00000080     /* FIQ enable */
#define ARM_IF_VCMASK   0x0000003F     /* FIQ = (index from VC source) */
#define ARM_IF_TIMER    0x00000040     /* FIQ = ARM timer */
#define ARM_IF_MAIL     0x00000041     /* FIQ = ARM Mail */
#define ARM_IF_BELL0    0x00000042     /* FIQ = ARM Doorbell 0 */
#define ARM_IF_BELL1    0x00000043     /* FIQ = ARM Doorbell 1 */
#define ARM_IF_VP0HALT  0x00000044     /* FIQ = VPU0 Halt seen */
#define ARM_IF_VP1HALT  0x00000045     /* FIQ = VPU1 Halt seen */
#define ARM_IF_ILLEGAL  0x00000046     /* FIQ = Illegal access seen */

#define ARM_IRQ_ENBL1 HW_REGISTER_RW(ARM_BASE+0x210) /* Bank1 enable bits */
#define ARM_IRQ_ENBL2 HW_REGISTER_RW(ARM_BASE+0x214) /* Bank2 enable bits */
#define ARM_IRQ_ENBL3 HW_REGISTER_RW(ARM_BASE+0x218) /* ARM irqs enable bits */
#define ARM_IRQ_DIBL1 HW_REGISTER_RW(ARM_BASE+0x21C) /* Bank1 disable bits */
#define ARM_IRQ_DIBL2 HW_REGISTER_RW(ARM_BASE+0x220) /* Bank2 disable bits */
#define ARM_IRQ_DIBL3 HW_REGISTER_RW(ARM_BASE+0x224) /* ARM irqs disable bits */
#define ARM_IE_TIMER    0x00000001     /* Timer IRQ */
#define ARM_IE_MAIL     0x00000002     /* Mail IRQ */
#define ARM_IE_BELL0    0x00000004     /* Doorbell 0 */
#define ARM_IE_BELL1    0x00000008     /* Doorbell 1 */
#define ARM_IE_VP0HALT  0x00000010     /* VPU0 Halt */
#define ARM_IE_VP1HALT  0x00000020     /* VPU1 Halt */
#define ARM_IE_ILLEGAL  0x00000040     /* Illegal access seen */

/* Timer */
/* For reg. fields see sp804 spec. */
#define ARM_T_LOAD    HW_REGISTER_RW(ARM_BASE+0x400)
#define ARM_T_VALUE   HW_REGISTER_RW(ARM_BASE+0x404)
#define ARM_T_CONTROL HW_REGISTER_RW(ARM_BASE+0x408)
#define ARM_T_IRQCNTL HW_REGISTER_RW(ARM_BASE+0x40C)
#define ARM_T_RAWIRQ  HW_REGISTER_RW(ARM_BASE+0x410)
#define ARM_T_MSKIRQ  HW_REGISTER_RW(ARM_BASE+0x414)
#define ARM_T_RELOAD  HW_REGISTER_RW(ARM_BASE+0x418)
#define ARM_T_PREDIV  HW_REGISTER_RW(ARM_BASE+0x41c)
#define ARM_T_FREECNT HW_REGISTER_RW(ARM_BASE+0x420)

#define TIMER_CTRL_ONESHOT  (1 << 0)
#define TIMER_CTRL_32BIT    (1 << 1)
#define TIMER_CTRL_DIV1     (0 << 2)
#define TIMER_CTRL_DIV16    (1 << 2)
#define TIMER_CTRL_DIV256   (2 << 2)
#define TIMER_CTRL_IE       (1 << 5)
#define TIMER_CTRL_PERIODIC (1 << 6)
#define TIMER_CTRL_ENABLE   (1 << 7)
#define TIMER_CTRL_DBGHALT  (1 << 8)
#define TIMER_CTRL_ENAFREE  (1 << 9)
#define TIMER_CTRL_FREEDIV_SHIFT 16)
#define TIMER_CTRL_FREEDIV_MASK  0xff

/* Semaphores, Doorbells, Mailboxes */
#define ARM_SBM_OWN0  (ARM_BASE+0x800)
#define ARM_SBM_OWN1  (ARM_BASE+0x900)
#define ARM_SBM_OWN2  (ARM_BASE+0xA00)
#define ARM_SBM_OWN3  (ARM_BASE+0xB00)

/* MAILBOXES
 * Register flags are common across all
 * owner registers. See end of this section
 *
 * Semaphores, Doorbells, Mailboxes Owner 0
 *
 */

#define ARM_0_SEMS       HW_REGISTER_RW(ARM_SBM_OWN0+0x00)
#define ARM_0_SEM0       HW_REGISTER_RW(ARM_SBM_OWN0+0x00)
#define ARM_0_SEM1       HW_REGISTER_RW(ARM_SBM_OWN0+0x04)
#define ARM_0_SEM2       HW_REGISTER_RW(ARM_SBM_OWN0+0x08)
#define ARM_0_SEM3       HW_REGISTER_RW(ARM_SBM_OWN0+0x0C)
#define ARM_0_SEM4       HW_REGISTER_RW(ARM_SBM_OWN0+0x10)
#define ARM_0_SEM5       HW_REGISTER_RW(ARM_SBM_OWN0+0x14)
#define ARM_0_SEM6       HW_REGISTER_RW(ARM_SBM_OWN0+0x18)
#define ARM_0_SEM7       HW_REGISTER_RW(ARM_SBM_OWN0+0x1C)
#define ARM_0_BELL0      HW_REGISTER_RW(ARM_SBM_OWN0+0x40)
#define ARM_0_BELL1      HW_REGISTER_RW(ARM_SBM_OWN0+0x44)
#define ARM_0_BELL2      HW_REGISTER_RW(ARM_SBM_OWN0+0x48)
#define ARM_0_BELL3      HW_REGISTER_RW(ARM_SBM_OWN0+0x4C)
/* MAILBOX 0 access in Owner 0 area */
/* Some addresses should ONLY be used by owner 0 */
#define ARM_0_MAIL0_WRT  HW_REGISTER_RW(ARM_SBM_OWN0+0x80)  /* .. 0x8C (4 locations) */
#define ARM_0_MAIL0_RD   HW_REGISTER_RW(ARM_SBM_OWN0+0x80)  /* .. 0x8C (4 locations) Normal read */
#define ARM_0_MAIL0_POL  HW_REGISTER_RW(ARM_SBM_OWN0+0x90)  /* none-pop read */
#define ARM_0_MAIL0_SND  HW_REGISTER_RW(ARM_SBM_OWN0+0x94)  /* Sender read (only LS 2 bits) */
#define ARM_0_MAIL0_STA  HW_REGISTER_RW(ARM_SBM_OWN0+0x98)  /* Status read */
#define ARM_0_MAIL0_CNF  HW_REGISTER_RW(ARM_SBM_OWN0+0x9C)  /* Config read/write */
/* MAILBOX 1 access in Owner 0 area */
/* Owner 0 should only WRITE to this mailbox */
#define ARM_0_MAIL1_WRT  HW_REGISTER_RW(ARM_SBM_OWN0+0xA0)   /* .. 0xAC (4 locations) */
/*#define ARM_0_MAIL1_RD   HW_REGISTER_RW(ARM_SBM_OWN0+0xA0) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_0_MAIL1_POL  HW_REGISTER_RW(ARM_SBM_OWN0+0xB0) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_0_MAIL1_SND  HW_REGISTER_RW(ARM_SBM_OWN0+0xB4) */ /* DO NOT USE THIS !!!!! */
#define ARM_0_MAIL1_STA  HW_REGISTER_RW(ARM_SBM_OWN0+0xB8)   /* Status read */
/*#define ARM_0_MAIL1_CNF  HW_REGISTER_RW(ARM_SBM_OWN0+0xBC) */ /* DO NOT USE THIS !!!!! */
/* General SEM, BELL, MAIL config/status */
#define ARM_0_SEMCLRDBG   HW_REGISTER_RW(ARM_SBM_OWN0+0xE0)  /* semaphore clear/debug register */
#define ARM_0_BELLCLRDBG  HW_REGISTER_RW(ARM_SBM_OWN0+0xE4)  /* Doorbells clear/debug register */
#define ARM_0_ALL_IRQS    HW_REGISTER_RW(ARM_SBM_OWN0+0xF8)  /* ALL interrupts */
#define ARM_0_MY_IRQS     HW_REGISTER_RW(ARM_SBM_OWN0+0xFC)  /* IRQS pending for owner 0 */

/* Semaphores, Doorbells, Mailboxes Owner 1 */
#define ARM_1_SEMS       HW_REGISTER_RW(ARM_SBM_OWN1+0x00)
#define ARM_1_SEM0       HW_REGISTER_RW(ARM_SBM_OWN1+0x00)
#define ARM_1_SEM1       HW_REGISTER_RW(ARM_SBM_OWN1+0x04)
#define ARM_1_SEM2       HW_REGISTER_RW(ARM_SBM_OWN1+0x08)
#define ARM_1_SEM3       HW_REGISTER_RW(ARM_SBM_OWN1+0x0C)
#define ARM_1_SEM4       HW_REGISTER_RW(ARM_SBM_OWN1+0x10)
#define ARM_1_SEM5       HW_REGISTER_RW(ARM_SBM_OWN1+0x14)
#define ARM_1_SEM6       HW_REGISTER_RW(ARM_SBM_OWN1+0x18)
#define ARM_1_SEM7       HW_REGISTER_RW(ARM_SBM_OWN1+0x1C)
#define ARM_1_BELL0      HW_REGISTER_RW(ARM_SBM_OWN1+0x40)
#define ARM_1_BELL1      HW_REGISTER_RW(ARM_SBM_OWN1+0x44)
#define ARM_1_BELL2      HW_REGISTER_RW(ARM_SBM_OWN1+0x48)
#define ARM_1_BELL3      HW_REGISTER_RW(ARM_SBM_OWN1+0x4C)
/* MAILBOX 0 access in Owner 0 area */
/* Owner 1 should only WRITE to this mailbox */
#define ARM_1_MAIL0_WRT  HW_REGISTER_RW(ARM_SBM_OWN1+0x80)  /* .. 0x8C (4 locations) */
/*#define ARM_1_MAIL0_RD  HW_REGISTER_RW(ARM_SBM_OWN1+0x80) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_1_MAIL0_POL HW_REGISTER_RW(ARM_SBM_OWN1+0x90) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_1_MAIL0_SND HW_REGISTER_RW(ARM_SBM_OWN1+0x94) */ /* DO NOT USE THIS !!!!! */
#define ARM_1_MAIL0_STA  HW_REGISTER_RW(ARM_SBM_OWN1+0x98)  /* Status read */
/*#define ARM_1_MAIL0_CNF HW_REGISTER_RW(ARM_SBM_OWN1+0x9C) */ /* DO NOT USE THIS !!!!! */
/* MAILBOX 1 access in Owner 0 area */
#define ARM_1_MAIL1_WRT  HW_REGISTER_RW(ARM_SBM_OWN1+0xA0)  /* .. 0xAC (4 locations) */
#define ARM_1_MAIL1_RD   HW_REGISTER_RW(ARM_SBM_OWN1+0xA0)  /* .. 0xAC (4 locations) Normal read */
#define ARM_1_MAIL1_POL  HW_REGISTER_RW(ARM_SBM_OWN1+0xB0)  /* none-pop read */
#define ARM_1_MAIL1_SND  HW_REGISTER_RW(ARM_SBM_OWN1+0xB4)  /* Sender read (only LS 2 bits) */
#define ARM_1_MAIL1_STA  HW_REGISTER_RW(ARM_SBM_OWN1+0xB8)  /* Status read */
#define ARM_1_MAIL1_CNF  HW_REGISTER_RW(ARM_SBM_OWN1+0xBC)
/* General SEM, BELL, MAIL config/status */
#define ARM_1_SEMCLRDBG   HW_REGISTER_RW(ARM_SBM_OWN1+0xE0)  /* semaphore clear/debug register */
#define ARM_1_BELLCLRDBG  HW_REGISTER_RW(ARM_SBM_OWN1+0xE4)  /* Doorbells clear/debug register */
#define ARM_1_MY_IRQS     HW_REGISTER_RW(ARM_SBM_OWN1+0xFC)  /* IRQS pending for owner 1 */
#define ARM_1_ALL_IRQS    HW_REGISTER_RW(ARM_SBM_OWN1+0xF8)  /* ALL interrupts */

/* Semaphores, Doorbells, Mailboxes Owner 2 */
#define ARM_2_SEMS       HW_REGISTER_RW(ARM_SBM_OWN2+0x00)
#define ARM_2_SEM0       HW_REGISTER_RW(ARM_SBM_OWN2+0x00)
#define ARM_2_SEM1       HW_REGISTER_RW(ARM_SBM_OWN2+0x04)
#define ARM_2_SEM2       HW_REGISTER_RW(ARM_SBM_OWN2+0x08)
#define ARM_2_SEM3       HW_REGISTER_RW(ARM_SBM_OWN2+0x0C)
#define ARM_2_SEM4       HW_REGISTER_RW(ARM_SBM_OWN2+0x10)
#define ARM_2_SEM5       HW_REGISTER_RW(ARM_SBM_OWN2+0x14)
#define ARM_2_SEM6       HW_REGISTER_RW(ARM_SBM_OWN2+0x18)
#define ARM_2_SEM7       HW_REGISTER_RW(ARM_SBM_OWN2+0x1C)
#define ARM_2_BELL0      HW_REGISTER_RW(ARM_SBM_OWN2+0x40)
#define ARM_2_BELL1      HW_REGISTER_RW(ARM_SBM_OWN2+0x44)
#define ARM_2_BELL2      HW_REGISTER_RW(ARM_SBM_OWN2+0x48)
#define ARM_2_BELL3      HW_REGISTER_RW(ARM_SBM_OWN2+0x4C)
/* MAILBOX 0 access in Owner 2 area */
/* Owner 2 should only WRITE to this mailbox */
#define ARM_2_MAIL0_WRT  HW_REGISTER_RW(ARM_SBM_OWN2+0x80)   /* .. 0x8C (4 locations) */
/*#define ARM_2_MAIL0_RD  HW_REGISTER_RW(ARM_SBM_OWN2+0x80)  */ /* DO NOT USE THIS !!!!! */
/*#define ARM_2_MAIL0_POL HW_REGISTER_RW(ARM_SBM_OWN2+0x90)  */ /* DO NOT USE THIS !!!!! */
/*#define ARM_2_MAIL0_SND HW_REGISTER_RW(ARM_SBM_OWN2+0x94)  */ /* DO NOT USE THIS !!!!! */
#define ARM_2_MAIL0_STA  HW_REGISTER_RW(ARM_SBM_OWN2+0x98)   /* Status read */
/*#define ARM_2_MAIL0_CNF HW_REGISTER_RW(ARM_SBM_OWN2+0x9C)  */ /* DO NOT USE THIS !!!!! */
/* MAILBOX 1 access in Owner 2 area */
/* Owner 2 should only WRITE to this mailbox */
#define ARM_2_MAIL1_WRT  HW_REGISTER_RW(ARM_SBM_OWN2+0xA0)   /* .. 0xAC (4 locations) */
/*#define ARM_2_MAIL1_RD   HW_REGISTER_RW(ARM_SBM_OWN2+0xA0) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_2_MAIL1_POL  HW_REGISTER_RW(ARM_SBM_OWN2+0xB0) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_2_MAIL1_SND  HW_REGISTER_RW(ARM_SBM_OWN2+0xB4) */ /* DO NOT USE THIS !!!!! */
#define ARM_2_MAIL1_STA  HW_REGISTER_RW(ARM_SBM_OWN2+0xB8)   /* Status read */
/*#define ARM_2_MAIL1_CNF  HW_REGISTER_RW(ARM_SBM_OWN2+0xBC) */ /* DO NOT USE THIS !!!!! */
/* General SEM, BELL, MAIL config/status */
#define ARM_2_SEMCLRDBG   HW_REGISTER_RW(ARM_SBM_OWN2+0xE0)  /* semaphore clear/debug register */
#define ARM_2_BELLCLRDBG  HW_REGISTER_RW(ARM_SBM_OWN2+0xE4)  /* Doorbells clear/debug register */
#define ARM_2_MY_IRQS     HW_REGISTER_RW(ARM_SBM_OWN2+0xFC)  /* IRQS pending for owner 2 */
#define ARM_2_ALL_IRQS    HW_REGISTER_RW(ARM_SBM_OWN2+0xF8)  /* ALL interrupts */

/* Semaphores, Doorbells, Mailboxes Owner 3 */
#define ARM_3_SEMS       HW_REGISTER_RW(ARM_SBM_OWN3+0x00)
#define ARM_3_SEM0       HW_REGISTER_RW(ARM_SBM_OWN3+0x00)
#define ARM_3_SEM1       HW_REGISTER_RW(ARM_SBM_OWN3+0x04)
#define ARM_3_SEM2       HW_REGISTER_RW(ARM_SBM_OWN3+0x08)
#define ARM_3_SEM3       HW_REGISTER_RW(ARM_SBM_OWN3+0x0C)
#define ARM_3_SEM4       HW_REGISTER_RW(ARM_SBM_OWN3+0x10)
#define ARM_3_SEM5       HW_REGISTER_RW(ARM_SBM_OWN3+0x14)
#define ARM_3_SEM6       HW_REGISTER_RW(ARM_SBM_OWN3+0x18)
#define ARM_3_SEM7       HW_REGISTER_RW(ARM_SBM_OWN3+0x1C)
#define ARM_3_BELL0      HW_REGISTER_RW(ARM_SBM_OWN3+0x40)
#define ARM_3_BELL1      HW_REGISTER_RW(ARM_SBM_OWN3+0x44)
#define ARM_3_BELL2      HW_REGISTER_RW(ARM_SBM_OWN3+0x48)
#define ARM_3_BELL3      HW_REGISTER_RW(ARM_SBM_OWN3+0x4C)
/* MAILBOX 0 access in Owner 3 area */
/* Owner 3 should only WRITE to this mailbox */
#define ARM_3_MAIL0_WRT  HW_REGISTER_RW(ARM_SBM_OWN3+0x80)   /* .. 0x8C (4 locations) */
/*#define ARM_3_MAIL0_RD  HW_REGISTER_RW(ARM_SBM_OWN3+0x80)  */ /* DO NOT USE THIS !!!!! */
/*#define ARM_3_MAIL0_POL HW_REGISTER_RW(ARM_SBM_OWN3+0x90)  */ /* DO NOT USE THIS !!!!! */
/*#define ARM_3_MAIL0_SND HW_REGISTER_RW(ARM_SBM_OWN3+0x94)  */ /* DO NOT USE THIS !!!!! */
#define ARM_3_MAIL0_STA HW_REGISTER_RW(ARM_SBM_OWN3+0x98)    /* Status read */
/*#define ARM_3_MAIL0_CNF HW_REGISTER_RW(ARM_SBM_OWN3+0x9C)  */ /* DO NOT USE THIS !!!!! */
/* MAILBOX 1 access in Owner 3 area */
/* Owner 3 should only WRITE to this mailbox */
#define ARM_3_MAIL1_WRT  HW_REGISTER_RW(ARM_SBM_OWN3+0xA0)   /* .. 0xAC (4 locations) */
/*#define ARM_3_MAIL1_RD   HW_REGISTER_RW(ARM_SBM_OWN3+0xA0) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_3_MAIL1_POL  HW_REGISTER_RW(ARM_SBM_OWN3+0xB0) */ /* DO NOT USE THIS !!!!! */
/*#define ARM_3_MAIL1_SND  HW_REGISTER_RW(ARM_SBM_OWN3+0xB4) */ /* DO NOT USE THIS !!!!! */
#define ARM_3_MAIL1_STA  HW_REGISTER_RW(ARM_SBM_OWN3+0xB8)   /* Status read */
/*#define ARM_3_MAIL1_CNF  HW_REGISTER_RW(ARM_SBM_OWN3+0xBC) */ /* DO NOT USE THIS !!!!! */
/* General SEM, BELL, MAIL config/status */
#define ARM_3_SEMCLRDBG   HW_REGISTER_RW(ARM_SBM_OWN3+0xE0)  /* semaphore clear/debug register */
#define ARM_3_BELLCLRDBG  HW_REGISTER_RW(ARM_SBM_OWN3+0xE4)  /* Doorbells clear/debug register */
#define ARM_3_MY_IRQS     HW_REGISTER_RW(ARM_SBM_OWN3+0xFC)  /* IRQS pending for owner 3 */
#define ARM_3_ALL_IRQS    HW_REGISTER_RW(ARM_SBM_OWN3+0xF8)  /* ALL interrupts */



/*  Mailbox flags. Valid for all owners */

/* Mailbox status register (...0x98) */
#define ARM_MS_FULL       0x80000000
#define ARM_MS_EMPTY      0x40000000
#define ARM_MS_LEVEL      0x400000FF /* Max. value depdnds on mailbox depth parameter */

/* MAILBOX config/status register (...0x9C) */
/* ANY write to this register clears the error bits! */
#define ARM_MC_IHAVEDATAIRQEN    0x00000001 /* mailbox irq enable:  has data */
#define ARM_MC_IHAVESPACEIRQEN   0x00000002 /* mailbox irq enable:  has space */
#define ARM_MC_OPPISEMPTYIRQEN   0x00000004 /* mailbox irq enable: Opp. is empty */
#define ARM_MC_MAIL_CLEAR        0x00000008 /* mailbox clear write 1, then  0 */
#define ARM_MC_IHAVEDATAIRQPEND  0x00000010 /* mailbox irq pending:  has space */
#define ARM_MC_IHAVESPACEIRQPEND 0x00000020 /* mailbox irq pending: Opp. is empty */
#define ARM_MC_OPPISEMPTYIRQPEND 0x00000040 /* mailbox irq pending */
/* Bit 7 is unused */
#define ARM_MC_ERRNOOWN   0x00000100 /* error : none owner read from mailbox */
#define ARM_MC_ERROVERFLW 0x00000200 /* error : write to fill mailbox */
#define ARM_MC_ERRUNDRFLW 0x00000400 /* error : read from empty mailbox */

/* Semaphore clear/debug register (...0xE0) */
#define ARM_SD_OWN0      0x00000003  /* Owner of sem 0 */
#define ARM_SD_OWN1      0x0000000C  /* Owner of sem 1 */
#define ARM_SD_OWN2      0x00000030  /* Owner of sem 2 */
#define ARM_SD_OWN3      0x000000C0  /* Owner of sem 3 */
#define ARM_SD_OWN4      0x00000300  /* Owner of sem 4 */
#define ARM_SD_OWN5      0x00000C00  /* Owner of sem 5 */
#define ARM_SD_OWN6      0x00003000  /* Owner of sem 6 */
#define ARM_SD_OWN7      0x0000C000  /* Owner of sem 7 */
#define ARM_SD_SEM0      0x00010000  /* Status of sem 0 */
#define ARM_SD_SEM1      0x00020000  /* Status of sem 1 */
#define ARM_SD_SEM2      0x00040000  /* Status of sem 2 */
#define ARM_SD_SEM3      0x00080000  /* Status of sem 3 */
#define ARM_SD_SEM4      0x00100000  /* Status of sem 4 */
#define ARM_SD_SEM5      0x00200000  /* Status of sem 5 */
#define ARM_SD_SEM6      0x00400000  /* Status of sem 6 */
#define ARM_SD_SEM7      0x00800000  /* Status of sem 7 */

/* Doorbells clear/debug register (...0xE4) */
#define ARM_BD_OWN0      0x00000003  /* Owner of doorbell 0 */
#define ARM_BD_OWN1      0x0000000C  /* Owner of doorbell 1 */
#define ARM_BD_OWN2      0x00000030  /* Owner of doorbell 2 */
#define ARM_BD_OWN3      0x000000C0  /* Owner of doorbell 3 */
#define ARM_BD_BELL0     0x00000100  /* Status of doorbell 0 */
#define ARM_BD_BELL1     0x00000200  /* Status of doorbell 1 */
#define ARM_BD_BELL2     0x00000400  /* Status of doorbell 2 */
#define ARM_BD_BELL3     0x00000800  /* Status of doorbell 3 */

/* MY IRQS register (...0xF8) */
#define ARM_MYIRQ_BELL   0x00000001  /* This owner has a doorbell IRQ */
#define ARM_MYIRQ_MAIL   0x00000002  /* This owner has a mailbox  IRQ */

/* ALL IRQS register (...0xF8) */
#define ARM_AIS_BELL0 0x00000001  /* Doorbell 0 IRQ pending */
#define ARM_AIS_BELL1 0x00000002  /* Doorbell 1 IRQ pending */
#define ARM_AIS_BELL2 0x00000004  /* Doorbell 2 IRQ pending */
#define ARM_AIS_BELL3 0x00000008  /* Doorbell 3 IRQ pending */
#define ARM_AIS0_HAVEDATA 0x00000010  /* MAIL 0 has data IRQ pending */
#define ARM_AIS0_HAVESPAC 0x00000020  /* MAIL 0 has space IRQ pending */
#define ARM_AIS0_OPPEMPTY 0x00000040  /* MAIL 0 opposite is empty IRQ */
#define ARM_AIS1_HAVEDATA 0x00000080  /* MAIL 1 has data IRQ pending */
#define ARM_AIS1_HAVESPAC 0x00000100  /* MAIL 1 has space IRQ pending */
#define ARM_AIS1_OPPEMPTY 0x00000200  /* MAIL 1 opposite is empty IRQ */
/* Note   that bell-0, bell-1 and MAIL0 IRQ go only to the ARM */
/* Whilst that bell-2, bell-3 and MAIL1 IRQ go only to the VC */
/* */
/* ARM JTAG BASH */
/* */
#define AJB_BASE 0x7e2000c0

#define AJBCONF HW_REGISTER_RW(AJB_BASE+0x00)
#define   AJB_BITS0    0x000000
#define   AJB_BITS4    0x000004
#define   AJB_BITS8    0x000008
#define   AJB_BITS12   0x00000C
#define   AJB_BITS16   0x000010
#define   AJB_BITS20   0x000014
#define   AJB_BITS24   0x000018
#define   AJB_BITS28   0x00001C
#define   AJB_BITS32   0x000020
#define   AJB_BITS34   0x000022
#define   AJB_OUT_MS   0x000040
#define   AJB_OUT_LS   0x000000
#define   AJB_INV_CLK  0x000080
#define   AJB_D0_RISE  0x000100
#define   AJB_D0_FALL  0x000000
#define   AJB_D1_RISE  0x000200
#define   AJB_D1_FALL  0x000000
#define   AJB_IN_RISE  0x000400
#define   AJB_IN_FALL  0x000000
#define   AJB_ENABLE   0x000800
#define   AJB_HOLD0    0x000000
#define   AJB_HOLD1    0x001000
#define   AJB_HOLD2    0x002000
#define   AJB_HOLD3    0x003000
#define   AJB_RESETN   0x004000
#define   AJB_CLKSHFT  16
#define   AJB_BUSY     0x80000000
#define AJBTMS HW_REGISTER_RW(AJB_BASE+0x04)
#define AJBTDI HW_REGISTER_RW(AJB_BASE+0x08)
#define AJBTDO HW_REGISTER_RW(AJB_BASE+0x0c)

#define ARM_LOCAL_BASE 0x40000000
#define ARM_LOCAL_CONTROL		HW_REGISTER_RW(ARM_LOCAL_BASE+0x000)
#define ARM_LOCAL_PRESCALER		HW_REGISTER_RW(ARM_LOCAL_BASE+0x008)
#define ARM_LOCAL_GPU_INT_ROUTING	HW_REGISTER_RW(ARM_LOCAL_BASE+0x00C)
#define ARM_LOCAL_PM_ROUTING_SET	HW_REGISTER_RW(ARM_LOCAL_BASE+0x010)
#define ARM_LOCAL_PM_ROUTING_CLR	HW_REGISTER_RW(ARM_LOCAL_BASE+0x014)
#define ARM_LOCAL_TIMER_LS		HW_REGISTER_RW(ARM_LOCAL_BASE+0x01C)
#define ARM_LOCAL_TIMER_MS		HW_REGISTER_RW(ARM_LOCAL_BASE+0x020)
#define ARM_LOCAL_INT_ROUTING		HW_REGISTER_RW(ARM_LOCAL_BASE+0x024)
#define ARM_LOCAL_AXI_COUNT		HW_REGISTER_RW(ARM_LOCAL_BASE+0x02C)
#define ARM_LOCAL_AXI_IRQ		HW_REGISTER_RW(ARM_LOCAL_BASE+0x030)
#define ARM_LOCAL_TIMER_CONTROL		HW_REGISTER_RW(ARM_LOCAL_BASE+0x034)
#define ARM_LOCAL_TIMER_WRITE		HW_REGISTER_RW(ARM_LOCAL_BASE+0x038)

#define ARM_LOCAL_TIMER_INT_CONTROL0	HW_REGISTER_RW(ARM_LOCAL_BASE+0x040)
#define ARM_LOCAL_TIMER_INT_CONTROL1	HW_REGISTER_RW(ARM_LOCAL_BASE+0x044)
#define ARM_LOCAL_TIMER_INT_CONTROL2	HW_REGISTER_RW(ARM_LOCAL_BASE+0x048)
#define ARM_LOCAL_TIMER_INT_CONTROL3	HW_REGISTER_RW(ARM_LOCAL_BASE+0x04C)

#define ARM_LOCAL_MAILBOX_INT_CONTROL0	HW_REGISTER_RW(ARM_LOCAL_BASE+0x050)
#define ARM_LOCAL_MAILBOX_INT_CONTROL1	HW_REGISTER_RW(ARM_LOCAL_BASE+0x054)
#define ARM_LOCAL_MAILBOX_INT_CONTROL2	HW_REGISTER_RW(ARM_LOCAL_BASE+0x058)
#define ARM_LOCAL_MAILBOX_INT_CONTROL3	HW_REGISTER_RW(ARM_LOCAL_BASE+0x05C)

#define ARM_LOCAL_IRQ_PENDING0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x060)
#define ARM_LOCAL_IRQ_PENDING1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x064)
#define ARM_LOCAL_IRQ_PENDING2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x068)
#define ARM_LOCAL_IRQ_PENDING3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x06C)

#define ARM_LOCAL_FIQ_PENDING0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x070)
#define ARM_LOCAL_FIQ_PENDING1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x074)
#define ARM_LOCAL_FIQ_PENDING2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x078)
#define ARM_LOCAL_FIQ_PENDING3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x07C)

#define ARM_LOCAL_MAILBOX0_SET0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x080)
#define ARM_LOCAL_MAILBOX1_SET0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x084)
#define ARM_LOCAL_MAILBOX2_SET0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x088)
#define ARM_LOCAL_MAILBOX3_SET0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x08C)

#define ARM_LOCAL_MAILBOX0_SET1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x090)
#define ARM_LOCAL_MAILBOX1_SET1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x094)
#define ARM_LOCAL_MAILBOX2_SET1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x098)
#define ARM_LOCAL_MAILBOX3_SET1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x09C)

#define ARM_LOCAL_MAILBOX0_SET2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0A0)
#define ARM_LOCAL_MAILBOX1_SET2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0A4)
#define ARM_LOCAL_MAILBOX2_SET2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0A8)
#define ARM_LOCAL_MAILBOX3_SET2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0AC)

#define ARM_LOCAL_MAILBOX0_SET3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0B0)
#define ARM_LOCAL_MAILBOX1_SET3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0B4)
#define ARM_LOCAL_MAILBOX2_SET3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0B8)
#define ARM_LOCAL_MAILBOX3_SET3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0BC)

#define ARM_LOCAL_MAILBOX0_CLR0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0C0)
#define ARM_LOCAL_MAILBOX1_CLR0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0C4)
#define ARM_LOCAL_MAILBOX2_CLR0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0C8)
#define ARM_LOCAL_MAILBOX3_CLR0		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0CC)

#define ARM_LOCAL_MAILBOX0_CLR1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0D0)
#define ARM_LOCAL_MAILBOX1_CLR1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0D4)
#define ARM_LOCAL_MAILBOX2_CLR1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0D8)
#define ARM_LOCAL_MAILBOX3_CLR1		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0DC)

#define ARM_LOCAL_MAILBOX0_CLR2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0E0)
#define ARM_LOCAL_MAILBOX1_CLR2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0E4)
#define ARM_LOCAL_MAILBOX2_CLR2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0E8)
#define ARM_LOCAL_MAILBOX3_CLR2		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0EC)

#define ARM_LOCAL_MAILBOX0_CLR3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0F0)
#define ARM_LOCAL_MAILBOX1_CLR3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0F4)
#define ARM_LOCAL_MAILBOX2_CLR3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0F8)
#define ARM_LOCAL_MAILBOX3_CLR3		HW_REGISTER_RW(ARM_LOCAL_BASE+0x0FC)

#endif
