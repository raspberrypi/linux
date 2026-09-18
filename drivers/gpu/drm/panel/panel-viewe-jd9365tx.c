// SPDX-License-Identifier: GPL-2.0
/*
 * DRM panel driver for VIEWE 7" 720x1280 Jadard JD9365TX (module 7KF82)
 *
 * 2-lane MIPI-DSI video mode. Optional reset-gpios; many FPCs use POR only.
 * Backlight is a separate I2C device on the same FPC.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct jd9365tx_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset;
	struct regulator *vdd;
	struct regulator *iovcc;
	enum drm_panel_orientation orientation;
	bool prepared;
};

struct jd9365tx_cmd {
	u8 len;
	u8 data[64];
	u16 delay_ms;
};

#define JD_CMD(...) {					\
	.len = sizeof((u8[]){ __VA_ARGS__ }),		\
	.data = { __VA_ARGS__ },			\
	.delay_ms = 0,					\
}

#define JD_CMD_DELAY(ms, ...) {				\
	.len = sizeof((u8[]){ __VA_ARGS__ }),		\
	.data = { __VA_ARGS__ },			\
	.delay_ms = (ms),				\
}

/*
 * Init sequence (page select via 0xDE). Page0 0xCC = 0x31 (2-lane).
 */
static const struct jd9365tx_cmd jd9365tx_7kf82_init[] = {
#include "panel-viewe-jd9365tx-init.h"
};

static inline struct jd9365tx_panel *to_jd9365tx(struct drm_panel *panel)
{
	return container_of(panel, struct jd9365tx_panel, panel);
}

static void jd9365tx_run_dsi_diag(struct mipi_dsi_device *dsi, const char *tag);

static int jd9365tx_send_cmds(struct mipi_dsi_device *dsi,
			      const struct jd9365tx_cmd *cmds,
			      unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		const struct jd9365tx_cmd *cmd = &cmds[i];

		if (cmd->len) {
			ret = mipi_dsi_dcs_write_buffer(dsi, cmd->data, cmd->len);
			if (ret < 0) {
				dev_err(&dsi->dev,
					"DCS write 0x%02x failed: %d\n",
					cmd->data[0], ret);
				return ret;
			}
		}

		if (cmd->delay_ms)
			msleep(cmd->delay_ms);
	}

	return 0;
}

static int jd9365tx_panel_reset(struct jd9365tx_panel *ctx)
{
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(20);
		gpiod_set_value_cansleep(ctx->reset, 0);
		msleep(20);
		return 0;
	}

	/* No RESET on many DSI FPCs; rely on power-on reset. */
	msleep(50);
	return 0;
}

static int jd9365tx_init_once(struct jd9365tx_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	/* ensure LP-11 before register programming */
	ret = mipi_dsi_dcs_nop(dsi);
	if (ret)
		dev_warn(&dsi->dev, "DSI NOP failed: %d\n", ret);
	msleep(10);

	ret = jd9365tx_panel_reset(ctx);
	if (ret)
		return ret;

	/* Register programming; Sleep Out / Display On follow in prepare() */
	return jd9365tx_send_cmds(dsi, jd9365tx_7kf82_init,
				  ARRAY_SIZE(jd9365tx_7kf82_init));
}

static int jd9365tx_prepare(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;
	int attempt;

	if (ctx->prepared)
		return 0;

	ret = regulator_enable(ctx->iovcc);
	if (ret) {
		dev_err(&dsi->dev, "failed to enable iovcc: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->vdd);
	if (ret) {
		dev_err(&dsi->dev, "failed to enable vdd: %d\n", ret);
		regulator_disable(ctx->iovcc);
		return ret;
	}

	/* Cold boot: wait for board POR / AVDD/AVEE before any DSI traffic */
	msleep(150);

	/* BTA/read test before writing init tables */
	ret = mipi_dsi_dcs_nop(dsi);
	if (ret)
		dev_warn(&dsi->dev, "pre-diag NOP failed: %d\n", ret);
	msleep(10);
	jd9365tx_run_dsi_diag(dsi, "pre-init");

	for (attempt = 1; attempt <= 3; attempt++) {
		ret = jd9365tx_init_once(ctx);
		if (!ret)
			break;

		dev_warn(&dsi->dev, "panel init attempt %d/3 failed: %d\n",
			 attempt, ret);
		msleep(80);
	}

	if (ret) {
		if (ctx->reset)
			gpiod_set_value_cansleep(ctx->reset, 1);
		regulator_disable(ctx->vdd);
		regulator_disable(ctx->iovcc);
		return ret;
	}

	/* Exit sleep / display on while still in LP command mode */
	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret) {
		dev_err(&dsi->dev, "exit sleep failed: %d\n", ret);
		goto err_power;
	}
	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret) {
		dev_err(&dsi->dev, "set display on failed: %d\n", ret);
		goto err_power;
	}
	msleep(20);

	/* After Sleep Out / Display On — panel should answer power-mode etc. */
	jd9365tx_run_dsi_diag(dsi, "post-on");

	ctx->prepared = true;
	return 0;

err_power:
	if (ctx->reset)
		gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->vdd);
	regulator_disable(ctx->iovcc);
	return ret;
}

