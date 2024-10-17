// SPDX-License-Identifier: MIT
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#ifndef _HAILO_DRIVER_UTILS_H_
#define _HAILO_DRIVER_UTILS_H_

#include <linux/bitops.h>

#define DWORD_SIZE                  (4)
#define WORD_SIZE                   (2)
#define BYTE_SIZE                   (1)
#define BITS_IN_BYTE                (8)

#define hailo_clear_bit(bit, pval)  { *(pval) &= ~(1 << bit); }
#define hailo_test_bit(pos,var_addr)  ((*var_addr) & (1<<(pos)))

#define READ_BITS_AT_OFFSET(amount_bits, offset, initial_value) \
    (((initial_value) >> (offset)) & ((1 << (amount_bits)) - 1))
#define WRITE_BITS_AT_OFFSET(amount_bits, offset, initial_value, value) \
    (((initial_value) & ~(((1 << (amount_bits)) - 1) << (offset))) | \
    (((value) & ((1 << (amount_bits)) - 1)) << (offset)))

#ifdef __cplusplus
extern "C"
{
#endif

static inline bool is_powerof2(size_t v) {
    // bit trick
    return (v & (v - 1)) == 0;
}

static inline void hailo_set_bit(int nr, u32* addr) {
	u32 mask = BIT_MASK(nr);
	u32 *p = addr + BIT_WORD(nr);

	*p  |= mask;
}

static inline uint8_t ceil_log2(uint32_t n)
{
    uint8_t result = 0;

    if (n <= 1) {
        return 0;
    }

    while (n > 1) {
        result++;
        n = (n + 1) >> 1;
    }

    return result;
}

// Gets the nearest power of 2 >= value, for any value <= MAX_POWER_OF_2_VALUE. Otherwise POWER_OF_2_ERROR is returned.
#define MAX_POWER_OF_2_VALUE (0x80000000)
#define POWER_OF_2_ERROR ((uint32_t)-1)
static inline uint32_t get_nearest_powerof_2(uint32_t value)
{
    uint32_t power_of_2 = 1;
    if (value > MAX_POWER_OF_2_VALUE) {
        return POWER_OF_2_ERROR;
    }

    while (value > power_of_2) {
        power_of_2 <<=  1;
    }
    return power_of_2;
}

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#endif

#ifdef __cplusplus
}
#endif

#endif // _HAILO_DRIVER_UTILS_H_