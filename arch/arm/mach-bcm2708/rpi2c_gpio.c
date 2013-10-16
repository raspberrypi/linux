#include <linux/module.h>
#include <linux/list.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <mach/gpio.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <mach/platform.h>

#include "rpi2c.h"

int rpi2c_set_function(unsigned offset,
                                int function);
int rpi2c_get(unsigned offset);
void rpi2c_set(unsigned offset, int value);
extern unsigned int rpi2c_sda_gpio_b;


static struct gpio_chip rpi2c_gpio;

static int rpi2c_gpio_dir_in(struct gpio_chip *gc, unsigned offset) {
	if((offset < 28 && offset > 31) || rpi2c_sda_gpio_b == offset) {
		return -EINVAL;
	}
	return rpi2c_set_function(offset, GPIO_FSEL_INPUT);	
}

static int rpi2c_gpio_dir_out(struct gpio_chip *gc, unsigned offset,
                                int value) {
        if((offset < 28 && offset > 31) || rpi2c_sda_gpio_b == offset) {
                return -EINVAL;
        }
	return rpi2c_set_function(offset, GPIO_FSEL_OUTPUT);
}

static int rpi2c_gpio_get(struct gpio_chip *gc, unsigned offset) {
        if((offset < 28 && offset > 31) || rpi2c_sda_gpio_b == offset) {
                return -EINVAL;
        }
	return rpi2c_get(offset);
}

static void rpi2c_gpio_set(struct gpio_chip *gc, unsigned offset, int value) {
        if((offset < 28 && offset > 31) || rpi2c_sda_gpio_b == offset) {
                return;
        }
	rpi2c_set(offset, value);
}

int rpi2c_gpio_init(void) {
	rpi2c_gpio.label = "rpi2c_gpio";
	rpi2c_gpio.base = 0;
	rpi2c_gpio.ngpio = ARCH_NR_GPIOS;
	rpi2c_gpio.owner = THIS_MODULE;

	rpi2c_gpio.direction_input = rpi2c_gpio_dir_in;
	rpi2c_gpio.direction_output = rpi2c_gpio_dir_out;
	rpi2c_gpio.get = rpi2c_gpio_get;
	rpi2c_gpio.set = rpi2c_gpio_set;
	rpi2c_gpio.can_sleep = 0;

	return gpiochip_add(&rpi2c_gpio);
}

void rpi2c_gpio_destroy(void) {
	gpiochip_remove(&rpi2c_gpio);
}













