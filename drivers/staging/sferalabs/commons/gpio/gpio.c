#include "gpio.h"

#include <linux/delay.h>
#include <linux/interrupt.h>

#include "../misc/misc.h"

static struct platform_device *_pdev;

/**
 * convert common user inputs into boolean values
 * @s: input string
 * @res: result
 *
 * This routine returns 0 iff the first character is one of 'Yy1Nn0', or
 * [oO][NnFf] for "on" and "off". Otherwise it will return -EINVAL.  Value
 * pointed to by res is updated upon finding a match.
 */
static int mkstrtobool(const char *s, bool *res) {
  if (!s) return -EINVAL;

  switch (s[0]) {
    case 'y':
      // fall through
    case 'Y':
      // fall through
    case '1':
      *res = true;
      return 0;
    case 'n':
      // fall through
    case 'N':
      // fall through
    case '0':
      *res = false;
      return 0;
    case 'o':
      // fall through
    case 'O':
      switch (s[1]) {
        case 'n':
          // fall through
        case 'N':
          *res = true;
          return 0;
        case 'f':
          // fall through
        case 'F':
          *res = false;
          return 0;
        default:
          break;
      }
      break;
    default:
      break;
  }

  return -EINVAL;
}

static void debounceTimerRestart(struct DebouncedGpioBean *deb) {
  unsigned long debTime_usec;

  if (gpioGetVal(&deb->gpio)) {
    debTime_usec = deb->onMinTime_usec;
  } else {
    debTime_usec = deb->offMinTime_usec;
  }

  hrtimer_cancel(&deb->timer);
  hrtimer_start(&deb->timer, ktime_set(0, debTime_usec * 1000),
                HRTIMER_MODE_REL);
}

static irqreturn_t debounceIrqHandler(int irq, void *dev) {
  struct DebouncedGpioBean *deb;
  deb = (struct DebouncedGpioBean *)dev;
  if (deb->irq != irq) {
    // should never happen
    return IRQ_HANDLED;
  }
  debounceTimerRestart(deb);
  return IRQ_HANDLED;
}

static enum hrtimer_restart debounceTimerHandler(struct hrtimer *tmr) {
  struct DebouncedGpioBean *deb;
  int val;

  deb = container_of(tmr, struct DebouncedGpioBean, timer);
  val = gpioGetVal(&deb->gpio);

  if (deb->value != val) {
    deb->value = val;
    if (val) {
      deb->onCnt++;
    } else {
      deb->offCnt++;
    }
    if (deb->notifKn != NULL) {
      sysfs_notify_dirent(deb->notifKn);
    }
  }

  return HRTIMER_NORESTART;
}

void gpioSetPlatformDev(struct platform_device *pdev) { _pdev = pdev; }

int gpioInit(struct GpioBean *g) {
  g->desc = gpiod_get(&_pdev->dev, g->name, g->flags);
  return IS_ERR(g->desc);
}

