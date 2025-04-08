#include "wiegand.h"

#include <linux/interrupt.h>

#include "../misc/misc.h"

#define WIEGAND_MAX_BITS 64

int wCount = 0;

static enum hrtimer_restart wiegandTimerHandler(struct hrtimer *tmr) {
  struct WiegandBean *w;
  w = container_of(tmr, struct WiegandBean, timer);
  if (w->notifKn != NULL) {
    sysfs_notify_dirent(w->notifKn);
  }
  return HRTIMER_NORESTART;
}

void wiegandInit(struct WiegandBean *w) {
  w->d0.irqRequested = false;
  w->d1.irqRequested = false;
  w->enabled = false;
  w->pulseWidthMin_usec = 10;
  w->pulseWidthMax_usec = 150;
  w->pulseIntervalMin_usec = 1200;
  w->pulseIntervalMax_usec = 2700;
  w->noise = 0;
  w->id = '0' + (++wCount);
  hrtimer_init(&w->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  w->timer.function = &wiegandTimerHandler;
}

static void wiegandReset(struct WiegandBean *w) {
  w->enabled = true;
  w->data = 0;
  w->bitCount = 0;
  w->activeLine = NULL;
  w->d0.wasLow = false;
  w->d1.wasLow = false;
}

void wiegandDisable(struct WiegandBean *w) {
  if (w->enabled) {
    hrtimer_cancel(&w->timer);

    gpioFree(w->d0.gpio);
    gpioFree(w->d1.gpio);

    if (w->d0.irqRequested) {
      free_irq(w->d0.irq, w);
      w->d0.irqRequested = false;
    }

    if (w->d1.irqRequested) {
      free_irq(w->d1.irq, w);
      w->d1.irqRequested = false;
    }

    w->d0.gpio->owner = NULL;
    w->d1.gpio->owner = NULL;
    w->enabled = false;
  }
}

static irqreturn_t wiegandDataIrqHandler(int irq, void *dev) {
  bool isLow;
  struct timespec64 now;
  unsigned long long diff;
  struct WiegandBean *w;
  struct WiegandLine *l;

  w = (struct WiegandBean *)dev;
  l = NULL;

  if (w->enabled) {
    if (irq == w->d0.irq) {
      l = &w->d0;
    } else if (irq == w->d1.irq) {
      l = &w->d1;
    }
  }

  if (l == NULL) {
    return IRQ_HANDLED;
  }

  isLow = gpioGetVal(l->gpio) == 0;

  ktime_get_raw_ts64(&now);

  if (l->wasLow == isLow) {
    // got the interrupt but didn't change state. Maybe a fast pulse
    if (w->noise == 0) {
      w->noise = 10;
    }
    return IRQ_HANDLED;
  }

  l->wasLow = isLow;

  if (isLow) {
    if (w->bitCount != 0) {
      diff = diff_usec((struct timespec64 *)&(w->lastBitTs), &now);

      if (diff < w->pulseIntervalMin_usec) {
        // pulse too early
        w->noise = 11;
        goto noise;
      }

      if (diff > w->pulseIntervalMax_usec) {
        w->data = 0;
        w->bitCount = 0;
      }
    }

    if (w->activeLine != NULL) {
      // there's movement on both lines
      w->noise = 12;
      goto noise;
    }

    w->activeLine = l;

    w->lastBitTs.tv_sec = now.tv_sec;
    w->lastBitTs.tv_nsec = now.tv_nsec;

  } else {
    if (w->activeLine != l) {
      // there's movement on both lines or previous noise
      w->noise = 13;
      goto noise;
    }

    w->activeLine = NULL;

    if (w->bitCount >= WIEGAND_MAX_BITS) {
      return IRQ_HANDLED;
    }

    diff = diff_usec((struct timespec64 *)&(w->lastBitTs), &now);
    if (diff < w->pulseWidthMin_usec) {
      // pulse too short
      w->noise = 14;
      goto noise;
    }
    if (diff > w->pulseWidthMax_usec) {
      // pulse too long
      w->noise = 15;
      goto noise;
    }

    w->data <<= 1;
    if (l == &w->d1) {
      w->data |= 1;
    }
    w->bitCount++;

    hrtimer_cancel(&w->timer);
    hrtimer_start(&w->timer,
                  ktime_set(0, (w->pulseIntervalMax_usec - diff) * 1000),
                  HRTIMER_MODE_REL);
  }

  return IRQ_HANDLED;

noise:
  wiegandReset(w);
  return IRQ_HANDLED;
}

ssize_t devAttrWiegandEnabled_show(struct device *dev,
                                   struct device_attribute *attr, char *buf) {
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }
  return sprintf(buf, w->enabled ? "1\n" : "0\n");
}

