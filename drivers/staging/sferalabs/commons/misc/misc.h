#ifndef _SL_COMMONS_H
#define _SL_COMMONS_H

#include <linux/time.h>

unsigned long long to_usec(struct timespec64 *t);
unsigned long long diff_usec(struct timespec64 *t1, struct timespec64 *t2);
char toUpper(char c);
int valToStr(char *buf, int64_t val, const char *vals, bool sign, uint8_t len,
             uint8_t base, uint32_t mask);
int64_t strToVal(const char *buf, const char *vals, bool sign, uint8_t base);

#endif