int gpioInitDebounce(struct DebouncedGpioBean *d) {
  int res;

  res = gpioInit(&d->gpio);
  if (res) {
    return res;
  }

  d->irqRequested = false;
  d->value = DEBOUNCE_STATE_NOT_DEFINED;
  d->onMinTime_usec = DEBOUNCE_DEFAULT_TIME_USEC;
  d->offMinTime_usec = DEBOUNCE_DEFAULT_TIME_USEC;
  d->onCnt = 0;
  d->offCnt = 0;

  hrtimer_init(&d->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
  d->timer.function = &debounceTimerHandler;

  d->irq = gpiod_to_irq(d->gpio.desc);
  res = request_irq(d->irq, debounceIrqHandler,
                    (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING), d->gpio.name,
                    d);
  if (res) {
    return res;
  }
  d->irqRequested = true;

  debounceTimerRestart(d);

  return res;
}

void gpioFree(struct GpioBean *g) {
  if (g->desc != NULL && !IS_ERR(g->desc)) {
    gpiod_put(g->desc);
  }
}

void gpioFreeDebounce(struct DebouncedGpioBean *d) {
  gpioFree(&d->gpio);
  if (d->irqRequested) {
    free_irq(d->irq, d);
    hrtimer_cancel(&d->timer);
    d->irqRequested = false;
  }
}

int gpioGetVal(struct GpioBean *g) {
  int v;
  v = gpiod_get_value(g->desc);
  if (g->invert) {
    v = v == 0 ? 1 : 0;
  }
  return v;
}

void gpioSetVal(struct GpioBean *g, int val) {
  if (g->invert) {
    val = val == 0 ? 1 : 0;
  }
  gpiod_set_value(g->desc, val);
}

ssize_t devAttrGpioMode_show(struct device *dev, struct device_attribute *attr,
                             char *buf) {
  struct GpioBean *g;
  const char *vals = NULL;
  g = gpioGetBean(dev, attr, &vals);
  if (g == NULL) {
    return -EFAULT;
  }
  if (g->flags == GPIOD_IN) {
    return sprintf(buf, "in\n");
  }
  if (g->flags == GPIOD_OUT_HIGH || g->flags == GPIOD_OUT_LOW) {
    return sprintf(buf, "out\n");
  }
  return sprintf(buf, "x\n");
}

ssize_t devAttrGpioMode_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count) {
  struct GpioBean *g;
  const char *vals = NULL;
  g = gpioGetBean(dev, attr, &vals);
  if (g == NULL) {
    return -EFAULT;
  }

  if (g->owner != NULL && g->owner != attr) {
    return -EBUSY;
  }

  gpioFree(g);
  g->owner = NULL;

  if (toUpper(buf[0]) == 'I') {
    g->flags = GPIOD_IN;
  } else if (toUpper(buf[0]) == 'O') {
    g->flags = GPIOD_OUT_LOW;
  } else {
    g->flags = 0;
  }

  if (g->flags != 0) {
    if (gpioInit(g)) {
      g->flags = 0;
      return -EFAULT;
    } else {
      g->owner = attr;
    }
  }

  return count;
}

ssize_t devAttrGpio_show(struct device *dev, struct device_attribute *attr,
                         char *buf) {
  struct GpioBean *g;
  const char *vals = NULL;
  g = gpioGetBean(dev, attr, &vals);
  if (g == NULL) {
    return -EFAULT;
  }
  if (g->flags != GPIOD_IN && g->flags != GPIOD_OUT_LOW &&
      g->flags != GPIOD_OUT_HIGH) {
    return -EPERM;
  }
  return valToStr(buf, gpioGetVal(g), vals, false, 0, 10, 0);
}

ssize_t devAttrGpio_store(struct device *dev, struct device_attribute *attr,
                          const char *buf, size_t count) {
  bool bVal;
  int64_t val;
  struct GpioBean *g;
  const char *vals = NULL;
  g = gpioGetBean(dev, attr, &vals);
  if (g == NULL) {
    return -EFAULT;
  }
  if (g->flags != GPIOD_OUT_HIGH && g->flags != GPIOD_OUT_LOW) {
    return -EPERM;
  }

  if (vals == NULL) {
    if (mkstrtobool(buf, &bVal) < 0) {
      if (toUpper(buf[0]) == 'E') {  // Enable
        bVal = true;
      } else if (toUpper(buf[0]) == 'D') {  // Disable
        bVal = false;
      } else if (toUpper(buf[0]) == 'F' ||
                 toUpper(buf[0]) == 'T') {  // Flip/Toggle
        bVal = gpioGetVal(g) == 1 ? false : true;
      } else {
        return -EINVAL;
      }
    }
    val = bVal ? 1 : 0;
  } else {
    val = strToVal(buf, vals, false, 10);
    if (val < 0) {
      return val;
    }
  }

  gpioSetVal(g, val);
  return count;
}