ssize_t devAttrWiegandEnabled_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count) {
  struct WiegandBean *w;
  bool enable;
  int result = 0;

  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  if (buf[0] == '0') {
    enable = false;
  } else if (buf[0] == '1') {
    enable = true;
  } else {
    return -EINVAL;
  }

  if (enable && !w->enabled) {
    if (w->d0.gpio->owner != NULL || w->d1.gpio->owner != NULL) {
      return -EBUSY;
    }
    w->d0.gpio->owner = w;
    w->d1.gpio->owner = w;

    w->d0.gpio->flags = GPIOD_IN;
    w->d1.gpio->flags = GPIOD_IN;

    result = gpioInit(w->d0.gpio);
    if (!result) {
      result = gpioInit(w->d1.gpio);
    }

    if (result) {
      pr_err("error setting up wiegand GPIOs\n");
      enable = false;
    } else {
      gpiod_set_debounce(w->d0.gpio->desc, 0);
      gpiod_set_debounce(w->d1.gpio->desc, 0);

      w->d0.irq = gpiod_to_irq(w->d0.gpio->desc);
      w->d1.irq = gpiod_to_irq(w->d1.gpio->desc);

      result = request_irq(w->d0.irq, wiegandDataIrqHandler,
                           IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                           w->d0.gpio->name, w);

      if (result) {
        pr_err("error registering wiegand D0 irq handler\n");
        enable = false;
      } else {
        w->d0.irqRequested = true;

        result = request_irq(w->d1.irq, wiegandDataIrqHandler,
                             IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                             w->d1.gpio->name, w);

        if (result) {
          pr_err("error registering wiegand D1 irq handler\n");
          enable = false;
        } else {
          w->d1.irqRequested = true;
        }
      }
    }
  }

  if (enable) {
    w->noise = 0;
    wiegandReset(w);
  } else {
    wiegandDisable(w);
  }

  if (result) {
    return result;
  }
  return count;
}

ssize_t devAttrWiegandData_show(struct device *dev,
                                struct device_attribute *attr, char *buf) {
  struct timespec64 now;
  unsigned long long diff;
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  if (!w->enabled) {
    return -ENODEV;
  }

  if (w->notifKn == NULL) {
    w->notifKn = sysfs_get_dirent(dev->kobj.sd, attr->attr.name);
  }

  ktime_get_raw_ts64(&now);
  diff = diff_usec((struct timespec64 *)&(w->lastBitTs), &now);
  if (diff <= w->pulseIntervalMax_usec) {
    return -EBUSY;
  }

  return sprintf(buf, "%llu %d %llu\n", to_usec(&w->lastBitTs), w->bitCount,
                 w->data);
}

ssize_t devAttrWiegandNoise_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  struct WiegandBean *w;
  int noise;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }
  noise = w->noise;

  w->noise = 0;

  return sprintf(buf, "%d\n", noise);
}

ssize_t devAttrWiegandPulseIntervalMin_show(struct device *dev,
                                            struct device_attribute *attr,
                                            char *buf) {
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  return sprintf(buf, "%lu\n", w->pulseIntervalMin_usec);
}

ssize_t devAttrWiegandPulseIntervalMin_store(struct device *dev,
                                             struct device_attribute *attr,
                                             const char *buf, size_t count) {
  int ret;
  unsigned long val;
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  ret = kstrtol(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  w->pulseIntervalMin_usec = val;

  return count;
}

ssize_t devAttrWiegandPulseIntervalMax_show(struct device *dev,
                                            struct device_attribute *attr,
                                            char *buf) {
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  return sprintf(buf, "%lu\n", w->pulseIntervalMax_usec);
}

ssize_t devAttrWiegandPulseIntervalMax_store(struct device *dev,
                                             struct device_attribute *attr,
                                             const char *buf, size_t count) {
  int ret;
  unsigned long val;
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  ret = kstrtol(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  w->pulseIntervalMax_usec = val;

  return count;
}

ssize_t devAttrWiegandPulseWidthMin_show(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf) {
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  return sprintf(buf, "%lu\n", w->pulseWidthMin_usec);
}

ssize_t devAttrWiegandPulseWidthMin_store(struct device *dev,
                                          struct device_attribute *attr,
                                          const char *buf, size_t count) {
  int ret;
  unsigned long val;
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  ret = kstrtol(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  w->pulseWidthMin_usec = val;

  return count;
}

ssize_t devAttrWiegandPulseWidthMax_show(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf) {
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  return sprintf(buf, "%lu\n", w->pulseWidthMax_usec);
}

ssize_t devAttrWiegandPulseWidthMax_store(struct device *dev,
                                          struct device_attribute *attr,
                                          const char *buf, size_t count) {
  int ret;
  unsigned long val;
  struct WiegandBean *w;
  w = wiegandGetBean(dev, attr);
  if (w == NULL) {
    return -EFAULT;
  }

  ret = kstrtol(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }

  w->pulseWidthMax_usec = val;

  return count;
}
