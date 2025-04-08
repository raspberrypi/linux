/*
 * Based on:
 *
 * An I2C and SPI driver for the NXP PCF2127/29/31 RTC
 * Copyright 2013 Til-Technologies
 *
 * Author: Renaud Cerrato <r.cerrato@til-technologies.fr>
 *
 * Watchdog and tamper functions
 * Author: Bruno Thomsen <bruno.thomsen@gmail.com>
 *
 * PCF2131 support
 * Author: Hugo Villeneuve <hvilleneuve@dimonoff.com>
 */

#include "pcf2131.h"

#include <linux/bcd.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/watchdog.h>

#define RTC_PCF21XX_DEBUG 0

#if RTC_PCF21XX_DEBUG
#undef dev_dbg
#define dev_dbg dev_info
#endif

/* Control register 1 */
#define PCF21XX_REG_CTRL1 0x00
#define PCF21XX_BIT_CTRL1_SI BIT(0)
#define PCF21XX_BIT_CTRL1_POR_OVRD BIT(3)
#define PCF21XX_BIT_CTRL1_TSF1 BIT(4)
#define PCF21XX_BIT_CTRL1_STOP BIT(5)
/* Control register 2 */
#define PCF21XX_REG_CTRL2 0x01
#define PCF21XX_BIT_CTRL2_AIE BIT(1)
#define PCF21XX_BIT_CTRL2_TSIE BIT(2)
#define PCF21XX_BIT_CTRL2_AF BIT(4)
#define PCF21XX_BIT_CTRL2_TSF2 BIT(5)
#define PCF21XX_BIT_CTRL2_WDTF BIT(6)
#define PCF21XX_BIT_CTRL2_MSF BIT(7)
/* Control register 3 */
#define PCF21XX_REG_CTRL3 0x02
#define PCF21XX_BIT_CTRL3_BLIE BIT(0)
#define PCF21XX_BIT_CTRL3_BIE BIT(1)
#define PCF21XX_BIT_CTRL3_BLF BIT(2)
#define PCF21XX_BIT_CTRL3_BF BIT(3)
#define PCF21XX_BIT_CTRL3_BTSE BIT(4)
#define PCF21XX_CTRL3_PWRMNG_MASK GENMASK(7, 5)
/* Time and date registers */
#define PCF21XX_REG_TIME_BASE 0x03
#define PCF21XX_BIT_SC_OSF BIT(7)
/* Alarm registers */
#define PCF21XX_REG_ALARM_BASE 0x0A
#define PCF21XX_BIT_ALARM_AE BIT(7)
/* CLKOUT control register */
#define PCF21XX_REG_CLKOUT 0x0f
#define PCF21XX_BIT_CLKOUT_OTPR BIT(5)
/* Watchdog registers */
#define PCF21XX_REG_WD_CTL 0x10
#define PCF21XX_BIT_WD_CTL_TF0 BIT(0)
#define PCF21XX_BIT_WD_CTL_TF1 BIT(1)
#define PCF21XX_BIT_WD_CTL_TI_TP BIT(5)
#define PCF21XX_BIT_WD_CTL_CD0 BIT(6)
#define PCF21XX_BIT_WD_CTL_CD1 BIT(7)
#define PCF21XX_REG_WD_VAL 0x11
/* Tamper timestamp1 registers */
#define PCF21XX_REG_TS1_BASE 0x12
#define PCF21XX_BIT_TS_CTRL_TSOFF BIT(6)
#define PCF21XX_BIT_TS_CTRL_TSM BIT(7)
/*
 * RAM registers
 * PCF2127 has 512 bytes general-purpose static RAM (SRAM) that is
 * battery backed and can survive a power outage.
 * PCF2129/31 doesn't have this feature.
 */
#define PCF21XX_REG_RAM_ADDR_MSB 0x1A
#define PCF21XX_REG_RAM_WRT_CMD 0x1C
#define PCF21XX_REG_RAM_RD_CMD 0x1D

/* Watchdog timer value constants */
#define PCF21XX_WD_VAL_STOP 0
/* PCF2127/29 watchdog timer value constants */
#define PCF21XX_WD_CLOCK_HZ_X1000 1000 /* 1Hz */
#define PCF21XX_WD_MIN_HW_HEARTBEAT_MS 500
/* PCF2131 watchdog timer value constants */
#define PCF2131_WD_CLOCK_HZ_X1000 250 /* 1/4Hz */
#define PCF2131_WD_MIN_HW_HEARTBEAT_MS 4000

#define PCF21XX_WD_DEFAULT_TIMEOUT_S 60
#define IONOPI_WD_DEFAULT_OFF_TIME_S 5

/* Mask for currently enabled interrupts */
#define PCF21XX_CTRL1_IRQ_MASK (PCF21XX_BIT_CTRL1_TSF1)
#define PCF21XX_CTRL2_IRQ_MASK \
  (PCF21XX_BIT_CTRL2_AF | PCF21XX_BIT_CTRL2_WDTF | PCF21XX_BIT_CTRL2_TSF2)

#define PCF21XX_MAX_TS_SUPPORTED 4

