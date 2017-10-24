#ifndef __CUSTOM_GPIO_H__
#define __CUSTOM_GPIO_H___

#include <drv_conf.h>
#include <osdep_service.h>
#include <drv_types_linux.h>

typedef enum cust_gpio_modes {
	WLAN_PWDN_ON,
	WLAN_PWDN_OFF,
	WLAN_POWER_ON,
	WLAN_POWER_OFF,
	WLAN_BT_PWDN_ON,
	WLAN_BT_PWDN_OFF
} cust_gpio_modes_t;

extern int rtw_wifi_gpio_init(void);
extern int rtw_wifi_gpio_deinit(void);
extern void rtw_wifi_gpio_wlan_ctrl(int onoff);

#endif
