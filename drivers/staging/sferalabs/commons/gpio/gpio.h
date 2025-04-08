#ifndef _SL_GPIO_H
#define _SL_GPIO_H

#include <linux/gpio/consumer.h>
#include <linux/platform_device.h>
#include <linux/version.h>

#define DEBOUNCE_DEFAULT_TIME_USEC 50000ul
#define DEBOUNCE_STATE_NOT_DEFINED -1

struct GpioBean {
  const char *name;
  struct gpio_desc *desc;
  enum gpiod_flags flags;
  bool invert;
  void *owner;
};

struct DebouncedGpioBean {
  struct GpioBean gpio;
  int value;
  int irq;
  bool irqRequested;
  unsigned long onMinTime_usec;
  unsigned long offMinTime_usec;
  unsigned long onCnt;
  unsigned long offCnt;
  struct hrtimer timer;
  struct kernfs_node *notifKn;
};

void gpioSetPlatformDev(struct platform_device *pdev);

int gpioInit(struct GpioBean *g);

int gpioInitDebounce(struct DebouncedGpioBean *d);

void gpioFree(struct GpioBean *g);

void gpioFreeDebounce(struct DebouncedGpioBean *d);

int gpioGetVal(struct GpioBean *g);

void gpioSetVal(struct GpioBean *g, int val);

ssize_t devAttrGpioMode_show(struct device *dev, struct device_attribute *attr,
                             char *buf);

ssize_t devAttrGpioMode_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count);

ssize_t devAttrGpio_show(struct device *dev, struct device_attribute *attr,
                         char *buf);

ssize_t devAttrGpio_store(struct device *dev, struct device_attribute *attr,
                          const char *buf, size_t count);

ssize_t devAttrGpioDeb_show(struct device *dev, struct device_attribute *attr,
                            char *buf);

ssize_t devAttrGpioDebMsOn_show(struct device *dev,
                                struct device_attribute *attr, char *buf);

ssize_t devAttrGpioDebMsOn_store(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count);

ssize_t devAttrGpioDebMsOff_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

ssize_t devAttrGpioDebMsOff_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t count);

ssize_t devAttrGpioDebOnCnt_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

ssize_t devAttrGpioDebOffCnt_show(struct device *dev,
                                  struct device_attribute *attr, char *buf);

ssize_t devAttrGpioBlink_store(struct device *dev,
                               struct device_attribute *attr, const char *buf,
                               size_t count);

struct GpioBean *gpioGetBean(struct device *dev, struct device_attribute *attr,
                             const char **vals);

#endif
