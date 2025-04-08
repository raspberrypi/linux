#ifndef _SL_PCF2131_H
#define _SL_PCF2131_H

#include <linux/device.h>

int pcf2131_i2c_register_driver(void);
void pcf2131_i2c_unregister_driver(void);

ssize_t devAttrWatchdogEnabled_show(struct device *dev,
                                    struct device_attribute *attr, char *buf);
ssize_t devAttrWatchdogEnabled_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count);
ssize_t devAttrWatchdogHeartbeat_store(struct device *dev,
                                       struct device_attribute *attr,
                                       const char *buf, size_t count);
ssize_t devAttrWatchdogTimeout_show(struct device *dev,
                                    struct device_attribute *attr, char *buf);
ssize_t devAttrWatchdogTimeout_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count);
ssize_t devAttrWatchdogOffTime_show(struct device *dev,
                                    struct device_attribute *attr, char *buf);
ssize_t devAttrWatchdogOffTime_store(struct device *dev,
                                     struct device_attribute *attr,
                                     const char *buf, size_t count);

#endif