static int jd9365tx_enable(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int ret;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret)
		dev_warn(&dsi->dev, "display on: %d\n", ret);
	msleep(20);
	return 0;
}

/* power-down */

static uint dispoff_delay_ms = 80;
module_param(dispoff_delay_ms, uint, 0644);
MODULE_PARM_DESC(dispoff_delay_ms,
		 "Delay after DCS 0x28 (DISPOFF) during disable/shutdown");

static uint slpin_delay_ms = 200;
module_param(slpin_delay_ms, uint, 0644);
MODULE_PARM_DESC(slpin_delay_ms,
		 "Delay after DCS 0x10 (SLPIN) during unprepare/shutdown");

static uint sleep_cmd_retries = 3;
module_param(sleep_cmd_retries, uint, 0644);
MODULE_PARM_DESC(sleep_cmd_retries,
		 "Retries for DISPOFF/SLPIN in shutdown path");

static uint sleep_cmd_retry_delay_ms = 20;
module_param(sleep_cmd_retry_delay_ms, uint, 0644);
MODULE_PARM_DESC(sleep_cmd_retry_delay_ms,
		 "Delay between DISPOFF/SLPIN retries");

static struct mipi_dsi_device *g_reboot_dsi;
static struct notifier_block jd9365tx_reboot_nb;

static int jd9365tx_send_display_off_retry(struct mipi_dsi_device *dsi)
{
	u32 i, tries = sleep_cmd_retries ? sleep_cmd_retries : 1;
	int ret = -EINVAL;

	for (i = 0; i < tries; i++) {
		ret = mipi_dsi_dcs_set_display_off(dsi);
		if (ret >= 0)
			return ret;
		if (i + 1 < tries)
			msleep(sleep_cmd_retry_delay_ms);
	}
	return ret;
}

static int jd9365tx_send_sleep_in_retry(struct mipi_dsi_device *dsi)
{
	u32 i, tries = sleep_cmd_retries ? sleep_cmd_retries : 1;
	int ret = -EINVAL;

	for (i = 0; i < tries; i++) {
		ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
		if (ret >= 0)
			return ret;
		if (i + 1 < tries)
			msleep(sleep_cmd_retry_delay_ms);
	}
	return ret;
}

static void jd9365tx_force_sleep(struct mipi_dsi_device *dsi)
{
	struct jd9365tx_panel *ctx;

	if (!dsi)
		return;
	ctx = mipi_dsi_get_drvdata(dsi);
	if (!ctx)
		return;

	if (jd9365tx_send_display_off_retry(dsi) < 0)
		dev_warn(&dsi->dev, "force sleep: display off failed\n");
	msleep(dispoff_delay_ms);

	if (jd9365tx_send_sleep_in_retry(dsi) < 0)
		dev_warn(&dsi->dev, "force sleep: enter sleep failed\n");
	msleep(slpin_delay_ms);

	/* Optional reset: Pi DSI board usually has none */
	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(60);
	}
}

static int jd9365tx_reboot_notifier(struct notifier_block *nb,
				    unsigned long mode, void *ptr)
{
	struct mipi_dsi_device *dsi = g_reboot_dsi;

	if (!dsi)
		return NOTIFY_DONE;

	if (mode != SYS_RESTART && mode != SYS_HALT && mode != SYS_POWER_OFF)
		return NOTIFY_DONE;

	dev_info(&dsi->dev,
		 "reboot notifier: display off + sleep (mode=%lu)\n", mode);
	jd9365tx_force_sleep(dsi);
	return NOTIFY_DONE;
}

/* disable: display off only */
static int jd9365tx_disable(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	int ret;

	ret = jd9365tx_send_display_off_retry(ctx->dsi);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "display off failed: %d\n", ret);
	msleep(dispoff_delay_ms);
	return 0;
}

/* unprepare: SLPIN after disable; then drop rails */
static int jd9365tx_unprepare(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;

	if (!ctx->prepared)
		return 0;

	if (jd9365tx_send_sleep_in_retry(dsi) < 0)
		dev_warn(&dsi->dev, "enter sleep mode failed\n");
	msleep(slpin_delay_ms);

	if (ctx->reset) {
		gpiod_set_value_cansleep(ctx->reset, 1);
		msleep(20);
	}

	regulator_disable(ctx->vdd);
	msleep(10);
	regulator_disable(ctx->iovcc);
	msleep(20);

	ctx->prepared = false;
	dev_info(&dsi->dev, "panel unprepared (SLPIN + rails off)\n");
	return 0;
}