/* Control register 4 */
#define PCF2131_REG_CTRL4 0x03
#define PCF2131_BIT_CTRL4_TSF4 BIT(4)
#define PCF2131_BIT_CTRL4_TSF3 BIT(5)
#define PCF2131_BIT_CTRL4_TSF2 BIT(6)
#define PCF2131_BIT_CTRL4_TSF1 BIT(7)
/* Control register 5 */
#define PCF2131_REG_CTRL5 0x04
#define PCF2131_BIT_CTRL5_TSIE4 BIT(4)
#define PCF2131_BIT_CTRL5_TSIE3 BIT(5)
#define PCF2131_BIT_CTRL5_TSIE2 BIT(6)
#define PCF2131_BIT_CTRL5_TSIE1 BIT(7)
/* Software reset register */
#define PCF2131_REG_SR_RESET 0x05
#define PCF2131_SR_RESET_READ_PATTERN (BIT(2) | BIT(5))
#define PCF2131_SR_RESET_CPR_CMD (PCF2131_SR_RESET_READ_PATTERN | BIT(7))
/* Time and date registers */
#define PCF2131_REG_TIME_BASE 0x07
/* Alarm registers */
#define PCF2131_REG_ALARM_BASE 0x0E
/* CLKOUT control register */
#define PCF2131_REG_CLKOUT 0x13
/* Watchdog registers */
#define PCF2131_REG_WD_CTL 0x35
#define PCF2131_REG_WD_VAL 0x36
/* Tamper timestamp1 registers */
#define PCF2131_REG_TS1_BASE 0x14
/* Tamper timestamp2 registers */
#define PCF2131_REG_TS2_BASE 0x1B
/* Tamper timestamp3 registers */
#define PCF2131_REG_TS3_BASE 0x22
/* Tamper timestamp4 registers */
#define PCF2131_REG_TS4_BASE 0x29
/* Interrupt mask registers */
#define PCF2131_REG_INT_A_MASK1 0x31
#define PCF2131_REG_INT_A_MASK2 0x32
#define PCF2131_REG_INT_B_MASK1 0x33
#define PCF2131_REG_INT_B_MASK2 0x34
#define PCF2131_BIT_INT_BLIE BIT(0)
#define PCF2131_BIT_INT_BIE BIT(1)
#define PCF2131_BIT_INT_AIE BIT(2)
#define PCF2131_BIT_INT_WD_CD BIT(3)
#define PCF2131_BIT_INT_SI BIT(4)
#define PCF2131_BIT_INT_MI BIT(5)
#define PCF2131_CTRL2_IRQ_MASK (PCF21XX_BIT_CTRL2_AF | PCF21XX_BIT_CTRL2_WDTF)
#define PCF2131_CTRL4_IRQ_MASK                                                \
  (PCF2131_BIT_CTRL4_TSF4 | PCF2131_BIT_CTRL4_TSF3 | PCF2131_BIT_CTRL4_TSF2 | \
   PCF2131_BIT_CTRL4_TSF1)

enum pcf21xx_type { PCF2131, PCF21XX_LAST_ID };

struct pcf21xx_ts_config {
  u8 reg_base; /* Base register to read timestamp values. */

  /*
   * If the TS input pin is driven to GND, an interrupt can be generated
   * (supported by all variants).
   */
  u8 gnd_detect_reg; /* Interrupt control register address. */
  u8 gnd_detect_bit; /* Interrupt bit. */

  /*
   * If the TS input pin is driven to an intermediate level between GND
   * and supply, an interrupt can be generated (optional feature depending
   * on variant).
   */
  u8 inter_detect_reg; /* Interrupt control register address. */
  u8 inter_detect_bit; /* Interrupt bit. */

  u8 ie_reg; /* Interrupt enable control register. */
  u8 ie_bit; /* Interrupt enable bit. */
};

struct pcf21xx_config {
  int type; /* IC variant */
  int max_register;
  unsigned int has_nvmem : 1;
  unsigned int has_bit_wd_ctl_cd0 : 1;
  unsigned int
      wd_val_reg_readable : 1;  /* If watchdog value register can be read. */
  unsigned int has_int_a_b : 1; /* PCF2131 supports two interrupt outputs. */
  u8 reg_time_base;             /* Time/date base register. */
  u8 regs_alarm_base;           /* Alarm function base registers. */
  u8 reg_wd_ctl;                /* Watchdog control register. */
  u8 reg_wd_val;                /* Watchdog value register. */
  u8 reg_clkout;                /* Clkout register. */
  int wdd_clock_hz_x1000;       /* Watchdog clock in Hz multiplicated by 1000 */
  int wdd_min_hw_heartbeat_ms;
  struct attribute_group attribute_group;
};

struct pcf21xx {
  struct rtc_device *rtc;
  struct watchdog_device wdd;
  struct regmap *regmap;
  const struct pcf21xx_config *cfg;
  bool irq_enabled;
  int wdd_off_time_sec;
};

static struct pcf21xx *_instance;

static int pcf21xx_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm);
static int pcf21xx_wdt_ping(struct watchdog_device *wdd);
static int pcf21xx_wdt_start(struct watchdog_device *wdd);
static int pcf21xx_wdt_stop(struct watchdog_device *wdd);

