/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RUST_BINDER_H
#define _LINUX_RUST_BINDER_H

#include <uapi/linux/android/binderfs.h>

/*
 * This typedef is used for Rust binder driver instances. The driver object is
 * completely opaque from C and can only be accessed via calls into Rust, so we
 * use a typedef.
 */
typedef void *rust_binder_device;

int init_rust_binderfs(void);

#endif
