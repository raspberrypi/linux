/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * This header provides constants for binding rpi,gpio-fsm.
 */

#ifndef _DT_BINDINGS_GPIO_FSM_H
#define _DT_BINDINGS_GPIO_FSM_H

#define GF_IN       0
#define GF_OUT      1
#define GF_SOFT     2
#define GF_DELAY    3
#define GF_SHUTDOWN 4

#define GF_IO(t, v) (((v) << 16) | ((t) & 0xffff))

#define GF_IP(x)    GF_IO(GF_IN, (x))
#define GF_OP(x)    GF_IO(GF_OUT, (x))
#define GF_SW(x)    GF_IO(GF_SOFT, (x))

#endif
