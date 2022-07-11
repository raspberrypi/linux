/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SOC_RASPBERRY_CLOCKS_H__
#define __SOC_RASPBERRY_CLOCKS_H__

#if IS_ENABLED(CONFIG_CLK_RASPBERRYPI)
unsigned long rpi_firmware_clk_get_max_rate(struct clk *clk);
#else
static inline unsigned long rpi_firmware_clk_get_max_rate(struct clk *clk)
{
	return ULONG_MAX;
}
#endif

#endif /* __SOC_RASPBERRY_CLOCKS_H__ */
