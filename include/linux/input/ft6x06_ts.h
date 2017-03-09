#ifndef __LINUX_FT6X06_TS_H__
#define __LINUX_FT6X06_TS_H__

/* -- dirver configure -- */
#define CFG_MAX_TOUCH_POINTS	2

#define PRESS_MAX	0xFF
#define FT_PRESS		0x7F

#define FT6X06_NAME 	"ft6x06_ts"

#define FT_MAX_ID	0x0F
#define FT_TOUCH_STEP	6
#define FT_TOUCH_X_H_POS		3
#define FT_TOUCH_X_L_POS		4
#define FT_TOUCH_Y_H_POS		5
#define FT_TOUCH_Y_L_POS		6
#define FT_TOUCH_EVENT_POS		3
#define FT_TOUCH_ID_POS			5

/*register address*/
#define FT6x06_REG_FW_VER		0xA6
#define FT6x06_REG_POINT_RATE	0x88
#define FT6x06_REG_THGROUP	0x80

struct ft6x06_platform_data {
	unsigned int irq_gpio;
	unsigned int reset_gpio;
};

#endif
