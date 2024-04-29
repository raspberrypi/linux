// SPDX-License-Identifier: GPL-2.0-only
/* binder_pick_impl.c
 *
 * This file contains the logic for choosing between the C and Rust
 * implementations of the Android Binder driver.
 *
 * Copyright (C) 2024 Google LLC.
 */

#include <linux/moduleparam.h>
#include <linux/kconfig.h>
#include <linux/string.h>

#ifdef MODULE_PARAM_PREFIX
#undef MODULE_PARAM_PREFIX
#endif
#define MODULE_PARAM_PREFIX "binder."

#ifndef CONFIG_ANDROID_BINDER_IPC_C
#ifndef CONFIG_ANDROID_BINDER_IPC_RUST
#error "When enabling CONFIG_ANDROID_BINDER_IPC, you must enable at least one of CONFIG_ANDROID_BINDER_IPC_C and CONFIG_ANDROID_BINDER_IPC_RUST"
#endif
#endif

#ifndef CONFIG_ANDROID_BINDER_IPC_RUST
#ifdef CONFIG_ANDROID_BINDER_IPC_DEFAULT_IS_RUST
#error "The default Binder driver implementation is Rust, but the Rust implementation is disabled"
#endif
#endif

#ifndef CONFIG_ANDROID_BINDER_IPC_C
#ifndef CONFIG_ANDROID_BINDER_IPC_DEFAULT_IS_RUST
#error "The default Binder driver implementation is C, but the C implementation is disabled"
#endif
#endif

bool binder_use_rust = IS_ENABLED(CONFIG_ANDROID_BINDER_IPC_DEFAULT_IS_RUST);
bool binder_driver_initialized;

static int binder_param_set(const char *buffer, const struct kernel_param *kp)
{
	if (binder_driver_initialized)
		return -EOPNOTSUPP;

	if (!strcmp(buffer, "rust"))
		binder_use_rust = true;
	else if (!strcmp(buffer, "c"))
		binder_use_rust = false;
	else
		return -EINVAL;

	if (!binder_use_rust && !IS_ENABLED(CONFIG_ANDROID_BINDER_IPC_C)) {
		binder_use_rust = true;
		return -EINVAL;
	}
	if (binder_use_rust && !IS_ENABLED(CONFIG_ANDROID_BINDER_IPC_RUST)) {
		binder_use_rust = false;
		return -EINVAL;
	}

	return 0;
}

static int binder_param_get(char *buffer, const struct kernel_param *kp)
{
	// The buffer is 4k bytes, so this will not overflow.
	if (binder_use_rust)
		strscpy(buffer, "rust\n", 4096);
	else
		strscpy(buffer, "c\n", 4096);
	return strlen(buffer);
}

static const struct kernel_param_ops binder_param_ops = {
	.set = binder_param_set,
	.get = binder_param_get,
};

module_param_cb(impl, &binder_param_ops, NULL, 0444);
