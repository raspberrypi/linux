#ifndef _SL_WIEGAND_H
#define _SL_WIEGAND_H

#include <linux/device.h>

#include "../gpio/gpio.h"

struct WiegandLine {
  struct GpioBean *gpio;
  unsigned int irq;
  bool irqRequested;
  bool wasLow;
};

struct WiegandBean {
  char id;
  struct WiegandLine d0;
  struct WiegandLine d1;
  struct WiegandLine *activeLine;
  unsigned long pulseIntervalMin_usec;
  unsigned long pulseIntervalMax_usec;
  unsigned long pulseWidthMin_usec;
  unsigned long pulseWidthMax_usec;
  bool enabled;
  uint64_t data;
  int bitCount;
  int noise;
  struct timespec64 lastBitTs;
  struct hrtimer timer;
  struct kernfs_node *notifKn;
};

void wiegandInit(struct WiegandBean *w);

void wiegandDisable(struct WiegandBean *w);

ssize_t devAttrWiegandEnabled_show(struct device *dev,
                                   struct device_attribute *attr, char *buf);

ssize_t devAttrWiegandEnabled_store(struct device *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count);

ssize_t devAttrWiegandData_show(struct device *dev,
                                struct device_attribute *attr, char *buf);

ssize_t devAttrWiegandNoise_show(struct device *dev,
                                 struct device_attribute *attr, char *buf);

ssize_t devAttrWiegandPulseIntervalMin_show(struct device *dev,
                                            struct device_attribute *attr,
                                            char *buf);

ssize_t devAttrWiegandPulseIntervalMin_store(struct device *dev,
                                             struct device_attribute *attr,
                                             const char *buf, size_t count);

ssize_t devAttrWiegandPulseIntervalMax_show(struct device *dev,
                                            struct device_attribute *attr,
                                            char *buf);

ssize_t devAttrWiegandPulseIntervalMax_store(struct device *dev,
                                             struct device_attribute *attr,
                                             const char *buf, size_t count);

ssize_t devAttrWiegandPulseWidthMin_show(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf);

ssize_t devAttrWiegandPulseWidthMin_store(struct device *dev,
                                          struct device_attribute *attr,
                                          const char *buf, size_t count);

ssize_t devAttrWiegandPulseWidthMax_show(struct device *dev,
                                         struct device_attribute *attr,
                                         char *buf);

ssize_t devAttrWiegandPulseWidthMax_store(struct device *dev,
                                          struct device_attribute *attr,
                                          const char *buf, size_t count);

struct WiegandBean *wiegandGetBean(struct device *dev,
                                   struct device_attribute *attr);

#endif