/*
 * In the routines that deal directly with the hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch.
 */
static int pcf21xx_rtc_read_time_opt_check(struct device *dev,
                                           struct rtc_time *tm,
                                           bool check_osf) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev);
  unsigned char buf[7];
  int ret;

  /*
   * Avoid reading CTRL2 register as it causes WD_VAL register
   * value to reset to 0 which means watchdog is stopped.
   */
  ret = regmap_bulk_read(pcf21xx->regmap, pcf21xx->cfg->reg_time_base, buf,
                         sizeof(buf));
  if (ret) {
    dev_err(dev, "%s: read error\n", __func__);
    return ret;
  }

  /* Clock integrity is not guaranteed when OSF flag is set. */
  if (check_osf && (buf[0] & PCF21XX_BIT_SC_OSF)) {
    /*
     * no need clear the flag here,
     * it will be cleared once the new date is saved
     */
    dev_warn(dev, "oscillator stop detected, date/time is not reliable\n");
    return -EINVAL;
  }

  dev_dbg(dev,
          "%s: raw data is sec=%02x, min=%02x, hr=%02x, "
          "mday=%02x, wday=%02x, mon=%02x, year=%02x\n",
          __func__, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

  tm->tm_sec = bcd2bin(buf[0] & 0x7F);
  tm->tm_min = bcd2bin(buf[1] & 0x7F);
  tm->tm_hour = bcd2bin(buf[2] & 0x3F);
  tm->tm_mday = bcd2bin(buf[3] & 0x3F);
  tm->tm_wday = buf[4] & 0x07;
  tm->tm_mon = bcd2bin(buf[5] & 0x1F) - 1;
  tm->tm_year = bcd2bin(buf[6]);
  tm->tm_year += 100;

  dev_dbg(dev,
          "%s: tm is secs=%d, mins=%d, hours=%d, "
          "mday=%d, mon=%d, year=%d, wday=%d\n",
          __func__, tm->tm_sec, tm->tm_min, tm->tm_hour, tm->tm_mday,
          tm->tm_mon, tm->tm_year, tm->tm_wday);

  return 0;
}

static int pcf21xx_rtc_read_time(struct device *dev, struct rtc_time *tm) {
  return pcf21xx_rtc_read_time_opt_check(dev, tm, true);
}

static int pcf21xx_rtc_set_time(struct device *dev, struct rtc_time *tm) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev);
  unsigned char buf[7];
  int i = 0, err;
  bool wd_active;

  dev_dbg(dev,
          "%s: secs=%d, mins=%d, hours=%d, "
          "mday=%d, mon=%d, year=%d, wday=%d\n",
          __func__, tm->tm_sec, tm->tm_min, tm->tm_hour, tm->tm_mday,
          tm->tm_mon, tm->tm_year, tm->tm_wday);

  /* hours, minutes and seconds */
  buf[i++] = bin2bcd(tm->tm_sec); /* this will also clear OSF flag */
  buf[i++] = bin2bcd(tm->tm_min);
  buf[i++] = bin2bcd(tm->tm_hour);
  buf[i++] = bin2bcd(tm->tm_mday);
  buf[i++] = tm->tm_wday & 0x07;

  /* month, 1 - 12 */
  buf[i++] = bin2bcd(tm->tm_mon + 1);

  /* year */
  buf[i++] = bin2bcd(tm->tm_year - 100);

  /* Write access to time registers:
   * PCF2127/29: no special action required.
   * PCF2131:    requires setting the STOP and CPR bits. STOP bit needs to
   *             be cleared after time registers are updated.
   */
  if (pcf21xx->cfg->type == PCF2131) {
    err = regmap_update_bits(pcf21xx->regmap, PCF21XX_REG_CTRL1,
                             PCF21XX_BIT_CTRL1_STOP, PCF21XX_BIT_CTRL1_STOP);
    if (err) {
      dev_dbg(dev, "setting STOP bit failed\n");
      return err;
    }

    err = regmap_write(pcf21xx->regmap, PCF2131_REG_SR_RESET,
                       PCF2131_SR_RESET_CPR_CMD);
    if (err) {
      dev_dbg(dev, "sending CPR cmd failed\n");
      return err;
    }
  }

  wd_active = watchdog_active(&pcf21xx->wdd);

  if (wd_active) {
    err = pcf21xx_wdt_stop(&pcf21xx->wdd);
    if (err) {
      dev_dbg(dev, "watchdog stop failed\n");
      return err;
    }
  }

  /* write time register's data */
  err = regmap_bulk_write(pcf21xx->regmap, pcf21xx->cfg->reg_time_base, buf, i);
  if (err) {
    dev_dbg(dev, "%s: err=%d", __func__, err);
    return err;
  }

  if (pcf21xx->cfg->type == PCF2131) {
    /* Clear STOP bit (PCF2131 only) after write is completed. */
    err = regmap_update_bits(pcf21xx->regmap, PCF21XX_REG_CTRL1,
                             PCF21XX_BIT_CTRL1_STOP, 0);
    if (err) {
      dev_dbg(dev, "clearing STOP bit failed\n");
      return err;
    }
  }

  if (wd_active) {
    err = pcf21xx_wdt_start(&pcf21xx->wdd);
    if (err) {
      dev_dbg(dev, "watchdog restart failed\n");
      return err;
    }
  }

  return 0;
}

