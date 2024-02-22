// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/gunyah.h>
#include <linux/module.h>
#include <linux/rwsem.h>

#include "rsc_mgr.h"

static const struct gunyah_rm_platform_ops *rm_platform_ops;
static DECLARE_RWSEM(rm_platform_ops_lock);

int gunyah_rm_platform_pre_mem_share(struct gunyah_rm *rm,
				     struct gunyah_rm_mem_parcel *mem_parcel)
{
	int ret = 0;

	down_read(&rm_platform_ops_lock);
	if (rm_platform_ops && rm_platform_ops->pre_mem_share)
		ret = rm_platform_ops->pre_mem_share(rm, mem_parcel);
	up_read(&rm_platform_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_platform_pre_mem_share);

int gunyah_rm_platform_post_mem_reclaim(struct gunyah_rm *rm,
					struct gunyah_rm_mem_parcel *mem_parcel)
{
	int ret = 0;

	down_read(&rm_platform_ops_lock);
	if (rm_platform_ops && rm_platform_ops->post_mem_reclaim)
		ret = rm_platform_ops->post_mem_reclaim(rm, mem_parcel);
	up_read(&rm_platform_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_platform_post_mem_reclaim);

int gunyah_rm_platform_pre_demand_page(struct gunyah_rm *rm, u16 vmid,
				       enum gunyah_pagetable_access access,
				       struct folio *folio)
{
	int ret = 0;

	down_read(&rm_platform_ops_lock);
	if (rm_platform_ops && rm_platform_ops->pre_demand_page)
		ret = rm_platform_ops->pre_demand_page(rm, vmid, access, folio);
	up_read(&rm_platform_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_platform_pre_demand_page);

int gunyah_rm_platform_reclaim_demand_page(struct gunyah_rm *rm, u16 vmid,
					   enum gunyah_pagetable_access access,
					   struct folio *folio)
{
	int ret = 0;

	down_read(&rm_platform_ops_lock);
	if (rm_platform_ops && rm_platform_ops->pre_demand_page)
		ret = rm_platform_ops->release_demand_page(rm, vmid, access,
							   folio);
	up_read(&rm_platform_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_platform_reclaim_demand_page);

int gunyah_rm_register_platform_ops(
	const struct gunyah_rm_platform_ops *platform_ops)
{
	int ret = 0;

	down_write(&rm_platform_ops_lock);
	if (!rm_platform_ops)
		rm_platform_ops = platform_ops;
	else
		ret = -EEXIST;
	up_write(&rm_platform_ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(gunyah_rm_register_platform_ops);

void gunyah_rm_unregister_platform_ops(
	const struct gunyah_rm_platform_ops *platform_ops)
{
	down_write(&rm_platform_ops_lock);
	if (rm_platform_ops == platform_ops)
		rm_platform_ops = NULL;
	up_write(&rm_platform_ops_lock);
}
EXPORT_SYMBOL_GPL(gunyah_rm_unregister_platform_ops);

static void _devm_gunyah_rm_unregister_platform_ops(void *data)
{
	gunyah_rm_unregister_platform_ops(
		(const struct gunyah_rm_platform_ops *)data);
}

int devm_gunyah_rm_register_platform_ops(
	struct device *dev, const struct gunyah_rm_platform_ops *ops)
{
	int ret;

	ret = gunyah_rm_register_platform_ops(ops);
	if (ret)
		return ret;

	return devm_add_action(dev, _devm_gunyah_rm_unregister_platform_ops,
			       (void *)ops);
}
EXPORT_SYMBOL_GPL(devm_gunyah_rm_register_platform_ops);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gunyah Platform Hooks");
