#ifndef _SL_ATECC_H
#define _SL_ATECC_H

#include <linux/device.h>

ssize_t devAttrAteccSerial_show(struct device *dev,
                                struct device_attribute *attr, char *buf);
#endif