static int pcf21xx_rtc_ioctl(struct device *dev, unsigned int cmd,
                             unsigned long arg) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev);
  int val, touser = 0;
  int ret;

  switch (cmd) {
    case RTC_VL_READ:
      ret = regmap_read(pcf21xx->regmap, PCF21XX_REG_CTRL3, &val);
      if (ret) return ret;

      if (val & PCF21XX_BIT_CTRL3_BLF) touser |= RTC_VL_BACKUP_LOW;

      if (val & PCF21XX_BIT_CTRL3_BF) touser |= RTC_VL_BACKUP_SWITCH;

      return put_user(touser, (unsigned int __user *)arg);

    case RTC_VL_CLR:
      return regmap_update_bits(pcf21xx->regmap, PCF21XX_REG_CTRL3,
                                PCF21XX_BIT_CTRL3_BF, 0);

    default:
      return -ENOIOCTLCMD;
  }
}

/* watchdog driver */

static int pcf21xx_wdt_update(struct watchdog_device *wdd) {
  int wd_val;
  struct pcf21xx *pcf21xx = watchdog_get_drvdata(wdd);

  /*
   * Compute counter value of WATCHDG_TIM_VAL to obtain desired period
   * in seconds, depending on the source clock frequency.
   */
  wd_val = ((wdd->timeout * pcf21xx->cfg->wdd_clock_hz_x1000) / 1000) + 1;

  return regmap_write(pcf21xx->regmap, pcf21xx->cfg->reg_wd_val, wd_val);
}

static int pcf21xx_wdt_ping(struct watchdog_device *wdd) {
  int ret;
  time64_t ts;
  struct rtc_wkalrm alrm;
  struct pcf21xx *pcf21xx = watchdog_get_drvdata(wdd);

  dev_dbg(wdd->parent, "%s: active=%d\n", __func__, watchdog_active(wdd));

  ret = pcf21xx_rtc_read_time_opt_check(wdd->parent, &alrm.time, false);
  if (ret) return ret;

  ts = rtc_tm_to_time64(&alrm.time);
  ts += wdd->timeout + pcf21xx->wdd_off_time_sec;
  rtc_time64_to_tm(ts, &alrm.time);
  alrm.enabled = true;

  ret = pcf21xx_rtc_set_alarm(wdd->parent, &alrm);
  if (ret) return ret;

  return pcf21xx_wdt_update(wdd);
}

/*
 * Restart watchdog timer if feature is active.
 *
 * Note: Reading CTRL2 register causes watchdog to stop which is unfortunate,
 * since register also contain control/status flags for other features.
 * Always call this function after reading CTRL2 register.
 *
 * This does not seem to apply for PCF2131
 */
static int pcf21xx_wdt_active_ping(struct pcf21xx *pcf21xx) {
  int ret = 0;

  if (pcf21xx->cfg->type == PCF2131) {
    return ret;
  }

  if (watchdog_active(&pcf21xx->wdd)) {
    ret = pcf21xx_wdt_update(&pcf21xx->wdd);
    if (ret)
      dev_err(pcf21xx->wdd.parent, "%s: watchdog restart failed, ret=%d\n",
              __func__, ret);
  }

  return ret;
}

static int pcf21xx_wdt_start(struct watchdog_device *wdd) {
  struct pcf21xx *pcf21xx = watchdog_get_drvdata(wdd);
  int ret = 0;

  dev_dbg(wdd->parent, "%s\n", __func__);

  ret = regmap_clear_bits(pcf21xx->regmap, PCF21XX_REG_CTRL1,
                          PCF21XX_BIT_CTRL1_SI);
  if (ret < 0) return ret;

  ret = regmap_set_bits(pcf21xx->regmap, pcf21xx->cfg->reg_wd_ctl,
                        PCF21XX_BIT_WD_CTL_CD1);
  if (ret < 0) return ret;

  return pcf21xx_wdt_ping(wdd);
}

static int pcf21xx_wdt_stop(struct watchdog_device *wdd) {
  struct pcf21xx *pcf21xx = watchdog_get_drvdata(wdd);
  int ret = 0;

  dev_dbg(wdd->parent, "%s\n", __func__);

  ret = regmap_clear_bits(pcf21xx->regmap, pcf21xx->cfg->reg_wd_ctl,
                          PCF21XX_BIT_WD_CTL_CD1);
  if (ret < 0) return ret;

  ret =
      regmap_set_bits(pcf21xx->regmap, PCF21XX_REG_CTRL1, PCF21XX_BIT_CTRL1_SI);
  if (ret < 0) return ret;

  ret = regmap_write(pcf21xx->regmap, pcf21xx->cfg->reg_wd_val,
                     PCF21XX_WD_VAL_STOP);
  if (ret) return ret;

  clear_bit(WDOG_ACTIVE, &pcf21xx->wdd.status);
  return ret;
}

