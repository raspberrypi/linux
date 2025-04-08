/*
 * Iono Pi v3 kernel module
 *
 *     Copyright (C) 2024-2025 Sfera Labs S.r.l.
 *
 *     For information, visit https://www.sferalabs.cc
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * LICENSE.txt file for more details.
 *
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/version.h>

#include "../commons/atecc/atecc.h"
#include "../commons/misc/misc.h"
#include "../commons/gpio/gpio.h"
#include "../commons/wiegand/wiegand.h"
#include "pcf2131/pcf2131.h"

#define MCP3462_CMD_DEV_ADDR 0b01000000

#define MCP3462_CMD_READ_STATIC (MCP3462_CMD_DEV_ADDR | 0b00000001)
#define MCP3462_CMD_WRITE_INCR (MCP3462_CMD_DEV_ADDR | 0b00000010)
#define MCP3462_CMD_READ_INCR (MCP3462_CMD_DEV_ADDR | 0b00000011)
#define MCP3462_CMD_CONV_START (MCP3462_CMD_DEV_ADDR | 0b00101000)

#define MCP3462_STAT_DR_STATUS_MASK 0b00000100
#define MCP3462_STAT_DEV_ADDR_MASK 0b00111000
#define MCP3462_STAT_DEV_ADDR 0b00010000

#define MCP3462_REG_ADDR_ADCDATA (0x0 << 2)
#define MCP3462_REG_ADDR_CONFIG0 (0x1 << 2)
#define MCP3462_REG_ADDR_CONFIG1 (0x2 << 2)
#define MCP3462_REG_ADDR_CONFIG2 (0x3 << 2)
#define MCP3462_REG_ADDR_CONFIG3 (0x4 << 2)
#define MCP3462_REG_ADDR_MUX (0x6 << 2)

#define MCP3462_CONFIG0_ADC_MODE 0b00000011  // Conversion mode
#define MCP3462_CONFIG0_CS_SEL 0b00000000    // No current source
#define MCP3462_CONFIG0_CLK_SEL 0b00100000   // Internal clock, no output
#define MCP3462_CONFIG0_VREF_SEL 0b10000000  // Internal voltage reference
#define MCP3462_CONFIG0                                 \
  (MCP3462_CONFIG0_VREF_SEL | MCP3462_CONFIG0_CLK_SEL | \
   MCP3462_CONFIG0_CS_SEL | MCP3462_CONFIG0_ADC_MODE)

#define MCP3462_CONFIG1_PRE 0b00000000       // AMCLK = MCLK
#define MCP3462_CONFIG1_RESERVED 0b00000000  // Always 00

#define MCP3462_CONFIG2_RESERVED 0b00000001  // Always 1
#define MCP3462_CONFIG2_AZ_REF \
  0b00000010  // Auto-Zeroing Reference Buffer Setting enabled
#define MCP3462_CONFIG2_GAIN 0b00001000   // Gain x 1
#define MCP3462_CONFIG2_BOOST 0b10000000  // Current x 1

#define MCP3462_CONFIG3_EN_GAINCAL 0b00000000   // Disabled
#define MCP3462_CONFIG3_EN_OFFCAL 0b00000000    // Disabled
#define MCP3462_CONFIG3_EN_CRCCOM 0b00000100    // Enabled
#define MCP3462_CONFIG3_CRC_FORMAT 0b00000000   // 16 bit
#define MCP3462_CONFIG3_DATA_FORMAT 0b00000000  // 16 bit
#define MCP3462_CONFIG3_CONV_MODE 0b11000000    // Continuous
#define MCP3462_CONFIG3                                      \
  (MCP3462_CONFIG3_CONV_MODE | MCP3462_CONFIG3_DATA_FORMAT | \
   MCP3462_CONFIG3_CRC_FORMAT | MCP3462_CONFIG3_EN_CRCCOM |  \
   MCP3462_CONFIG3_EN_OFFCAL | MCP3462_CONFIG3_EN_GAINCAL)

#define MCP3462_MUX_SE_CH0 0x08
#define MCP3462_MUX_SE_CH1 0x18
#define MCP3462_MUX_SE_CH2 0x28
#define MCP3462_MUX_SE_CH3 0x38

#define MCP3462_FACTOR_AV 1125
#define MCP3462_FACTOR_AI 750
#define MCP3462_DIVISOR 1024

#define MCP3462_OSR_DEFAULT 0b0011
#define MCP3462_AZ_MUX_DEFAULT 0b1

#define LOG_TAG "ionopi: "

struct DeviceAttrBean {
  struct device_attribute devAttr;
  void *data;
};

struct DeviceBean {
  char *name;
  struct device *pDevice;
  struct DeviceAttrBean *devAttrBeans;
};

static struct class *pDeviceClass;

static ssize_t devAttrMcp3462Osr_show(struct device *dev,
                                      struct device_attribute *attr, char *buf);

static ssize_t devAttrMcp3462Osr_store(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count);

static ssize_t devAttrMcp3462AzMux_show(struct device *dev,
                                        struct device_attribute *attr,
                                        char *buf);

static ssize_t devAttrMcp3462AzMux_store(struct device *dev,
                                         struct device_attribute *attr,
                                         const char *buf, size_t count);

static ssize_t devAttrAv1Mv_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

static ssize_t devAttrAv2Mv_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

static ssize_t devAttrAi1Ua_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

static ssize_t devAttrAi2Ua_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

static ssize_t devAttrAv1Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf);

static ssize_t devAttrAv2Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf);

static ssize_t devAttrAi1Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf);

static ssize_t devAttrAi2Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf);

static ssize_t devAttrMax4896_show(struct device *dev,
                                   struct device_attribute *attr, char *buf);

static ssize_t devAttrMax4896_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count);

enum dout_e {
  DO_O1 = 0,
  DO_O2,
  DO_O3,
  DO_O4,
  DO_OC1,
  DO_OC2,
  DO_OC3,
  DO_ALL,
};

struct max4896_out {
  uint8_t mask;
  uint8_t shift;
};

static struct max4896_out _max4896_outs[] = {
    [DO_O1] =
        {
            .mask = 0b1,
            .shift = 0,
        },
    [DO_O2] =
        {
            .mask = 0b1,
            .shift = 1,
        },
    [DO_O3] =
        {
            .mask = 0b1,
            .shift = 2,
        },
    [DO_O4] =
        {
            .mask = 0b1,
            .shift = 3,
        },
    [DO_OC1] =
        {
            .mask = 0b1,
            .shift = 4,
        },
    [DO_OC2] =
        {
            .mask = 0b1,
            .shift = 5,
        },
    [DO_OC3] =
        {
            .mask = 0b1,
            .shift = 6,
        },
    [DO_ALL] =
        {
            .mask = 0b1111111,
            .shift = 0,
        },
};

enum din_e {
  DI1,
  DI2,
  DI3,
  DI4,
  DI5,
  DI6,
  DI_SIZE,
};

enum dt_e {
  DT1,
  DT2,
  DT3,
  DT4,
};

enum gpio_e {
  LED,
  DO_RESET,
  DO_FLAG,
  DO_PDCD,
  DO_SPLD,
  V5OF,
  GPIO_SIZE,
};

static struct GpioBean _gpio[] = {
    [LED] =
        {
            .name = "ionopi_led",
            .flags = GPIOD_OUT_LOW,
        },
    [DO_RESET] =
        {
            .name = "ionopi_do_reset",
            .invert = true,
            .flags = GPIOD_OUT_HIGH,
        },
    [DO_FLAG] =
        {
            .name = "ionopi_do_flag",
            .invert = true,
            .flags = GPIOD_IN,
        },
    [DO_PDCD] =
        {
            .name = "ionopi_do_pdcd",
            .invert = true,
            .flags = GPIOD_OUT_HIGH,
        },
    [DO_SPLD] =
        {
            .name = "ionopi_do_spld",
            .invert = true,
            .flags = GPIOD_OUT_HIGH,
        },
    [V5OF] =
        {
            .name = "ionopi_v5of",
            .flags = GPIOD_IN,
            .invert = true,
        },
};

static struct GpioBean _gpio_dt[] = {
    [DT1] =
        {
            .name = "ionopi_dt1",
        },
    [DT2] =
        {
            .name = "ionopi_dt2",
        },
    [DT3] =
        {
            .name = "ionopi_dt3",
        },
    [DT4] =
        {
            .name = "ionopi_dt4",
        },
};

static struct DebouncedGpioBean _gpio_din[] = {
    [DI1] =
        {
            .gpio =
                {
                    .name = "ionopi_di1",
                    .flags = GPIOD_IN,
                },
        },
    [DI2] =
        {
            .gpio =
                {
                    .name = "ionopi_di2",
                    .flags = GPIOD_IN,
                },
        },
    [DI3] =
        {
            .gpio =
                {
                    .name = "ionopi_di3",
                    .flags = GPIOD_IN,
                },
        },
    [DI4] =
        {
            .gpio =
                {
                    .name = "ionopi_di4",
                    .flags = GPIOD_IN,
                },
        },
    [DI5] =
        {
            .gpio =
                {
                    .name = "ionopi_di5",
                    .flags = GPIOD_IN,
                },
        },
    [DI6] =
        {
            .gpio =
                {
                    .name = "ionopi_di6",
                    .flags = GPIOD_IN,
                },
        },
};

static struct WiegandBean _w1 = {
    .d0 =
        {
            .gpio = &_gpio_dt[DT1],
        },
    .d1 =
        {
            .gpio = &_gpio_dt[DT2],
        },
};

static struct WiegandBean _w2 = {
    .d0 =
        {
            .gpio = &_gpio_dt[DT3],
        },
    .d1 =
        {
            .gpio = &_gpio_dt[DT4],
        },
};

static struct DeviceAttrBean devAttrBeansLed[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "status",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio[LED],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "blink",
                        .mode = 0220,
                    },
                .store = devAttrGpioBlink_store,
            },
        .data = &_gpio[LED],
    },

    {},
};

static struct DeviceAttrBean devAttrBeansDigitalIn[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di1",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
            },
        .data = &_gpio_din[DI1].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di2",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
            },
        .data = &_gpio_din[DI2].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di3",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
            },
        .data = &_gpio_din[DI3].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di4",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
            },
        .data = &_gpio_din[DI4].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di5",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
            },
        .data = &_gpio_din[DI5].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di6",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
            },
        .data = &_gpio_din[DI6].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di1_deb",
                        .mode = 0440,
                    },
                .show = devAttrGpioDeb_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI1].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di2_deb",
                        .mode = 0440,
                    },
                .show = devAttrGpioDeb_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI2].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di3_deb",
                        .mode = 0440,
                    },
                .show = devAttrGpioDeb_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI3].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di4_deb",
                        .mode = 0440,
                    },
                .show = devAttrGpioDeb_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI4].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di5_deb",
                        .mode = 0440,
                    },
                .show = devAttrGpioDeb_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI5].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di6_deb",
                        .mode = 0440,
                    },
                .show = devAttrGpioDeb_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI6].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di1_deb_on_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOn_show,
                .store = devAttrGpioDebMsOn_store,
            },
        .data = &_gpio_din[DI1].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di1_deb_off_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOff_show,
                .store = devAttrGpioDebMsOff_store,
            },
        .data = &_gpio_din[DI1].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di2_deb_on_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOn_show,
                .store = devAttrGpioDebMsOn_store,
            },
        .data = &_gpio_din[DI2].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di2_deb_off_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOff_show,
                .store = devAttrGpioDebMsOff_store,
            },
        .data = &_gpio_din[DI2].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di3_deb_on_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOn_show,
                .store = devAttrGpioDebMsOn_store,
            },
        .data = &_gpio_din[DI3].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di3_deb_off_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOff_show,
                .store = devAttrGpioDebMsOff_store,
            },
        .data = &_gpio_din[DI3].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di4_deb_on_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOn_show,
                .store = devAttrGpioDebMsOn_store,
            },
        .data = &_gpio_din[DI4].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di4_deb_off_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOff_show,
                .store = devAttrGpioDebMsOff_store,
            },
        .data = &_gpio_din[DI4].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di5_deb_on_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOn_show,
                .store = devAttrGpioDebMsOn_store,
            },
        .data = &_gpio_din[DI5].gpio,
    },


    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di5_deb_off_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOff_show,
                .store = devAttrGpioDebMsOff_store,
            },
        .data = &_gpio_din[DI5].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di6_deb_on_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOn_show,
                .store = devAttrGpioDebMsOn_store,
            },
        .data = &_gpio_din[DI6].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di6_deb_off_ms",
                        .mode = 0660,
                    },
                .show = devAttrGpioDebMsOff_show,
                .store = devAttrGpioDebMsOff_store,
            },
        .data = &_gpio_din[DI6].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di1_deb_on_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOnCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI1].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di1_deb_off_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOffCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI1].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di2_deb_on_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOnCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI2].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di2_deb_off_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOffCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI2].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di3_deb_on_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOnCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI3].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di3_deb_off_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOffCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI3].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di4_deb_on_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOnCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI4].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di4_deb_off_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOffCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI4].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di5_deb_on_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOnCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI5].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di5_deb_off_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOffCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI5].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di6_deb_on_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOnCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI6].gpio,
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "di6_deb_off_cnt",
                        .mode = 0440,
                    },
                .show = devAttrGpioDebOffCnt_show,
                .store = NULL,
            },
        .data = &_gpio_din[DI6].gpio,
    },

    {},
};

static struct DeviceAttrBean devAttrBeansDigitalIO[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt1_mode",
                        .mode = 0660,
                    },
                .show = devAttrGpioMode_show,
                .store = devAttrGpioMode_store,
            },
        .data = &_gpio_dt[DT1],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt2_mode",
                        .mode = 0660,
                    },
                .show = devAttrGpioMode_show,
                .store = devAttrGpioMode_store,
            },
        .data = &_gpio_dt[DT2],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt3_mode",
                        .mode = 0660,
                    },
                .show = devAttrGpioMode_show,
                .store = devAttrGpioMode_store,
            },
        .data = &_gpio_dt[DT3],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt4_mode",
                        .mode = 0660,
                    },
                .show = devAttrGpioMode_show,
                .store = devAttrGpioMode_store,
            },
        .data = &_gpio_dt[DT4],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt1",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio_dt[DT1],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt2",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio_dt[DT2],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt3",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio_dt[DT3],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "dt4",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio_dt[DT4],
    },

    {},
};

static struct DeviceAttrBean devAttrBeansAnalogIn[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "osr",
                        .mode = 0660,
                    },
                .show = devAttrMcp3462Osr_show,
                .store = devAttrMcp3462Osr_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "az_mux",
                        .mode = 0660,
                    },
                .show = devAttrMcp3462AzMux_show,
                .store = devAttrMcp3462AzMux_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "av1_mv",
                        .mode = 0440,
                    },
                .show = devAttrAv1Mv_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "av2_mv",
                        .mode = 0440,
                    },
                .show = devAttrAv2Mv_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "ai1_ua",
                        .mode = 0440,
                    },
                .show = devAttrAi1Ua_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "ai2_ua",
                        .mode = 0440,
                    },
                .show = devAttrAi2Ua_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "av1_raw",
                        .mode = 0440,
                    },
                .show = devAttrAv1Raw_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "av2_raw",
                        .mode = 0440,
                    },
                .show = devAttrAv2Raw_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "ai1_raw",
                        .mode = 0440,
                    },
                .show = devAttrAi1Raw_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "ai2_raw",
                        .mode = 0440,
                    },
                .show = devAttrAi2Raw_show,
            },
    },

    {},
};

static struct DeviceAttrBean devAttrBeansDigitalOut[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "pdc",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio[DO_PDCD],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "spl",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio[DO_SPLD],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "reset",
                        .mode = 0660,
                    },
                .show = devAttrGpio_show,
                .store = devAttrGpio_store,
            },
        .data = &_gpio[DO_RESET],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "flag",
                        .mode = 0440,
                    },
                .show = devAttrGpio_show,
                .store = NULL,
            },
        .data = &_gpio[DO_FLAG],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "o1",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_O1],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "o2",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_O2],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "o3",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_O3],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "o4",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_O4],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "oc1",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_OC1],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "oc2",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_OC2],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "oc3",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_OC3],
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "outputs",
                        .mode = 0660,
                    },
                .show = devAttrMax4896_show,
                .store = devAttrMax4896_store,
            },
        .data = &_max4896_outs[DO_ALL],
    },

    {},
};

static struct DeviceAttrBean devAttrBeansWiegand[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_enabled",
                        .mode = 0660,
                    },
                .show = devAttrWiegandEnabled_show,
                .store = devAttrWiegandEnabled_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_data",
                        .mode = 0440,
                    },
                .show = devAttrWiegandData_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_noise",
                        .mode = 0440,
                    },
                .show = devAttrWiegandNoise_show,
                .store = NULL,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_pulse_itvl_min",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseIntervalMin_show,
                .store = devAttrWiegandPulseIntervalMin_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_pulse_itvl_max",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseIntervalMax_show,
                .store = devAttrWiegandPulseIntervalMax_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_pulse_width_min",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseWidthMin_show,
                .store = devAttrWiegandPulseWidthMin_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w1_pulse_width_max",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseWidthMax_show,
                .store = devAttrWiegandPulseWidthMax_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_enabled",
                        .mode = 0660,
                    },
                .show = devAttrWiegandEnabled_show,
                .store = devAttrWiegandEnabled_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_data",
                        .mode = 0440,
                    },
                .show = devAttrWiegandData_show,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_noise",
                        .mode = 0440,
                    },
                .show = devAttrWiegandNoise_show,
                .store = NULL,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_pulse_itvl_min",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseIntervalMin_show,
                .store = devAttrWiegandPulseIntervalMin_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_pulse_itvl_max",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseIntervalMax_show,
                .store = devAttrWiegandPulseIntervalMax_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_pulse_width_min",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseWidthMin_show,
                .store = devAttrWiegandPulseWidthMin_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "w2_pulse_width_max",
                        .mode = 0660,
                    },
                .show = devAttrWiegandPulseWidthMax_show,
                .store = devAttrWiegandPulseWidthMax_store,
            },
    },

    {},
};

static struct DeviceAttrBean devAttrBeansWatchdog[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "enabled",
                        .mode = 0660,
                    },
                .show = devAttrWatchdogEnabled_show,
                .store = devAttrWatchdogEnabled_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "heartbeat",
                        .mode = 0220,
                    },
                .show = NULL,
                .store = devAttrWatchdogHeartbeat_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "timeout",
                        .mode = 0660,
                    },
                .show = devAttrWatchdogTimeout_show,
                .store = devAttrWatchdogTimeout_store,
            },
    },

    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "off_time",
                        .mode = 0660,
                    },
                .show = devAttrWatchdogOffTime_show,
                .store = devAttrWatchdogOffTime_store,
            },
    },

    {},
};

static struct DeviceAttrBean devAttrBeansAtecc[] = {
    {
        .devAttr =
            {
                .attr =
                    {
                        .name = "serial_num",
                        .mode = 0440,
                    },
                .show = devAttrAteccSerial_show,
                .store = NULL,
            },
    },

    {},
};

static struct DeviceBean devices[] = {
    {
        .name = "led",
        .devAttrBeans = devAttrBeansLed,
    },

    {
        .name = "digital_in",
        .devAttrBeans = devAttrBeansDigitalIn,
    },

    {
        .name = "digital_io",
        .devAttrBeans = devAttrBeansDigitalIO,
    },

    {
        .name = "analog_in",
        .devAttrBeans = devAttrBeansAnalogIn,
    },

    {
        .name = "digital_out",
        .devAttrBeans = devAttrBeansDigitalOut,
    },

    {
        .name = "wiegand",
        .devAttrBeans = devAttrBeansWiegand,
    },

    {
        .name = "watchdog",
        .devAttrBeans = devAttrBeansWatchdog,
    },

    {
        .name = "sec_elem",
        .devAttrBeans = devAttrBeansAtecc,
    },

    {},
};

struct _ionopi_data_t {
  struct mutex spi_lock;
};

struct _mcp3462_data_t {
  uint8_t osr;
  uint8_t az_mux;
  struct spi_device *spi;
  struct spi_message msg;
  struct regulator *reg;
  uint8_t curr_channel;
  struct spi_transfer transfer[2];
  u8 cmd_tx_buf[5] ____cacheline_aligned;
  u8 cmd_rx_buf[5];
  u8 resp_rx_buf[5];
};

struct _max4896_data_t {
  struct spi_device *spi;
  struct spi_message msg;
  struct regulator *reg;
  bool first;
  struct spi_transfer transfer;
  u8 cmd ____cacheline_aligned;
  u8 state;
};

static struct _ionopi_data_t _ionopi_data;
static struct _mcp3462_data_t _mcp3462_data;
static struct _max4896_data_t _max4896_data;

static void *dev_attr_get_data(struct device_attribute *attr) {
  struct DeviceAttrBean *dab;
  dab = container_of(attr, struct DeviceAttrBean, devAttr);
  if (dab == NULL) {
    return NULL;
  }
  return dab->data;
}

struct GpioBean *gpioGetBean(struct device *dev, struct device_attribute *attr,
                             const char **vals) {
  return (struct GpioBean *)dev_attr_get_data(attr);
}

struct WiegandBean *wiegandGetBean(struct device *dev,
                                   struct device_attribute *attr) {
  if (attr->attr.name[1] == '1') {
    return &_w1;
  } else {
    return &_w2;
  }
}

static int _spi_lock(void) {
  int i, ret;
  ret = -EBUSY;
  for (i = 0; i < 40; i++) {
    if (mutex_trylock(&_ionopi_data.spi_lock)) {
      ret = 1;
      break;
    }
    msleep(1);
  }
  return ret;
}

static void _spi_unlock(void) { mutex_unlock(&_ionopi_data.spi_lock); }

static uint16_t mcp3462_crc(uint16_t crc, uint8_t dByte) {
  uint8_t k;
  crc ^= dByte << 8;
  for (k = 0; k < 8; k++) crc = crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1;
  return crc;
}

static int mcp3462_spi_msg(int cmdLen, int respLen) {
  int ret, i, j;
  uint16_t crc;

  if (respLen > 0) {
    respLen += 2;  // CRC bytes
  }
  _mcp3462_data.transfer[0].len = cmdLen;
  _mcp3462_data.transfer[1].len = respLen;

  spi_message_init_with_transfers(&_mcp3462_data.msg, _mcp3462_data.transfer,
                                  respLen > 0 ? 2 : 1);

  for (j = 0; j < 3; j++) {
    memset(&_mcp3462_data.cmd_rx_buf, 0, cmdLen);
    if (respLen > 0) {
      memset(&_mcp3462_data.resp_rx_buf, 0, respLen);
    }

    ret = spi_sync(_mcp3462_data.spi, &_mcp3462_data.msg);
    if (ret < 0) {
      pr_alert(LOG_TAG "mcp3462 spi xfer error\n");
      continue;
    }

    if ((_mcp3462_data.cmd_rx_buf[0] & MCP3462_STAT_DEV_ADDR_MASK) !=
        MCP3462_STAT_DEV_ADDR) {
      pr_alert(LOG_TAG "mcp3462 dev addr error\n");
      ret = -EIO;
      continue;
    }

    if (respLen > 0) {
      crc = mcp3462_crc(0, _mcp3462_data.cmd_rx_buf[0]);
      for (i = 0; i < respLen - 2; i++) {
        crc = mcp3462_crc(crc, _mcp3462_data.resp_rx_buf[i]);
      }
      if (_mcp3462_data.resp_rx_buf[i] != ((crc >> 8) & 0xff) ||
          _mcp3462_data.resp_rx_buf[i + 1] != (crc & 0xff)) {
        pr_alert(LOG_TAG "mcp3462 CRC error\n");
        ret = -EIO;
        continue;
      }
    }

    ret = 0;
    break;
  }

  return ret;
}

static ssize_t devAttrMcp3462Osr_show(struct device *dev,
                                      struct device_attribute *attr,
                                      char *buf) {
  return sprintf(buf, "%d\n", _mcp3462_data.osr);
}

static bool mcp3462_check_config(uint8_t addr, uint8_t val) {
  int ret;
  _mcp3462_data.cmd_tx_buf[0] = MCP3462_CMD_READ_STATIC | addr;
  ret = mcp3462_spi_msg(1, 1);
  if (ret < 0) {
    return false;
  }
  if (_mcp3462_data.resp_rx_buf[0] != val) {
    return false;
  }
  return true;
}

static int mcp3462_config(void) {
  int ret;

  u8 cfg1 = MCP3462_CONFIG1_RESERVED | ((_mcp3462_data.osr & 0xf) << 2) |
            MCP3462_CONFIG1_PRE;
  u8 cfg2 = MCP3462_CONFIG2_BOOST | MCP3462_CONFIG2_GAIN |
            ((_mcp3462_data.az_mux & 0x1) << 2) | MCP3462_CONFIG2_AZ_REF |
            MCP3462_CONFIG2_RESERVED;

  // Write CONFIG1-3

  _mcp3462_data.cmd_tx_buf[0] =
      MCP3462_CMD_WRITE_INCR | MCP3462_REG_ADDR_CONFIG1;
  _mcp3462_data.cmd_tx_buf[1] = cfg1;
  _mcp3462_data.cmd_tx_buf[2] = cfg2;
  _mcp3462_data.cmd_tx_buf[3] = MCP3462_CONFIG3;

  ret = mcp3462_spi_msg(4, 0);
  if (ret < 0) {
    pr_alert(LOG_TAG "mcp3462 CONFIG1-3 write failed attempt\n");
    return ret;
  }

  // Write CONFIG0 (start ADC conversion mode)

  _mcp3462_data.cmd_tx_buf[0] =
      MCP3462_CMD_WRITE_INCR | MCP3462_REG_ADDR_CONFIG0;
  _mcp3462_data.cmd_tx_buf[1] = MCP3462_CONFIG0;

  ret = mcp3462_spi_msg(2, 0);
  if (ret < 0) {
    pr_alert(LOG_TAG "mcp3462 CONFIG0 write failed attempt\n");
    return ret;
  }

  // Check config
  // (CONFIG0 checked last to give it more time to start)

  if (!mcp3462_check_config(MCP3462_REG_ADDR_CONFIG1, cfg1) ||
      !mcp3462_check_config(MCP3462_REG_ADDR_CONFIG2, cfg2) ||
      !mcp3462_check_config(MCP3462_REG_ADDR_CONFIG3, MCP3462_CONFIG3) ||
      !mcp3462_check_config(MCP3462_REG_ADDR_CONFIG0, MCP3462_CONFIG0)) {
    pr_alert(LOG_TAG "mcp3462 config check failed attempt\n");
    return -EIO;
  }

  pr_info(LOG_TAG "mcp3462 configured (osr=%d, az=%d)\n", _mcp3462_data.osr,
          _mcp3462_data.az_mux);

  return 0;
}

static ssize_t devAttrMcp3462Osr_store(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count) {
  int ret;
  unsigned long val;

  ret = kstrtoul(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  if (val > 15) {
    return -EINVAL;
  }

  _mcp3462_data.osr = val;

  ret = mcp3462_config();
  if (ret < 0) {
    pr_err(LOG_TAG "mcp3462 failed to configure\n");
    return ret;
  }

  return count;
}

static ssize_t devAttrMcp3462AzMux_show(struct device *dev,
                                        struct device_attribute *attr,
                                        char *buf) {
  return sprintf(buf, "%d\n", _mcp3462_data.az_mux);
}

static ssize_t devAttrMcp3462AzMux_store(struct device *dev,
                                         struct device_attribute *attr,
                                         const char *buf, size_t count) {
  int ret;
  unsigned long val;

  ret = kstrtoul(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  if (val > 1) {
    return -EINVAL;
  }

  _mcp3462_data.az_mux = val;

  ret = mcp3462_config();
  if (ret < 0) {
    pr_err(LOG_TAG "mcp3462 failed to configure\n");
    return ret;
  }

  return count;
}

static ssize_t devAttrMcp3462_show(char *buf, uint8_t channel, int mult) {
  int i, ret;

  ret = _spi_lock();
  if (ret < 0) {
    return ret;
  }

  if (_mcp3462_data.curr_channel != channel) {
    _mcp3462_data.cmd_tx_buf[0] = MCP3462_CMD_WRITE_INCR | MCP3462_REG_ADDR_MUX;
    _mcp3462_data.cmd_tx_buf[1] = channel;
    ret = mcp3462_spi_msg(2, 0);
    if (ret < 0) {
      _mcp3462_data.curr_channel = 0;
      _spi_unlock();
      pr_alert(LOG_TAG "mcp3462 MUX write error\n");
      return ret;
    }

    _mcp3462_data.cmd_tx_buf[0] = MCP3462_CMD_CONV_START;
    ret = mcp3462_spi_msg(1, 0);
    if (ret < 0) {
      _mcp3462_data.curr_channel = 0;
      _spi_unlock();
      pr_alert(LOG_TAG "mcp3462 conversion restart error\n");
      return ret;
    }

    _mcp3462_data.curr_channel = channel;
  }

  _mcp3462_data.cmd_tx_buf[0] =
      MCP3462_CMD_READ_STATIC | MCP3462_REG_ADDR_ADCDATA;

  for (i = 0; i < 150; i++) {
    ret = mcp3462_spi_msg(1, 2);
    if (ret < 0) {
      _spi_unlock();
      pr_alert(LOG_TAG "mcp3462 data read error\n");
      return ret;
    }

    if ((_mcp3462_data.cmd_rx_buf[0] & MCP3462_STAT_DR_STATUS_MASK) != 0) {
      ret = -EBUSY;
      continue;
    }

    ret = 0;
    break;
  }

  if (ret < 0) {
    _spi_unlock();
    pr_alert(LOG_TAG "mcp3462 data read timeout\n");
    return ret;
  }

  ret = (int16_t)((_mcp3462_data.resp_rx_buf[0] << 8) |
                  (_mcp3462_data.resp_rx_buf[1] & 0xff));

  _spi_unlock();

  if (mult > 0) {
    ret = ret * mult /
          MCP3462_DIVISOR;  // = (adc_data * 2.4V / 32768) * (AX_max_val) / 2.0V
    if (ret < 0) {
      ret = 0;
    }
  }

  return sprintf(buf, "%d\n", ret);
}

static ssize_t devAttrAv1Mv_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH0, MCP3462_FACTOR_AV);
}

static ssize_t devAttrAv2Mv_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH1, MCP3462_FACTOR_AV);
}

static ssize_t devAttrAi1Ua_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH2, MCP3462_FACTOR_AI);
}

static ssize_t devAttrAi2Ua_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH3, MCP3462_FACTOR_AI);
}

static ssize_t devAttrAv1Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH0, 0);
}

static ssize_t devAttrAv2Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH1, 0);
}

static ssize_t devAttrAi1Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH2, 0);
}

static ssize_t devAttrAi2Raw_show(struct device *dev,
                                  struct device_attribute *attr, char *buf) {
  return devAttrMcp3462_show(buf, MCP3462_MUX_SE_CH3, 0);
}

static int _mcp3462_spi_probe(struct spi_device *spi) {
  int ret;

  _mcp3462_data.spi = spi;
  _mcp3462_data.reg = devm_regulator_get(&spi->dev, "vref");
  if (IS_ERR(_mcp3462_data.reg)) {
    pr_err(LOG_TAG "mcp3462 failed to get regulator\n");
    return PTR_ERR(_mcp3462_data.reg);
  }

  ret = regulator_enable(_mcp3462_data.reg);
  if (ret < 0) {
    pr_err(LOG_TAG "mcp3462 failed to enable regulator\n");
    return ret;
  }

  _mcp3462_data.transfer[0].tx_buf = _mcp3462_data.cmd_tx_buf;
  _mcp3462_data.transfer[0].rx_buf = _mcp3462_data.cmd_rx_buf;
  _mcp3462_data.transfer[1].tx_buf = NULL;
  _mcp3462_data.transfer[1].rx_buf = _mcp3462_data.resp_rx_buf;

  _mcp3462_data.osr = MCP3462_OSR_DEFAULT;
  _mcp3462_data.az_mux = MCP3462_AZ_MUX_DEFAULT;

  pr_info(LOG_TAG "mcp3462 probing ...\n");

  ret = mcp3462_config();
  if (ret < 0) {
    pr_err(LOG_TAG "mcp3462 failed to configure\n");
    return ret;
  }

  return 0;
}

static void _mcp3462_spi_remove(struct spi_device *spi) {
  regulator_disable(_mcp3462_data.reg);
  pr_info(LOG_TAG "mcp3462 removed\n");
}

const struct of_device_id _mcp3462_of_match[] = {
    {.compatible = "sferalabs,ionopi-v3-mcp3462"},
    {},
};
MODULE_DEVICE_TABLE(of, _mcp3462_of_match);

static const struct spi_device_id _mcp3462_ids[] = {
    {"ionopi-v3-mcp3462", 0},
    {},
};
MODULE_DEVICE_TABLE(spi, _mcp3462_ids);

static struct spi_driver _mcp3462_spi_driver = {
    .driver =
        {
            .name = "ionopi-v3-mcp3462",
            .owner = THIS_MODULE,
            .of_match_table = of_match_ptr(_mcp3462_of_match),
        },
    .probe = _mcp3462_spi_probe,
    .remove = _mcp3462_spi_remove,
    .id_table = _mcp3462_ids,
};

static int _max4896_xfer(void) {
  int ret, i;

  ret = _spi_lock();
  if (ret < 0) {
    return ret;
  }

  for (i = 0; i < 3; i++) {
    ret = spi_sync(_max4896_data.spi, &_max4896_data.msg);
    if (ret < 0) {
      pr_alert(LOG_TAG "max4896 spi xfer error\n");
      continue;
    }

    ret = 0;
    break;
  }

  _spi_unlock();

  _max4896_data.first = false;

  return ret;
}

static ssize_t devAttrMax4896_show(struct device *dev,
                                   struct device_attribute *attr, char *buf) {
  struct max4896_out *data;
  uint8_t cmd, state, b, c, s;
  int i, ret;
  if (_max4896_data.first) {
    return -EACCES;
  }

  data = (struct max4896_out *)dev_attr_get_data(attr);
  if (data == NULL) {
    return -EFAULT;
  }

  // Rewrite current cmd and read
  ret = _max4896_xfer();
  if (ret < 0) {
    return ret;
  }

  cmd = (_max4896_data.cmd >> data->shift) & data->mask;
  state = (_max4896_data.state >> data->shift) & data->mask;

  b = 0;
  for (i = (data->mask == 0b1 ? 0 : 6); i >= 0; i--) {
    c = ((cmd >> i) & 1);
    s = ((state >> i) & 1);
    if (c ^ s) {
      buf[b++] = c ? 'S' : 'F';
    } else {
      buf[b++] = c ? '1' : '0';
    }
  }
  buf[b++] = '\n';

  return b;
}

static ssize_t devAttrMax4896_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count) {
  struct max4896_out *data;
  uint8_t cmd, mask, len, i, b;
  int ret;
  data = (struct max4896_out *)dev_attr_get_data(attr);
  if (data == NULL) {
    return -EFAULT;
  }

  len = (data->mask == 0b1 ? 1 : 7);

  if (count < len) {
    return -EINVAL;
  }

  cmd = 0;
  for (i = 0; i < len; i++) {
    cmd <<= 1;
    if (buf[i] == '1') {
      cmd |= 1;
    } else if (toUpper(buf[i]) == 'F') {
      b = len - i + data->shift - 1;
      cmd |= ~(_max4896_data.cmd >> b) & 1;
    } else if (buf[i] != '0') {
      return -EINVAL;
    }
  }

  cmd <<= data->shift;
  mask = data->mask << data->shift;

  _max4896_data.cmd = (_max4896_data.cmd & ~mask) | (cmd & mask);

  ret = _max4896_xfer();
  if (ret < 0) {
    return ret;
  }

  return count;
}

static int _max4896_spi_probe(struct spi_device *spi) {
  int ret;

  _max4896_data.spi = spi;
  _max4896_data.reg = devm_regulator_get(&spi->dev, "vref");
  if (IS_ERR(_max4896_data.reg)) {
    pr_err(LOG_TAG "max4896 failed to get regulator\n");
    return PTR_ERR(_max4896_data.reg);
  }

  ret = regulator_enable(_max4896_data.reg);
  if (ret < 0) {
    pr_err(LOG_TAG "max4896 failed to enable regulator\n");
    return ret;
  }

  _max4896_data.transfer.tx_buf = &_max4896_data.cmd;
  _max4896_data.transfer.rx_buf = &_max4896_data.state;
  _max4896_data.transfer.len = 1;

  spi_message_init_with_transfers(&_max4896_data.msg, &_max4896_data.transfer,
                                  1);

  _max4896_data.first = true;

  pr_info(LOG_TAG "max4896 added\n");

  return 0;
}

static void _max4896_spi_remove(struct spi_device *spi) {
  regulator_disable(_max4896_data.reg);
  pr_info(LOG_TAG "max4896 removed\n");
}

const struct of_device_id _max4896_of_match[] = {
    {.compatible = "sferalabs,ionopi-v3-max4896"},
    {},
};
MODULE_DEVICE_TABLE(of, _max4896_of_match);

static const struct spi_device_id _max4896_ids[] = {
    {"ionopi-v3-max4896", 0},
    {},
};
MODULE_DEVICE_TABLE(spi, _max4896_ids);

static struct spi_driver _max4896_spi_driver = {
    .driver =
        {
            .name = "ionopi-v3-max4896",
            .owner = THIS_MODULE,
            .of_match_table = of_match_ptr(_max4896_of_match),
        },
    .probe = _max4896_spi_probe,
    .remove = _max4896_spi_remove,
    .id_table = _max4896_ids,
};

static void cleanup(struct platform_device *pdev) {
  struct DeviceBean *db;
  struct DeviceAttrBean *dab;
  int i, di, ai;

  spi_unregister_driver(&_mcp3462_spi_driver);

  spi_unregister_driver(&_max4896_spi_driver);

  pcf2131_i2c_unregister_driver();

  di = 0;
  while (devices[di].name != NULL) {
    if (devices[di].pDevice && !IS_ERR(devices[di].pDevice)) {
      db = &devices[di];
      ai = 0;
      while (db->devAttrBeans[ai].devAttr.attr.name != NULL) {
        dab = &db->devAttrBeans[ai];
        device_remove_file(db->pDevice, &dab->devAttr);
        ai++;
      }
    }
    device_destroy(pDeviceClass, 0);
    di++;
  }

  if (!IS_ERR(pDeviceClass)) {
    class_destroy(pDeviceClass);
  }

  wiegandDisable(&_w1);
  wiegandDisable(&_w2);

  for (i = 0; i < GPIO_SIZE; i++) {
    gpioFree(&_gpio[i]);
  }
  for (i = 0; i < DI_SIZE; i++) {
    gpioFreeDebounce(&_gpio_din[i]);
  }

  mutex_destroy(&_ionopi_data.spi_lock);
}

static int ionopi_init(struct platform_device *pdev) {
  struct DeviceBean *db;
  struct DeviceAttrBean *dab;
  int i, di, ai;

  pr_info(LOG_TAG "init\n");

  mutex_init(&_ionopi_data.spi_lock);

  gpioSetPlatformDev(pdev);

  if (spi_register_driver(&_mcp3462_spi_driver)) {
    pr_err(LOG_TAG "failed to register MCP3462 driver\n");
    goto fail;
  }

  if (spi_register_driver(&_max4896_spi_driver)) {
    pr_err(LOG_TAG "failed to register MAX4896 driver\n");
    goto fail;
  }

  if (pcf2131_i2c_register_driver()) {
    pr_err(LOG_TAG "failed to register PCF2131 driver\n");
    goto fail;
  }

  for (i = 0; i < GPIO_SIZE; i++) {
    if (gpioInit(&_gpio[i])) {
      pr_err(LOG_TAG "error setting up GPIO %s\n", _gpio[i].name);
      goto fail;
    }
  }
  for (i = 0; i < DI_SIZE; i++) {
    if (gpioInitDebounce(&_gpio_din[i])) {
      pr_err(LOG_TAG "error setting up GPIO %s\n", _gpio_din[i].gpio.name);
      goto fail;
    }
  }

  wiegandInit(&_w1);
  wiegandInit(&_w2);

  pDeviceClass = class_create("ionopi");

  if (IS_ERR(pDeviceClass)) {
    pr_err(LOG_TAG "failed to create device class\n");
    goto fail;
  }

  di = 0;
  while (devices[di].name != NULL) {
    db = &devices[di];
    db->pDevice = device_create(pDeviceClass, NULL, 0, NULL, db->name);
    if (IS_ERR(db->pDevice)) {
      pr_err(LOG_TAG "failed to create device '%s'\n", db->name);
      goto fail;
    }

    ai = 0;
    while (db->devAttrBeans[ai].devAttr.attr.name != NULL) {
      dab = &db->devAttrBeans[ai];
      if (device_create_file(db->pDevice, &dab->devAttr)) {
        pr_err(LOG_TAG "failed to create device file '%s/%s'\n", db->name,
               dab->devAttr.attr.name);
        goto fail;
      }
      ai++;
    }
    di++;
  }

  return 0;

fail:
  pr_err(LOG_TAG "init failed\n");
  cleanup(pdev);
  return -1;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static void ionopi_exit(struct platform_device *pdev) {
#else
static int ionopi_exit(struct platform_device *pdev) {
#endif
  cleanup(pdev);
  pr_info(LOG_TAG "exit\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
  return 0;
#endif
}

const struct of_device_id _ionopi_of_match[] = {
    {.compatible = "sferalabs,ionopi-v3"},
    {},
};
MODULE_DEVICE_TABLE(of, _ionopi_of_match);

static struct platform_driver ionopi_driver = {
    .probe = ionopi_init,
    .remove = ionopi_exit,
    .driver =
        {
            .name = "ionopi-v3",
            .owner = THIS_MODULE,
            .of_match_table = _ionopi_of_match,
        },
};

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sfera Labs - http://sferalabs.cc");
MODULE_DESCRIPTION("Iono Pi v3 driver module");
MODULE_VERSION("1.0");

module_platform_driver(ionopi_driver);