/* Vendor porch; default 50 Hz, optional 60 Hz via use_60hz */
static const struct drm_display_mode jd9365tx_7kf82_mode_60 = {
	.clock		= 72480,
	.hdisplay	= 720,
	.hsync_start	= 720 + 20,
	.hsync_end	= 720 + 20 + 20,
	.htotal		= 720 + 20 + 20 + 40,
	.vdisplay	= 1280,
	.vsync_start	= 1280 + 200,
	.vsync_end	= 1280 + 200 + 2,
	.vtotal		= 1280 + 200 + 2 + 28,
	.width_mm	= 87,
	.height_mm	= 155,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

/* 800 * 1510 * 50 / 1000 = 60400 */
static const struct drm_display_mode jd9365tx_7kf82_mode_50 = {
	.clock		= 60400,
	.hdisplay	= 720,
	.hsync_start	= 720 + 20,
	.hsync_end	= 720 + 20 + 20,
	.htotal		= 720 + 20 + 20 + 40,
	.vdisplay	= 1280,
	.vsync_start	= 1280 + 200,
	.vsync_end	= 1280 + 200 + 2,
	.vtotal		= 1280 + 200 + 2 + 28,
	.width_mm	= 87,
	.height_mm	= 155,
	.type		= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static bool use_60hz;
module_param(use_60hz, bool, 0644);
MODULE_PARM_DESC(use_60hz, "Use vendor 60Hz 72.48MHz; default 0 = stable 50Hz on Pi4 2-lane");

static bool non_cont_clock = true;
module_param(non_cont_clock, bool, 0644);
MODULE_PARM_DESC(non_cont_clock,
		 "Set MIPI_DSI_CLOCK_NON_CONTINUOUS (default true for vc4)");

/* Optional DSI read check (needs BTA on lane 0) */
static bool dsi_diag;
module_param(dsi_diag, bool, 0644);
MODULE_PARM_DESC(dsi_diag,
		 "Run DSI DCS read ID/status in prepare (default off; enable for wiring debug)");

static int jd9365tx_dcs_read_log(struct mipi_dsi_device *dsi, u8 cmd,
				 void *data, size_t len, const char *name)
{
	int ret;

	memset(data, 0, len);
	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_warn(&dsi->dev,
			 "DSI READ 0x%02x (%s) FAILED: %d%s\n",
			 cmd, name, ret,
			 ret == -ETIMEDOUT || ret == -110 ?
			 " (BTA/turnaround — check D0/CLK)" : "");
		return ret;
	}

	if (len == 1) {
		dev_info(&dsi->dev, "DSI READ 0x%02x (%s) = 0x%02x (ok)\n",
			 cmd, name, *(u8 *)data);
	} else if (len == 3) {
		u8 *p = data;

		dev_info(&dsi->dev,
			 "DSI READ 0x%02x (%s) = %02x %02x %02x (ok)\n",
			 cmd, name, p[0], p[1], p[2]);
	} else {
		dev_info(&dsi->dev, "DSI READ 0x%02x (%s) ok, %zu bytes\n",
			 cmd, name, len);
	}
	return 0;
}

static void jd9365tx_run_dsi_diag(struct mipi_dsi_device *dsi, const char *tag)
{
	u8 b = 0;
	u8 id[3] = { 0 };
	int ok = 0, fail = 0;
	unsigned long saved_flags;

	if (!dsi_diag)
		return;

	dev_info(&dsi->dev, "=== DSI BTA diag [%s] begin ===\n", tag);

	/* Force LP for command/BTA window */
	saved_flags = dsi->mode_flags;
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	if (mipi_dsi_dcs_nop(dsi)) {
		dev_warn(&dsi->dev, "DSI NOP (write) failed before reads\n");
		fail++;
	} else {
		dev_info(&dsi->dev, "DSI NOP (write) ok\n");
		ok++;
	}
	msleep(5);

	/* Standard DCS status / ID (need Bus Turn-Around on Lane0) */
	if (!jd9365tx_dcs_read_log(dsi, 0x0A, &b, 1, "power-mode"))
		ok++;
	else
		fail++;

	if (!jd9365tx_dcs_read_log(dsi, 0x0B, &b, 1, "address-mode"))
		ok++;
	else
		fail++;

	if (!jd9365tx_dcs_read_log(dsi, 0x0C, &b, 1, "pixel-format"))
		ok++;
	else
		fail++;

	if (!jd9365tx_dcs_read_log(dsi, 0x04, id, 3, "display-id"))
		ok++;
	else
		fail++;

	if (!jd9365tx_dcs_read_log(dsi, 0xDA, &b, 1, "RDDID1/manuf"))
		ok++;
	else
		fail++;

	if (!jd9365tx_dcs_read_log(dsi, 0xDB, &b, 1, "RDDID2"))
		ok++;
	else
		fail++;

	if (!jd9365tx_dcs_read_log(dsi, 0xDC, &b, 1, "RDDID3"))
		ok++;
	else
		fail++;

	dsi->mode_flags = saved_flags;

	dev_info(&dsi->dev,
		 "=== DSI BTA diag [%s] done: ok=%d fail=%d ===\n",
		 tag, ok, fail);
	if (fail && !ok)
		dev_warn(&dsi->dev,
			 "All DSI reads failed: Lane0 BTA dead (wiring/P-N/CLK) "
			 "or panel ignores BTA. I2C OK does NOT prove MIPI OK.\n");
	else if (ok && fail)
		dev_warn(&dsi->dev,
			 "Partial DSI reads: link flaky or panel only ACKs some cmds\n");
	else if (ok)
		dev_info(&dsi->dev,
			 "DSI bidirectional OK — MIPI Lane0 likely wired; "
			 "if still black, check HS video / timing\n");
}

static int jd9365tx_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);
	const struct drm_display_mode *src =
		use_60hz ? &jd9365tx_7kf82_mode_60 : &jd9365tx_7kf82_mode_50;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, src);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "failed to duplicate display mode\n");
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static enum drm_panel_orientation
jd9365tx_get_orientation(struct drm_panel *panel)
{
	struct jd9365tx_panel *ctx = to_jd9365tx(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs jd9365tx_funcs = {
	.prepare	= jd9365tx_prepare,
	.enable		= jd9365tx_enable,
	.disable	= jd9365tx_disable,
	.unprepare	= jd9365tx_unprepare,
	.get_modes	= jd9365tx_get_modes,
	.get_orientation = jd9365tx_get_orientation,
};

static int jd9365tx_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct jd9365tx_panel *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM;
	if (non_cont_clock)
		dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;

	dev_dbg(dev, "flags=0x%lx %sHz\n",
		dsi->mode_flags, use_60hz ? "60" : "50");

	ctx->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset),
				     "failed to get reset GPIO\n");

	ctx->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd))
		return dev_err_probe(dev, PTR_ERR(ctx->vdd),
				     "failed to get vdd regulator\n");

	ctx->iovcc = devm_regulator_get(dev, "iovcc");
	if (IS_ERR(ctx->iovcc))
		return dev_err_probe(dev, PTR_ERR(ctx->iovcc),
				     "failed to get iovcc regulator\n");

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get orientation\n");

	drm_panel_init(&ctx->panel, dev, &jd9365tx_funcs, DRM_MODE_CONNECTOR_DSI);

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret) {
		dev_warn(dev, "backlight not linked (%d); panel continues\n", ret);
		ctx->panel.backlight = NULL;
	}

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach DSI device\n");
	}

	jd9365tx_reboot_nb.notifier_call = jd9365tx_reboot_notifier;
	jd9365tx_reboot_nb.priority = 200;
	g_reboot_dsi = dsi;
	if (register_reboot_notifier(&jd9365tx_reboot_nb)) {
		dev_warn(dev, "register_reboot_notifier failed\n");
		g_reboot_dsi = NULL;
	}

	dev_info(dev, "viewe 7kf82 panel probed\n");
	return 0;
}