static int pcf21xx_wdt_set_timeout(struct watchdog_device *wdd,
                                   unsigned int new_timeout) {
  dev_dbg(wdd->parent, "new watchdog timeout: %is (old: %is)\n", new_timeout,
          wdd->timeout);

  wdd->timeout = new_timeout;

  if (watchdog_active(wdd)) {
    return pcf21xx_wdt_ping(wdd);
  }
  return 0;
}

static const struct watchdog_info pcf21xx_wdt_info = {
    .identity = "Iono Pi v3 Watchdog",
    .options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT,
};

static const struct watchdog_ops pcf21xx_watchdog_ops = {
    .owner = THIS_MODULE,
    .start = pcf21xx_wdt_start,
    .stop = pcf21xx_wdt_stop,
    .ping = pcf21xx_wdt_ping,
    .set_timeout = pcf21xx_wdt_set_timeout,
};

/*
 * Compute watchdog period, t, in seconds, from the WATCHDG_TIM_VAL register
 * value, n, and the clock frequency, f1000, in Hz x 1000.
 *
 * The PCF2127/29 datasheet gives t as:
 *   t = n / f
 * The PCF2131 datasheet gives t as:
 *   t = (n - 1) / f
 * For both variants, the watchdog is triggered when the WATCHDG_TIM_VAL reaches
 * the value 1, and not zero. Consequently, the equation from the PCF2131
 * datasheet seems to be the correct one for both variants.
 */
static int pcf21xx_watchdog_get_period(int n, int f1000) {
  return (1000 * (n - 1)) / f1000;
}

static int pcf21xx_watchdog_init(struct device *dev, struct pcf21xx *pcf21xx) {
  int ret;
  unsigned int wd_ctl, ctrl2;
  bool wd_active;

  if (!IS_ENABLED(CONFIG_WATCHDOG) ||
      !device_property_read_bool(dev, "reset-source"))
    return 0;

  pcf21xx->wdd.parent = dev;
  pcf21xx->wdd.info = &pcf21xx_wdt_info;
  pcf21xx->wdd.ops = &pcf21xx_watchdog_ops;

  pcf21xx->wdd.min_timeout =
      1 + pcf21xx_watchdog_get_period(2, pcf21xx->cfg->wdd_clock_hz_x1000);
  pcf21xx->wdd.max_timeout =
      pcf21xx_watchdog_get_period(255, pcf21xx->cfg->wdd_clock_hz_x1000);
  pcf21xx->wdd.timeout = PCF21XX_WD_DEFAULT_TIMEOUT_S;

  dev_dbg(dev, "%s clock=%dHz/1000 min_timeout=%d max_timeout=%d\n", __func__,
          pcf21xx->cfg->wdd_clock_hz_x1000, pcf21xx->wdd.min_timeout,
          pcf21xx->wdd.max_timeout);

  pcf21xx->wdd.min_hw_heartbeat_ms = pcf21xx->cfg->wdd_min_hw_heartbeat_ms;
  pcf21xx->wdd.status = WATCHDOG_NOWAYOUT_INIT_STATUS;

  pcf21xx->wdd_off_time_sec = IONOPI_WD_DEFAULT_OFF_TIME_S;

  watchdog_set_drvdata(&pcf21xx->wdd, pcf21xx);

  /* 1/4 Hz watchdog timer and permanent active signal when MSF is set */
  ret = regmap_update_bits(pcf21xx->regmap, pcf21xx->cfg->reg_wd_ctl,
                           PCF21XX_BIT_WD_CTL_TI_TP | PCF21XX_BIT_WD_CTL_TF1 |
                               PCF21XX_BIT_WD_CTL_TF0,
                           PCF21XX_BIT_WD_CTL_TF1);
  if (ret) {
    dev_err(dev, "%s: watchdog config (wd_ctl) failed\n", __func__);
    return ret;
  }

  /* Check if watchdog is active */
  ret = regmap_read(pcf21xx->regmap, pcf21xx->cfg->reg_wd_ctl, &wd_ctl);
  if (ret) return ret;

  ret = regmap_read(pcf21xx->regmap, PCF21XX_REG_CTRL2, &ctrl2);
  if (ret) return ret;

  if ((wd_ctl & PCF21XX_BIT_WD_CTL_CD1) && !(ctrl2 & PCF21XX_BIT_CTRL2_WDTF)) {
    set_bit(WDOG_HW_RUNNING, &pcf21xx->wdd.status);
    ret = pcf21xx_wdt_start(&pcf21xx->wdd);
    wd_active = true;
  } else {
    ret = pcf21xx_wdt_stop(&pcf21xx->wdd);
    wd_active = false;
  }
  if (ret) return ret;

  if (wd_active)
    set_bit(WDOG_ACTIVE, &pcf21xx->wdd.status);
  else
    clear_bit(WDOG_ACTIVE, &pcf21xx->wdd.status);

  return devm_watchdog_register_device(dev, &pcf21xx->wdd);
}

/* Alarm */