ssize_t devAttrGpioBlink_store(struct device *dev,
                               struct device_attribute *attr, const char *buf,
                               size_t count) {
  int i;
  long on = 0;
  long off = 0;
  long rep = 1;
  char *end = NULL;
  struct GpioBean *g;
  const char *vals = NULL;
  g = gpioGetBean(dev, attr, &vals);
  if (g == NULL) {
    return -EFAULT;
  }
  if (g->flags != GPIOD_OUT_HIGH && g->flags != GPIOD_OUT_LOW) {
    return -EPERM;
  }
  on = simple_strtol(buf, &end, 10);
  if (++end < buf + count) {
    off = simple_strtol(end, &end, 10);
    if (++end < buf + count) {
      rep = simple_strtol(end, NULL, 10);
    }
  }
  if (rep < 1) {
    rep = 1;
  }
  if (on > 0) {
    for (i = 0; i < rep; i++) {
      gpioSetVal(g, 1);
      msleep(on);
      gpioSetVal(g, 0);
      if (i < rep - 1) {
        msleep(off);
      }
    }
  }
  return count;
}

static struct DebouncedGpioBean *gpioGetDebouncedBean(struct device *dev,
                                               struct device_attribute *attr) {
  struct GpioBean *g;
  struct DebouncedGpioBean *d;
  const char *vals = NULL;
  g = gpioGetBean(dev, attr, &vals);
  if (g == NULL) {
    return NULL;
  }
  d = container_of(g, struct DebouncedGpioBean, gpio);
  return d;
}

ssize_t devAttrGpioDeb_show(struct device *dev, struct device_attribute *attr,
                            char *buf) {
  struct DebouncedGpioBean *d;
  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }

  if (d->notifKn == NULL) {
    d->notifKn = sysfs_get_dirent(dev->kobj.sd, attr->attr.name);
  }

  return sprintf(buf, "%d\n", d->value);
}

ssize_t devAttrGpioDebMsOn_show(struct device *dev,
                                struct device_attribute *attr, char *buf) {
  struct DebouncedGpioBean *d;
  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }
  return sprintf(buf, "%lu\n", d->onMinTime_usec / 1000);
}

ssize_t devAttrGpioDebMsOff_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  struct DebouncedGpioBean *d;
  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }
  return sprintf(buf, "%lu\n", d->offMinTime_usec / 1000);
}

ssize_t devAttrGpioDebMsOn_store(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count) {
  unsigned int val;
  int ret;
  struct DebouncedGpioBean *d;

  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }
  ret = kstrtouint(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }
  d->onMinTime_usec = val * 1000;
  d->onCnt = 0;
  d->offCnt = 0;
  d->value = DEBOUNCE_STATE_NOT_DEFINED;
  debounceTimerRestart(d);

  return count;
}

ssize_t devAttrGpioDebMsOff_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count) {
  unsigned int val;
  int ret;
  struct DebouncedGpioBean *d;

  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }
  ret = kstrtouint(buf, 10, &val);
  if (ret < 0) {
    return ret;
  }
  d->offMinTime_usec = val * 1000;
  d->onCnt = 0;
  d->offCnt = 0;
  d->value = DEBOUNCE_STATE_NOT_DEFINED;
  debounceTimerRestart(d);

  return count;
}

ssize_t devAttrGpioDebOnCnt_show(struct device *dev,
                                 struct device_attribute *attr, char *buf) {
  struct DebouncedGpioBean *d;
  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }
  return sprintf(buf, "%lu\n", d->onCnt);
}

ssize_t devAttrGpioDebOffCnt_show(struct device *dev,
                                  struct device_attribute *attr, char *buf) {
  struct DebouncedGpioBean *d;
  d = gpioGetDebouncedBean(dev, attr);
  if (d == NULL) {
    return -EFAULT;
  }
  return sprintf(buf, "%lu\n", d->offCnt);
}
