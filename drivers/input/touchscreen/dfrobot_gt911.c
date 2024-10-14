#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#define GT_GSTID_REG 0x814E

#define RPI_TS_DEFAULT_WIDTH 1920
#define RPI_TS_DEFAULT_HEIGHT 480
#define RPI_TS_POLL_INTERVAL 17
#define RPI_TS_MAX_SUPPORTED_POINTS	10
struct gt911_data {
  struct i2c_client *client;
  struct input_dev *input;
};

typedef struct
{
  uint8_t xLow[5];
  uint8_t xHigh[5];
  uint8_t yLow[5];
  uint8_t yHigh[5];
  uint8_t finger[5];
  uint8_t point;
  uint8_t event;
}TouchData;


int goodix_i2c_write(struct i2c_client *client, u16 reg, const u8 *buf, int len)
{
  u8 *addr_buf;
  struct i2c_msg msg;
  int ret;
  
  addr_buf = kmalloc(len + 2, GFP_KERNEL);
  if (!addr_buf)
    return -ENOMEM;
  
  addr_buf[0] = reg >> 8;
  addr_buf[1] = reg & 0xFF;
  memcpy(&addr_buf[2], buf, len);
  
  msg.flags = 0;
  msg.addr = client->addr;
  msg.buf = addr_buf;
  msg.len = len + 2;
  
  ret = i2c_transfer(client->adapter, &msg, 1);
  if (ret >= 0)
    ret = (ret == 1 ? 0 : -EIO);
  
  kfree(addr_buf);
  
  if (ret)
    dev_err(&client->dev, "Error writing %d bytes to 0x%04x: %d\n",len, reg, ret);
  return ret;
}

int goodix_i2c_read(struct i2c_client *client, u16 reg, u8 *buf, int len)
{
  struct i2c_msg msgs[2];
  __be16 wbuf = cpu_to_be16(reg);
  int ret;
  
  msgs[0].flags = 0;
  msgs[0].addr  = client->addr;
  msgs[0].len   = 2;
  msgs[0].buf   = (u8 *)&wbuf;
  
  msgs[1].flags = I2C_M_RD;
  msgs[1].addr  = client->addr;
  msgs[1].len   = len;
  msgs[1].buf   = buf;
  
  ret = i2c_transfer(client->adapter, msgs, 2);
  if (ret >= 0)
    ret = (ret == ARRAY_SIZE(msgs) ? 0 : -EIO);
  
  if (ret)
    dev_err(&client->dev, "Error reading %d bytes from 0x%04x: %d\n",len, reg, ret);
  return ret;
}

static void gt911_read_touch_data(struct input_dev *input)
{
  struct gt911_data *data = input_get_drvdata(input);
  u8 touch_data[49];
  static uint8_t flag   = 0;     
  uint8_t  id=0,i=0; 
  uint16_t x=0,y=0;
  TouchData      MyTouchData;

  if(goodix_i2c_read(data->client, GT_GSTID_REG, touch_data, 1)) {
    dev_err(&data->client->dev, "Failed to read touch data\n");
    printk("err\r\n");
    return ;
  }

  if(touch_data[0] & 0x80){
    MyTouchData.point = touch_data[0] & 0x0F;
    if(MyTouchData.point > 0x05){
      MyTouchData.point = 0x05;
    }

    if(goodix_i2c_read(data->client, GT_GSTID_REG, touch_data, 9+8*MyTouchData.point)) {
      dev_err(&data->client->dev, "Failed to read touch data\n");
      return ;
    }

    for(i = 0; i < MyTouchData.point; i++){
      id = touch_data[1 + 8 * i];
      x  = (uint16_t)(touch_data[3 + 8 * i] & 0xFF) << 8 | (uint16_t)touch_data[2 + 8 * i];
      y  = (uint16_t)(touch_data[5 + 8 * i] & 0xFF) << 8 | (uint16_t)touch_data[4 + 8 * i];
      input_mt_slot(data->input, i);  
      input_mt_report_slot_state(data->input, MT_TOOL_FINGER, true);
      input_report_abs(data->input, ABS_MT_POSITION_X, x);  
      input_report_abs(data->input, ABS_MT_POSITION_Y, y);
    }
    
    for(;i<5;i++){
      input_mt_slot(data->input,i);
      input_mt_report_slot_inactive(data->input);
    }

    input_mt_sync_frame(input);
    input_sync(input);

    if(goodix_i2c_write(data->client, GT_GSTID_REG, &flag, 1)) {
      dev_err(&data->client->dev, "Failed to read touch data\n");
      return ;
    }
  }
  return ;
}


static int gt911_probe(struct i2c_client *client)
{
  struct gt911_data *data;
  struct input_dev *input;
  int error;

  data = devm_kzalloc(&client->dev, sizeof(struct gt911_data), GFP_KERNEL);
  if (!data)
      return -ENOMEM;

  data->client = client;
  i2c_set_clientdata(client, data);

  input = devm_input_allocate_device(&client->dev);
  if (!input)
      return -ENOMEM;

  data->input = input;
  input_set_drvdata(input, data);
  
  input->name = "GT911 Touchscreen";
  input->id.bustype = BUS_HOST;

  input_set_abs_params(input, ABS_MT_POSITION_X, 0, RPI_TS_DEFAULT_WIDTH, 0, 0);
  input_set_abs_params(input, ABS_MT_POSITION_Y, 0, RPI_TS_DEFAULT_HEIGHT, 0, 0);

  error = input_mt_init_slots(input, RPI_TS_MAX_SUPPORTED_POINTS,INPUT_MT_DIRECT);
  if(error) {
    dev_err(&client->dev, "could not init mt slots, %d\n", error);
  return error;
  }

  error = input_setup_polling(input, gt911_read_touch_data);
  if(error){
    dev_err(&client->dev, "could not set up polling mode, %d\n", error);
    return error;
  }

  input_set_poll_interval(input, RPI_TS_POLL_INTERVAL);  
  
  error = input_register_device(input);
  if(error) {
    dev_err(&client->dev, "could not register input device, %d\n", error);
    return error;
  }

  return 0;
}

static void gt911_remove(struct i2c_client *client)
{
  struct gt911_data *data = i2c_get_clientdata(client);

  input_unregister_device(data->input);
}

static const struct i2c_device_id gt911_id[] = {
  { "gt911", 0 },
  { }
};
MODULE_DEVICE_TABLE(i2c, gt911_id);

static struct i2c_driver gt911_driver = {
  .driver = {
      .name = "gt911",
      .owner = THIS_MODULE,
  },
  .probe = gt911_probe,
  .remove = gt911_remove,
  .id_table = gt911_id,
};

static int __init gt911_init(void)
{
  return i2c_add_driver(&gt911_driver);
}

static void __exit gt911_exit(void)
{
  i2c_del_driver(&gt911_driver);
}

module_init(gt911_init);
module_exit(gt911_exit);

MODULE_AUTHOR("fary<feng.yang@dfrobot.com>");
MODULE_DESCRIPTION("GT911 Touchscreen Driver");
MODULE_LICENSE("GPL");