static int pcf21xx_rtc_alarm_irq_enable(struct device *dev, u32 enable) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev);
  int ret;

  ret = regmap_update_bits(pcf21xx->regmap, PCF21XX_REG_CTRL2,
                           PCF21XX_BIT_CTRL2_AIE,
                           enable ? PCF21XX_BIT_CTRL2_AIE : 0);
  if (ret) return ret;

  return pcf21xx_wdt_active_ping(pcf21xx);
}

static int pcf21xx_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev);
  uint8_t buf[5];
  int ret;

  ret = regmap_clear_bits(
      pcf21xx->regmap, PCF21XX_REG_CTRL2,
      PCF21XX_BIT_CTRL2_AF | PCF21XX_BIT_CTRL2_WDTF | PCF21XX_BIT_CTRL2_MSF);
  if (ret < 0) return ret;

  ret = pcf21xx_wdt_active_ping(pcf21xx);
  if (ret) return ret;

  buf[0] = bin2bcd(alrm->time.tm_sec);
  buf[1] = bin2bcd(alrm->time.tm_min);
  buf[2] = bin2bcd(alrm->time.tm_hour);
  buf[3] = PCF21XX_BIT_ALARM_AE; /* Do not match on day */
  buf[4] = PCF21XX_BIT_ALARM_AE; /* Do not match on week day */

  ret = regmap_bulk_write(pcf21xx->regmap, pcf21xx->cfg->regs_alarm_base, buf,
                          sizeof(buf));
  if (ret) return ret;

  return pcf21xx_rtc_alarm_irq_enable(dev, alrm->enabled);
}

static const struct rtc_class_ops pcf21xx_rtc_ops = {
    .ioctl = pcf21xx_rtc_ioctl,
    .read_time = pcf21xx_rtc_read_time,
    .set_time = pcf21xx_rtc_set_time,
};

/* sysfs interface */

static ssize_t battery_low_show(struct device *dev,
                                struct device_attribute *attr, char *buf) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev->parent);
  unsigned int ctrl3;
  int ret;

  ret = regmap_read(pcf21xx->regmap, PCF21XX_REG_CTRL3, &ctrl3);
  if (ret) return ret;

  return sprintf(buf, "%d\n", (ctrl3 & PCF21XX_BIT_CTRL3_BLF) ? 1 : 0);
};

static DEVICE_ATTR_RO(battery_low);

static struct attribute *pcf2131_attrs[] = {
    &dev_attr_battery_low.attr,
    NULL,
};

static struct pcf21xx_config pcf21xx_cfg[] = {
    [PCF2131] =
        {
            .type = PCF2131,
            .max_register = 0x36,
            .has_nvmem = 0,
            .has_bit_wd_ctl_cd0 = 0,
            .wd_val_reg_readable = 0,
            .has_int_a_b = 1,
            .reg_time_base = PCF2131_REG_TIME_BASE,
            .regs_alarm_base = PCF2131_REG_ALARM_BASE,
            .reg_wd_ctl = PCF2131_REG_WD_CTL,
            .reg_wd_val = PCF2131_REG_WD_VAL,
            .reg_clkout = PCF2131_REG_CLKOUT,
            .wdd_clock_hz_x1000 = PCF2131_WD_CLOCK_HZ_X1000,
            .wdd_min_hw_heartbeat_ms = PCF2131_WD_MIN_HW_HEARTBEAT_MS,
            .attribute_group =
                {
                    .attrs = pcf2131_attrs,
                },
        },
};

/*
 * Route alarm and seconds interrupts to INT A pin
 * and watchdog interrupt to INT B
 */
static int pcf21xx_configure_interrupt_pins(struct device *dev) {
  struct pcf21xx *pcf21xx = dev_get_drvdata(dev);
  int ret;

  /* Mask bits need to be cleared to enable corresponding
   * interrupt source.
   */
  ret = regmap_write(pcf21xx->regmap, PCF2131_REG_INT_A_MASK1,
                     ~(PCF2131_BIT_INT_AIE | PCF2131_BIT_INT_SI) & 0x3f);
  if (ret) return ret;

  ret = regmap_write(pcf21xx->regmap, PCF2131_REG_INT_A_MASK2, 0x0f);
  if (ret) return ret;

  ret = regmap_write(pcf21xx->regmap, PCF2131_REG_INT_B_MASK1,
                     ~PCF2131_BIT_INT_WD_CD & 0x3f);
  if (ret) return ret;

  ret = regmap_write(pcf21xx->regmap, PCF2131_REG_INT_B_MASK2, 0x0f);
  if (ret) return ret;

  return ret;
}

