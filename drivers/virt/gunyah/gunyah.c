// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/gunyah.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

static int gunyah_probe(struct platform_device *pdev)
{
	struct gunyah_hypercall_hyp_identify_resp gunyah_api;

	if (!arch_is_gunyah_guest())
		return -ENODEV;

	gunyah_hypercall_hyp_identify(&gunyah_api);

	pr_info("Running under Gunyah hypervisor %llx/v%u\n",
		FIELD_GET(GUNYAH_API_INFO_VARIANT_MASK, gunyah_api.api_info),
		gunyah_api_version(&gunyah_api));

	/* Might move this out to individual drivers if there's ever an API version bump */
	if (gunyah_api_version(&gunyah_api) != GUNYAH_API_V1) {
		pr_info("Unsupported Gunyah version: %u\n",
			gunyah_api_version(&gunyah_api));
		return -ENODEV;
	}

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id gunyah_of_match[] = {
	{ .compatible = "gunyah-hypervisor" },
	{}
};
MODULE_DEVICE_TABLE(of, gunyah_of_match);

/* clang-format off */
static struct platform_driver gunyah_driver = {
	.probe = gunyah_probe,
	.driver = {
		.name = "gunyah",
		.of_match_table = gunyah_of_match,
	}
};
/* clang-format on */
module_platform_driver(gunyah_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Driver");
