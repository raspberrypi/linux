#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pm.h>

#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#define DF_DSI_DRIVER_NAME "df-ts-dsi"

struct df_panel {
  struct drm_panel base;
  struct mipi_dsi_device *dsi;
  struct i2c_client *i2c;
  const struct drm_display_mode *mode;
  enum drm_panel_orientation orientation;
};

static const struct drm_display_mode df_panel_8_8_mode = {
  .clock = 66300,

  .hdisplay = 480,
  .hsync_start = 480 + /* HFP */ 30,
  .hsync_end = 480 + 30 + /* HSync */ 30,
  .htotal = 480 + 30 + 30 + /* HBP */ 30,

  .vdisplay = 1920,
  .vsync_start = 1920 + /* VFP */ 6,
  .vsync_end = 1920 + 6 + /* VSync */ 6,
  .vtotal = 1920 + 6 + 6 + /* VBP */ 6,
};
static struct df_panel *panel_to_ts(struct drm_panel *panel)
{
  return container_of(panel, struct df_panel, base);
}

static void df_panel_i2c_write(struct df_panel *ts, u8 reg, u8 val)
{
  int ret;
  
  ret = i2c_smbus_write_byte_data(ts->i2c, reg, val);
  if(ret)
    dev_err(&ts->i2c->dev, "I2C write failed: %d\n", ret);
}

static int df_panel_disable(struct drm_panel *panel)
{
  struct df_panel *ts = panel_to_ts(panel);
  df_panel_i2c_write(ts, 0x01, 0x00);
  return 0;
}

static int df_panel_unprepare(struct drm_panel *panel)
{
  struct df_panel *ts = panel_to_ts(panel);
  df_panel_i2c_write(ts, 0x02, 0x00);
  return 0;
}

static int df_panel_prepare(struct drm_panel *panel)
{
  struct df_panel *ts = panel_to_ts(panel);
  df_panel_i2c_write(ts, 0x02, 0x01);
  return 0;
}

static int df_panel_enable(struct drm_panel *panel)
{
  struct df_panel *ts = panel_to_ts(panel);
  df_panel_i2c_write(ts, 0x01, 0xFF);
  return 0;
}

static int df_panel_get_modes(struct drm_panel *panel,struct drm_connector *connector)
{
  static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
  struct df_panel *ts = panel_to_ts(panel);
  struct drm_display_mode *mode;
  
  mode = drm_mode_duplicate(connector->dev, ts->mode);
  if (!mode) {
    dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
    ts->mode->hdisplay,
    ts->mode->vdisplay,
    drm_mode_vrefresh(ts->mode));
  }
  
  mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
  
  drm_mode_set_name(mode);
  
  drm_mode_probed_add(connector, mode);
  
  connector->display_info.bpc = 8;
  connector->display_info.width_mm = 64;
  connector->display_info.height_mm = 231;
  drm_display_info_set_bus_formats(&connector->display_info,&bus_format, 1);

  /*
   * TODO: Remove once all drm drivers call
   * drm_connector_set_orientation_from_panel()
   */
  drm_connector_set_panel_orientation(connector, ts->orientation);

  return 1;
}

static enum drm_panel_orientation df_panel_get_orientation(struct drm_panel *panel)
{
  struct df_panel *ts = panel_to_ts(panel);
  return ts->orientation;
}

static const struct drm_panel_funcs df_panel_funcs = {
  .disable = df_panel_disable,
  .unprepare = df_panel_unprepare,
  .prepare = df_panel_prepare,
  .enable = df_panel_enable,
  .get_modes = df_panel_get_modes,
  .get_orientation = df_panel_get_orientation,
};

static int df_panel_bl_update_status(struct backlight_device *bl)
{
  struct df_panel *ts = bl_get_data(bl);
  df_panel_i2c_write(ts, 0x01, backlight_get_brightness(bl));
  return 0;
}

static const struct backlight_ops df_panel_bl_ops = {
  .update_status = df_panel_bl_update_status,
};