static int pcf21xx_probe(struct device *dev, struct regmap *regmap,
                         int alarm_irq, const struct pcf21xx_config *config) {
  struct pcf21xx *pcf21xx;
  int ret = 0;
  unsigned int val;

  dev_dbg(dev, "%s\n", __func__);

  pcf21xx = devm_kzalloc(dev, sizeof(*pcf21xx), GFP_KERNEL);
  if (!pcf21xx) return -ENOMEM;

  pcf21xx->regmap = regmap;
  pcf21xx->cfg = config;

  dev_set_drvdata(dev, pcf21xx);

  pcf21xx->rtc = devm_rtc_allocate_device(dev);
  if (IS_ERR(pcf21xx->rtc)) return PTR_ERR(pcf21xx->rtc);

  pcf21xx->rtc->ops = &pcf21xx_rtc_ops;
  pcf21xx->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
  pcf21xx->rtc->range_max = RTC_TIMESTAMP_END_2099;
  pcf21xx->rtc->set_start_time = true; /* Sets actual start to 1970 */

  clear_bit(RTC_FEATURE_ALARM, pcf21xx->rtc->features);

  if (pcf21xx->cfg->has_int_a_b) {
    /* Configure int A/B pins, independently of alarm_irq. */
    ret = pcf21xx_configure_interrupt_pins(dev);
    if (ret) {
      dev_err(dev, "failed to configure interrupt pins\n");
      return ret;
    }
  }

  /*
   * The "Power-On Reset Override" facility prevents the RTC to do a reset
   * after power on. For normal operation the PORO must be disabled.
   */
  ret = regmap_clear_bits(pcf21xx->regmap, PCF21XX_REG_CTRL1,
                          PCF21XX_BIT_CTRL1_POR_OVRD);
  if (ret < 0) return ret;

  /* Make sure PWRMNG[2:0] is set to 000b. This is the default for
   * PCF2127/29, but not for PCF2131 (default of 111b).
   *
   * PWRMNG[2:0]  = 000b:
   *   battery switch-over function is enabled in standard mode;
   *   battery low detection function is enabled
   */
  ret = regmap_clear_bits(pcf21xx->regmap, PCF21XX_REG_CTRL3,
                          PCF21XX_CTRL3_PWRMNG_MASK);
  if (ret < 0) {
    dev_err(dev, "PWRMNG config failed\n");
    return ret;
  }

  ret = regmap_read(pcf21xx->regmap, pcf21xx->cfg->reg_clkout, &val);
  if (ret < 0) return ret;

  if (!(val & PCF21XX_BIT_CLKOUT_OTPR)) {
    ret = regmap_set_bits(pcf21xx->regmap, pcf21xx->cfg->reg_clkout,
                          PCF21XX_BIT_CLKOUT_OTPR);
    if (ret < 0) return ret;

    msleep(100);
  }

  pcf21xx_watchdog_init(dev, pcf21xx);

  /*
   * Disable battery low/switch-over timestamp and interrupts.
   * Clear battery interrupt flags which can block new trigger events.
   * Note: This is the default chip behaviour but added to ensure
   * correct tamper timestamp and interrupt function.
   */
  ret = regmap_update_bits(
      pcf21xx->regmap, PCF21XX_REG_CTRL3,
      PCF21XX_BIT_CTRL3_BTSE | PCF21XX_BIT_CTRL3_BIE | PCF21XX_BIT_CTRL3_BLIE,
      0);
  if (ret) {
    dev_err(dev, "%s: interrupt config (ctrl3) failed\n", __func__);
    return ret;
  }

  ret = rtc_add_group(pcf21xx->rtc, &pcf21xx->cfg->attribute_group);
  if (ret) {
    dev_err(dev, "%s: tamper sysfs registering failed\n", __func__);
    return ret;
  }

  ret = devm_rtc_register_device(pcf21xx->rtc);
  if (ret) return ret;

  _instance = pcf21xx;

  return 0;
}

static const struct of_device_id pcf21xx_of_match[] = {
    {
        .compatible = "sferalabs,ionopi-v3-pcf2131",
        .data = &pcf21xx_cfg[PCF2131],
    },
    {},
};
MODULE_DEVICE_TABLE(of, pcf21xx_of_match);

static int pcf21xx_i2c_write(void *context, const void *data, size_t count) {
  struct device *dev = context;
  struct i2c_client *client = to_i2c_client(dev);
  int ret;

  ret = i2c_master_send(client, data, count);
  if (ret != count) return ret < 0 ? ret : -EIO;

  return 0;
}

static int pcf21xx_i2c_gather_write(void *context, const void *reg,
                                    size_t reg_size, const void *val,
                                    size_t val_size) {
  struct device *dev = context;
  struct i2c_client *client = to_i2c_client(dev);
  int ret;
  void *buf;

  if (WARN_ON(reg_size != 1)) return -EINVAL;

  buf = kmalloc(val_size + 1, GFP_KERNEL);
  if (!buf) return -ENOMEM;

  memcpy(buf, reg, 1);
  memcpy(buf + 1, val, val_size);

  ret = i2c_master_send(client, buf, val_size + 1);

  kfree(buf);

  if (ret != val_size + 1) return ret < 0 ? ret : -EIO;

  return 0;
}

static int pcf21xx_i2c_read(void *context, const void *reg, size_t reg_size,
                            void *val, size_t val_size) {
  struct device *dev = context;
  struct i2c_client *client = to_i2c_client(dev);
  int ret;

  if (WARN_ON(reg_size != 1)) return -EINVAL;

  ret = i2c_master_send(client, reg, 1);
  if (ret != 1) return ret < 0 ? ret : -EIO;

  ret = i2c_master_recv(client, val, val_size);
  if (ret != val_size) return ret < 0 ? ret : -EIO;

  return 0;
}

/*
 * The reason we need this custom regmap_bus instead of using regmap_init_i2c()
 * is that the STOP condition is required between set register address and
 * read register data when reading from registers.
 */
