// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/netdevice.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/eswitch.h>
#include "mlx5_core.h"
#include "lag.h"
#include "eswitch.h"

bool mlx5_lag_shared_fdb_supported(struct mlx5_lag *ldev)
{
	struct mlx5_core_dev *dev;
	bool ret = false;
	int idx;
	int i;

	idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	if (idx < 0)
		return false;

	mlx5_ldev_for_each(i, 0, ldev) {
		if (i == idx)
			continue;
		dev = mlx5_lag_pf(ldev, i)->dev;
		if (is_mdev_switchdev_mode(dev) &&
		    mlx5_eswitch_vport_match_metadata_enabled(dev->priv.eswitch) &&
		    MLX5_CAP_GEN(dev, lag_native_fdb_selection) &&
		    MLX5_CAP_ESW(dev, root_ft_on_other_esw) &&
		    mlx5_eswitch_get_npeers(dev->priv.eswitch) ==
		    MLX5_CAP_GEN(dev, num_lag_ports) - 1)
			continue;
		return false;
	}

	dev = mlx5_lag_pf(ldev, idx)->dev;
	if (is_mdev_switchdev_mode(dev) &&
	    mlx5_eswitch_vport_match_metadata_enabled(dev->priv.eswitch) &&
	    mlx5_esw_offloads_devcom_is_ready(dev->priv.eswitch) &&
	    MLX5_CAP_ESW(dev, esw_shared_ingress_acl) &&
	    mlx5_eswitch_get_npeers(dev->priv.eswitch) ==
	    MLX5_CAP_GEN(dev, num_lag_ports) - 1)
		ret = true;

	return ret;
}

int mlx5_lag_create_single_fdb(struct mlx5_lag *ldev)
{
	int master_idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_eswitch *master_esw;
	struct mlx5_core_dev *dev0;
	int i, j;
	int err;

	if (master_idx < 0)
		return -EINVAL;

	dev0 = mlx5_lag_pf(ldev, master_idx)->dev;
	master_esw = dev0->priv.eswitch;
	mlx5_ldev_for_each(i, 0, ldev) {
		struct mlx5_eswitch *slave_esw;

		if (i == master_idx)
			continue;

		slave_esw = mlx5_lag_pf(ldev, i)->dev->priv.eswitch;

		err = mlx5_eswitch_offloads_single_fdb_add_one(master_esw,
							       slave_esw,
							       ldev->ports);
		if (err)
			goto err;
	}
	return 0;
err:
	mlx5_ldev_for_each_reverse(j, i, 0, ldev) {
		struct mlx5_eswitch *slave_esw;

		if (j == master_idx)
			continue;
		slave_esw = mlx5_lag_pf(ldev, j)->dev->priv.eswitch;
		mlx5_eswitch_offloads_single_fdb_del_one(master_esw, slave_esw);
	}
	return err;
}

int mlx5_lag_shared_fdb_create(struct mlx5_lag *ldev,
			       struct lag_tracker *tracker,
			       enum mlx5_lag_mode mode)
{
	int idx = mlx5_lag_get_dev_index_by_seq(ldev, MLX5_LAG_P1);
	struct mlx5_core_dev *dev0;
	int err;

	if (idx < 0)
		return -EINVAL;

	dev0 = mlx5_lag_pf(ldev, idx)->dev;

	mlx5_lag_remove_devices(ldev);

	err = mlx5_activate_lag(ldev, tracker, mode, true);
	if (err) {
		mlx5_core_warn(dev0, "Failed to create LAG in shared FDB mode (%d)\n",
			       err);
		goto err_add_devices;
	}

	mlx5_lag_rescan_dev_locked(ldev, dev0, true);
	err = mlx5_lag_reload_ib_reps_from_locked(ldev, 0, false);
	if (err) {
		mlx5_core_err(dev0, "Failed to enable lag\n");
		goto err_rescan_drivers;
	}

	mlx5_lag_set_vports_agg_speed(ldev);
	return 0;

err_rescan_drivers:
	mlx5_lag_rescan_dev_locked(ldev, dev0, false);
	mlx5_deactivate_lag(ldev);
err_add_devices:
	mlx5_lag_add_devices(ldev);
	mlx5_lag_reload_ib_reps_from_locked(ldev, 0, true);
	return err;
}

void mlx5_lag_shared_fdb_destroy(struct mlx5_lag *ldev)
{
	int err;

	mlx5_lag_remove_devices(ldev);

	err = mlx5_deactivate_lag(ldev);
	if (err)
		return;

	mlx5_lag_add_devices(ldev);
	mlx5_lag_reload_ib_reps_from_locked(ldev,
					    MLX5_PRIV_FLAGS_DISABLE_ALL_ADEV,
					    true);
}
