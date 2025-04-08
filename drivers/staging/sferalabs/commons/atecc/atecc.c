#include "atecc.h"

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/version.h>

struct AteccBean {
  uint8_t serialNumber[9];
  bool probed;
};

static struct AteccBean _atecc = {
    .probed = false,
};

static void _getCRC16LittleEndian(size_t length, const uint8_t *data,
                                  uint8_t *crc_le) {
  size_t counter;
  uint16_t crc = 0;
  uint16_t polynom = 0x8005;
  uint8_t shift;
  uint8_t data_bit, crc_bit;

  for (counter = 0; counter < length; counter++) {
    for (shift = 0x01; shift > 0x00; shift <<= 1) {
      data_bit = (data[counter] & shift) ? 1 : 0;
      crc_bit = crc >> 15;
      crc <<= 1;
      if (data_bit != crc_bit) {
        crc ^= polynom;
      }
    }
  }
  crc_le[0] = (uint8_t)(crc & 0x00FF);
  crc_le[1] = (uint8_t)(crc >> 8);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
static int _atecc_i2c_probe(struct i2c_client *client) {
#else
static int _atecc_i2c_probe(struct i2c_client *client,
                            const struct i2c_device_id *id) {
#endif
  uint8_t i;
  int ret = -1;
  uint8_t i2c_response[35];
  uint8_t crc_le[2];

  /*
   * 0x03 = normal command
   * 0x07 = total bytes for CRC generation (2 CRC bytes included)
   * 0x02 = read operation
   * 0x80 = read 32 bytes from configuration memory area
   * 0x00 = configuration memory address part 1
   * 0x00 = configuration memory address part 2
   * 0x09 = CRC byte 1 in little endian format
   * 0xAD = CRC byte 2 in little endian format
   */
  uint8_t cmd_read_sn[8] = {0x03, 0x07, 0x02, 0x80, 0x00, 0x00, 0x09, 0xAD};
  uint8_t cmd_wake = 0x00;

  for (i = 0; i < 10; i++) {
    i2c_master_send(client, &cmd_wake, 1);
    msleep(1);
    if (i2c_master_send(client, cmd_read_sn, 8) == 8) {
      msleep(1);
      if (i2c_master_recv(client, i2c_response, 35) == 35) {
        _getCRC16LittleEndian(33, i2c_response, crc_le);
        if (crc_le[0] == i2c_response[33] && crc_le[1] == i2c_response[34]) {
          memcpy(&_atecc.serialNumber[0], &i2c_response[1], 4);
          memcpy(&_atecc.serialNumber[4], &i2c_response[9], 5);
          _atecc.probed = true;
          ret = 0;
          break;
        }
      }
    }
    msleep(10);
  }

  return ret;
}

const struct of_device_id _atecc_of_match[] = {
    {
        .compatible = "sferalabs,atecc",
    },
    {},
};
MODULE_DEVICE_TABLE(of, _atecc_of_match);

static const struct i2c_device_id _atecc_i2c_id[] = {
    {"atecc", 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, _atecc_i2c_id);

static struct i2c_driver _atecc_i2c_driver = {
    .driver =
        {
            .name = "atecc",
            .owner = THIS_MODULE,
            .of_match_table = of_match_ptr(_atecc_of_match),
        },
    .probe = _atecc_i2c_probe,
    .id_table = _atecc_i2c_id,
};

ssize_t devAttrAteccSerial_show(struct device *dev,
                                struct device_attribute *attr, char *buf) {
  if (!_atecc.probed) {
    i2c_add_driver(&_atecc_i2c_driver);
    i2c_del_driver(&_atecc_i2c_driver);
    if (!_atecc.probed) {
      return -ENODEV;
    }
  }
  return sprintf(
      buf, "%02hX %02hX %02hX %02hX %02hX %02hX %02hX %02hX %02hX\n",
      _atecc.serialNumber[0], _atecc.serialNumber[1], _atecc.serialNumber[2],
      _atecc.serialNumber[3], _atecc.serialNumber[4], _atecc.serialNumber[5],
      _atecc.serialNumber[6], _atecc.serialNumber[7], _atecc.serialNumber[8]);
}