static struct backlight_device *
df_panel_create_backlight(struct df_panel *ts)
{
  struct device *dev = ts->base.dev;
  const struct backlight_properties props = {
    .type = BACKLIGHT_RAW,
    .brightness = 255,
    .max_brightness = 255,
  };

  return devm_backlight_device_register(dev, "dfrobot", dev, ts,&df_panel_bl_ops, &props);
}


static int df_panel_probe(struct i2c_client *i2c)
{
  struct device *dev = &i2c->dev;
  struct df_panel *ts;
  struct device_node *endpoint, *dsi_host_node;
  struct mipi_dsi_host *host;
  struct mipi_dsi_device_info info = {
    .type = DF_DSI_DRIVER_NAME,
    .channel = 0,
    .node = NULL,
  };
  int ret;
  ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
  if(!ts)
    return -ENOMEM;
  
  ts->mode = of_device_get_match_data(dev);
  if(!ts->mode)
    return -EINVAL;
  
  i2c_set_clientdata(i2c, ts);
  
  ts->i2c = i2c;
  
  ret = of_drm_get_panel_orientation(dev->of_node, &ts->orientation);
  if(ret) {
    dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
    return ret;
  }
  
  /* Look up the DSI host.  It needs to probe before we do. */
  endpoint = of_graph_get_next_endpoint(dev->of_node, NULL);
  if(!endpoint)
    return -ENODEV;
  
  dsi_host_node = of_graph_get_remote_port_parent(endpoint);
  if(!dsi_host_node)
    goto error;
  
  host = of_find_mipi_dsi_host_by_node(dsi_host_node);
  of_node_put(dsi_host_node);
  if(!host) {
    of_node_put(endpoint);
    return -EPROBE_DEFER;
  }
  
  info.node = of_graph_get_remote_port(endpoint);
  if(!info.node)
    goto error;

  of_node_put(endpoint);

  ts->dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
  if (IS_ERR(ts->dsi)) {
    dev_err(dev, "DSI device registration failed: %ld\n",PTR_ERR(ts->dsi));
    return PTR_ERR(ts->dsi);
  }
  
  drm_panel_init(&ts->base, dev, &df_panel_funcs,DRM_MODE_CONNECTOR_DSI);
  
  ts->base.backlight = df_panel_create_backlight(ts);
  if(IS_ERR(ts->base.backlight)) {
    ret = PTR_ERR(ts->base.backlight);
    dev_err(dev, "Failed to create backlight: %d\n", ret);
    return ret;
  }
  
  /* This appears last, as it's what will unblock the DSI host
   * driver's component bind function.
   */
  drm_panel_add(&ts->base);
  
  ts->dsi->mode_flags = (MIPI_DSI_MODE_VIDEO |
                         MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
                         MIPI_DSI_MODE_LPM);
  
  ts->dsi->format = MIPI_DSI_FMT_RGB888;
  ts->dsi->lanes = 2;
  
  ret = devm_mipi_dsi_attach(dev, ts->dsi);
  
  if(ret){
    dev_err(dev, "failed to attach dsi to host: %d\n", ret);
  }

  return 0;

error:
  of_node_put(endpoint);
  return -ENODEV;
}  

static void df_panel_remove(struct i2c_client *i2c)
{
  struct df_panel *ts = i2c_get_clientdata(i2c);
  drm_panel_remove(&ts->base);
}

static const struct of_device_id df_panel_of_ids[] = {
  {
    .compatible = "DFRobot,8.8inch-panel",
    .data = &df_panel_8_8_mode,
  },
  {
  /* sentinel */
  }
};
MODULE_DEVICE_TABLE(of, df_panel_of_ids);

static struct i2c_driver df_panel_driver = {
  .driver = {
    .name = "df_touchscreen",
    .of_match_table = df_panel_of_ids,
  },
  .probe = df_panel_probe,
  .remove = df_panel_remove,
};
module_i2c_driver(df_panel_driver);

MODULE_AUTHOR("Fary <feng.yang@dfrobot.com>");
MODULE_DESCRIPTION("DFRobot DSI panel driver");
MODULE_LICENSE("GPL");