static const struct regmap_bus pcf21xx_i2c_regmap = {
    .write = pcf21xx_i2c_write,
    .gather_write = pcf21xx_i2c_gather_write,
    .read = pcf21xx_i2c_read,
};

static const struct i2c_device_id pcf21xx_i2c_id[] = {
    {"ionopi-v3-pcf2131", PCF2131},
    {},
};
MODULE_DEVICE_TABLE(i2c, pcf21xx_i2c_id);

static int pcf21xx_i2c_probe(struct i2c_client *client) {
  struct regmap *regmap;
  static struct regmap_config config = {
      .reg_bits = 8,
      .val_bits = 8,
  };
  const struct pcf21xx_config *variant;

  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) return -ENODEV;

  if (client->dev.of_node) {
    variant = of_device_get_match_data(&client->dev);
    if (!variant) return -ENODEV;
  } else {
    enum pcf21xx_type type = i2c_match_id(pcf21xx_i2c_id, client)->driver_data;

    if (type >= PCF21XX_LAST_ID) return -ENODEV;
    variant = &pcf21xx_cfg[type];
  }

  config.max_register = variant->max_register,

  regmap = devm_regmap_init(&client->dev, &pcf21xx_i2c_regmap, &client->dev,
                            &config);
  if (IS_ERR(regmap)) {
    dev_err(&client->dev, "%s: regmap allocation failed: %ld\n", __func__,
            PTR_ERR(regmap));
    return PTR_ERR(regmap);
  }

  return pcf21xx_probe(&client->dev, regmap, client->irq, variant);
}

static struct i2c_driver pcf21xx_i2c_driver = {
    .driver =
        {
            .name = "ionopi-v3-pcf2131",
            .of_match_table = of_match_ptr(pcf21xx_of_match),
        },
    .probe = pcf21xx_i2c_probe,
    .id_table = pcf21xx_i2c_id,
};

int pcf2131_i2c_register_driver(void) {
  return i2c_add_driver(&pcf21xx_i2c_driver);
}

void pcf2131_i2c_unregister_driver(void) {
  i2c_del_driver(&pcf21xx_i2c_driver);
}

ssize_t devAttrWatchdogEnabled_show(struct device *dev,
                                    struct device_attribute *attr, char *buf) {
  if (_instance == NULL) {
    return -ENODEV;
  }
  return sprintf(buf, "%d\n", watchdog_active(&_instance->wdd));
}

ssize_t devAttrWatchdogEnabled_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count) {
  int ret;
  bool enable;
  if (_instance == NULL) {
    return -ENODEV;
  }

  if (buf[0] == '0') {
    enable = false;
  } else if (buf[0] == '1') {
    enable = true;
  } else {
    return -EINVAL;
  }

  if (enable) {
    ret = pcf21xx_wdt_start(&_instance->wdd);
  } else {
    ret = pcf21xx_wdt_stop(&_instance->wdd);
  }

  if (ret) return ret;

  if (enable) {
    set_bit(WDOG_ACTIVE, &_instance->wdd.status);
  } else {
    clear_bit(WDOG_ACTIVE, &_instance->wdd.status);
  }

  return count;
}

ssize_t devAttrWatchdogHeartbeat_store(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count) {
  int ret;
  if (_instance == NULL) {
    return -ENODEV;
  }

  if (!watchdog_active(&_instance->wdd)) {
    return -EPERM;
  }

  ret = pcf21xx_wdt_ping(&_instance->wdd);
  if (ret) {
    return ret;
  }

  return count;
}

ssize_t devAttrWatchdogTimeout_show(struct device *dev,
                                    struct device_attribute *attr, char *buf) {
  if (_instance == NULL) {
    return -ENODEV;
  }
  return sprintf(buf, "%d\n", _instance->wdd.timeout);
}

ssize_t devAttrWatchdogTimeout_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count) {
  int ret;
  unsigned long val;
  if (_instance == NULL) {
    return -ENODEV;
  }

  ret = kstrtoul(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  if (val < _instance->wdd.min_timeout || val > _instance->wdd.max_timeout) {
    return -EINVAL;
  }

  ret = pcf21xx_wdt_set_timeout(&_instance->wdd, val);
  if (ret) {
    return ret;
  }

  return count;
}

ssize_t devAttrWatchdogOffTime_show(struct device *dev,
                                    struct device_attribute *attr, char *buf) {
  if (_instance == NULL) {
    return -ENODEV;
  }
  return sprintf(buf, "%d\n", _instance->wdd_off_time_sec);
}

ssize_t devAttrWatchdogOffTime_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count) {
  int ret;
  unsigned long val;
  if (_instance == NULL) {
    return -ENODEV;
  }

  ret = kstrtoul(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  if (val < 5 || val > (86400 - _instance->wdd.timeout - 1)) {
    return -EINVAL;
  }

  _instance->wdd_off_time_sec = val;

  if (watchdog_active(&_instance->wdd)) {
    ret = pcf21xx_wdt_ping(&_instance->wdd);
    if (ret) {
      return ret;
    }
  }

  return count;
}