static void jd9365tx_remove(struct mipi_dsi_device *dsi)
{
	struct jd9365tx_panel *ctx = mipi_dsi_get_drvdata(dsi);

	if (g_reboot_dsi) {
		unregister_reboot_notifier(&jd9365tx_reboot_nb);
		g_reboot_dsi = NULL;
	}
	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static void jd9365tx_shutdown(struct mipi_dsi_device *dsi)
{
	if (!mipi_dsi_get_drvdata(dsi))
		return;

	jd9365tx_force_sleep(dsi);
}

static const struct of_device_id jd9365tx_of_match[] = {
	{ .compatible = "viewe,7kf82" },
	{ .compatible = "jadard,jd9365tx-7kf82" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jd9365tx_of_match);

static struct mipi_dsi_driver jd9365tx_driver = {
	.probe = jd9365tx_probe,
	.remove = jd9365tx_remove,
	.shutdown = jd9365tx_shutdown,
	.driver = {
		.name = "panel-viewe-jd9365tx",
		.of_match_table = jd9365tx_of_match,
	},
};
module_mipi_dsi_driver(jd9365tx_driver);

MODULE_AUTHOR("Hefei <3066883572@qq.com>");
MODULE_DESCRIPTION("VIEWE 7\" JD9365TX MIPI-DSI panel");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: viewe_bl_i2c");
