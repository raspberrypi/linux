/*
 * Adafruit power switch driver for Raspberry Pi
 *
 * Simulated power switch / button, using the GPIO banks.
 *
 * - Written by Sean Cross for Adafruit Industries (www.adafruit.com)
 */

#define RPI_POWER_SWITCH_VERSION "1.7"
#define POWER_SWITCH_CLASS_NAME "rpi-power-switch"

#include <linux/module.h>

#include <asm/io.h>
#include <asm/gpio.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/kdev_t.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/workqueue.h>


#ifndef BCM2708_PERI_BASE
  #define BCM2708_PERI_BASE	0x20000000
#endif

#define GPIO_BASE		(BCM2708_PERI_BASE + 0x200000)

#define GPPUD (gpio_reg+0x94)
#define GPPUDCLK0 (gpio_reg+0x98)
#define GPPUDCLK1 (gpio_reg+0x9C)
#define GPSET0    (gpio_reg+0x1c)
#define GPSET1    (gpio_reg+0x20)
#define GPCLR0    (gpio_reg+0x28)
#define GPCLR1    (gpio_reg+0x2c)

#define GPIO_REG(g) (gpio_reg+((g/10)*4))
#define SET_GPIO_OUTPUT(g) \
	__raw_writel( 							\
		(1<<(((g)%10)*3))					\
		| (__raw_readl(GPIO_REG(g)) & (~(7<<(((g)%10)*3)))),	\
		GPIO_REG(g))
#define SET_GPIO_INPUT(g) \
	__raw_writel( 							\
		0							\
		| (__raw_readl(GPIO_REG(g)) & (~(7<<(((g)%10)*3)))),	\
		GPIO_REG(g))
#define SET_GPIO_ALT(g,a) \
	__raw_writel( 							\
		(((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))		\
		| (__raw_readl(GPIO_REG(g)) & (~(7<<(((g)%10)*3)))),	\
		GPIO_REG(g))

enum button_mode {
	MODE_BUTTON = 0,
	MODE_SWITCH = 1,
};


enum gpio_pull_direction {
	GPIO_PULL_NONE = 0,
	GPIO_PULL_DOWN = 1,
	GPIO_PULL_UP = 2,
};


/* Module Parameters */
static int gpio_pin = 22;
static int mode = MODE_SWITCH;
static int led_pin = 16;

/* This is the base state.  When this changes, do a shutdown. */
static int gpio_pol;

static void __iomem *gpio_reg;
static void (*old_pm_power_off)(void);
static struct device *switch_dev;
static int raw_gpio = 0;


/* Attach either a pull up or pull down to the specified GPIO pin.  Or
 * clear any pull on the pin, if requested.
 */
static int set_gpio_pull(int gpio, enum gpio_pull_direction direction) {
	long *bank;
	int pin;

	bank = ((gpio&(~31))?GPPUDCLK1:GPPUDCLK0);
	pin = gpio & 31;

	/* Set the direction (involves two writes and a clock wait) */
	__raw_writel(direction, GPPUD);
	udelay(20);
	__raw_writel(1<<pin, bank);
	udelay(20);

	/* Cleanup */
	__raw_writel(0, GPPUD);
	__raw_writel(0, bank);
	return 0;
}


/* If the GPIO we want to use is already being used (e.g. if a driver
 * forgot to call gpio_free() during its module_exit() call), then we
 * will have to directly access the GPIO registers in order to set or
 * clear values.
 */
static int raw_gpio_set(int gpio, int val) {
	if (gpio < 0 || gpio > 63)
		return -1;
	else if (gpio < 32) 
		__raw_writel(1<<gpio, val?GPSET0:GPCLR0);
	else if (gpio < 64)
		__raw_writel(1<<gpio, val?GPSET1:GPCLR1);
	return 0;
}

/* Bottom half of the power switch ISR.
 * We need to break this out here, as you can't run call_usermodehelper
 * from an interrupt context.
 * This function will actually Call /sbin/shutdown when the switch gets hit.
 */
static void initiate_shutdown(struct work_struct *work) {
	int ret;
	char *cmd = "/sbin/shutdown";
	char *argv[] = {
		cmd,
		"-h",
		"now",
		NULL,
	};
	char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		NULL,
	};

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	/* We only want this IRQ to fire once, ever. */
	free_irq(gpio_to_irq(gpio_pin), NULL);

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	/* Make sure the switch hasn't just bounced */
	if (mode == MODE_SWITCH && gpio_get_value(gpio_pin) != gpio_pol)
		return;

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);


	ret = call_usermodehelper(cmd, argv, envp, UMH_WAIT_PROC);

//printk(KERN_ALERT "returned %d\n", ret);

}

static struct delayed_work initiate_shutdown_work;


/* This ISR gets called when the board is "off" and the switch changes.
 * It indicates we should start back up again, which means we need to
 * do a reboot.
 */
static irqreturn_t reboot_isr(int irqno, void *param) {
	emergency_restart();
	return IRQ_HANDLED;
}



/* Pulse the GPIO low for /duty/ cycles and then /high/ for 100-duty cycles.
 * Returns the number of usecs delayed.
 */
#define RATE 1
static int gpio_pulse(int gpio, int duty) {
	int low;
	int high;

	if (duty < 0)
		duty = 0;
	if (duty > 100)
		duty = 100;
	low = duty;
	high = 100-duty;

	if (raw_gpio)
		raw_gpio_set(gpio, 0);
	else
		gpio_set_value(gpio, 0);
	udelay(RATE*low);

	if (raw_gpio)
		raw_gpio_set(gpio, 1);
	else
		gpio_set_value(gpio, 1);
	udelay(RATE*high);

	return (RATE*low)+(RATE*high);
}



/* Give an indication that it's safe to turn off the board.  Pulse the LED
 * in a kind of "breathing" pattern, so the user knows that it's
 * "powered down".
 */
static int do_breathing_forever(int gpio) {
	int err;
	err = gpio_request(gpio, "LED light");
	if (err < 0) {
		pr_err("Unable to request GPIO, switching to raw access");
		raw_gpio = 1;
	}
	SET_GPIO_OUTPUT(gpio);

	while (1) {
		int usecs;
		/* We want four seconds:
		 *   - One second of ramp-up
		 *   - One second of ramp-down
		 *   - Two seconds of low
		 */
		for (usecs=0; usecs < 800000; )
			usecs += gpio_pulse(gpio, ((usecs*9)/80000)+10);

		for (usecs=0; usecs < 800000; )
			usecs += gpio_pulse(gpio, 100-((usecs*9)/80000));

		for (usecs=0; usecs < 800000; )
			usecs += gpio_pulse(gpio, 10);

		for (usecs=0; usecs < 800000; )
			usecs += gpio_pulse(gpio, 10);
	}
	return 0;
}



/* Our shutdown function.  Execution will stay here until the switch is
 * flipped.
 * NOTE: The default power_off function sends a message to the GPU via
 * a mailbox message to shut down most parts of the core.  Since we don't
 * have any documentation on the mailbox message formats, we will leave
 * the CPU powered up here but not executing any code in order to simulate
 * an "off" state.
 */
static void rpi_power_switch_power_off(void) {
	int ret;
	pr_info("Waiting for the switch to be flipped back...\n");
	if (mode == MODE_SWITCH)
		gpio_pol = !gpio_pol;
	ret = request_irq(gpio_to_irq(gpio_pin), reboot_isr,
			  gpio_pol?IRQF_TRIGGER_RISING:IRQF_TRIGGER_FALLING,
			  "Reboot ISR", NULL);

	/* If it's taken us so long to reboot that the switch was flipped,
	 * immediately reboot.
	 */
	if (gpio_pol == gpio_get_value(gpio_pin))
		reboot_isr(0, NULL);

	do_breathing_forever(led_pin);
	return;
}


static irqreturn_t power_isr(int irqno, void *param) {
	schedule_delayed_work(&initiate_shutdown_work, msecs_to_jiffies(100));
	return IRQ_HANDLED;
}



/* Sysfs entry */

static ssize_t do_shutdown_show(struct device *d,
				struct device_attribute *attr, char *buf)
{
        ssize_t ret;
        ret = sprintf(buf, "Write into this file to initiate a shutdown\n");
        return ret;
}

static ssize_t do_shutdown_store(struct device *d,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (mode == MODE_SWITCH)
		gpio_pol = !gpio_pol;
	schedule_delayed_work(&initiate_shutdown_work, msecs_to_jiffies(10));
	return count;
}
static DEVICE_ATTR(do_shutdown, 0660, do_shutdown_show, do_shutdown_store);

static struct attribute *rpi_power_switch_sysfs_entries[] = {
	&dev_attr_do_shutdown.attr,
	NULL,
};

static struct attribute_group rpi_power_switch_attribute_group = {
        .name = NULL,
        .attrs = rpi_power_switch_sysfs_entries,
};

static struct class power_switch_class = {
	.name =		POWER_SWITCH_CLASS_NAME,
	.owner =	THIS_MODULE,
};




/* Main module entry point */

int __init rpi_power_switch_init(void)
{
	int ret = 0;

	old_pm_power_off = pm_power_off;
	pm_power_off = rpi_power_switch_power_off;

	pr_info("Adafruit Industries' power switch driver v%s\n",
		RPI_POWER_SWITCH_VERSION);

	INIT_DELAYED_WORK(&initiate_shutdown_work, initiate_shutdown);

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	/* Register our own class for the power switch */
	ret = class_register(&power_switch_class);
        if (ret < 0) {
		pr_err("%s: Unable to register class\n", power_switch_class.name);
		goto out0;
	}


//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

        /* Create devices for each PWM present */
	switch_dev = device_create(&power_switch_class, &platform_bus,
                                MKDEV(0, 0), NULL, "pswitch%u", 0);
	if (IS_ERR(switch_dev)) {
		pr_err("%s: device_create failed\n", power_switch_class.name);
		ret = PTR_ERR(switch_dev);
		goto out1;
        }

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	ret = sysfs_create_group(&switch_dev->kobj,
				 &rpi_power_switch_attribute_group);
	if (ret < 0) {
		pr_err("%s: create_group failed\n", power_switch_class.name);
		goto out2;
	}

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	/* GPIO register memory must be mapped before doing any direct
	 * accesses such as changing GPIO alt functions or changing GPIO
	 * pull ups or pull downs.
	 */
	gpio_reg = ioremap(GPIO_BASE, 1024);

	/* Set the specified pin as a GPIO input */
	SET_GPIO_INPUT(gpio_pin);

	/* Set the pin as a pulldown.  Most pins should default to having
	 * pulldowns, and this seems most intuitive.
	 */
	set_gpio_pull(gpio_pin, GPIO_PULL_UP);

	ret = gpio_request(gpio_pin, "Power switch");
	if (ret) {
		printk(KERN_ALERT "GPIO request failure: %d\n", ret);
		goto out3;
	}

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	gpio_direction_input(gpio_pin);

	/* The targeted polarity should be the opposite of the current value.
	 * I.e. we want the pin to transition to this state in order to
	 * initiate a shutdown.
	 */
	gpio_pol = !gpio_get_value(gpio_pin);

	/* Request an interrupt to fire when the pin transitions to our
	 * desired state.
	 */
	ret = request_irq(__gpio_to_irq(gpio_pin), power_isr,
			  gpio_pol?IRQF_TRIGGER_RISING:IRQF_TRIGGER_FALLING,
			  "Power button", NULL);
	if (ret) {
		pr_err("Unable to request IRQ\n");
		goto out3;
	}

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

	return 0;


	/* Error handling */
out3:
	sysfs_remove_group(&switch_dev->kobj,&rpi_power_switch_attribute_group);
out2:
	device_unregister(switch_dev);
out1:
	class_unregister(&power_switch_class);
out0:
	iounmap(gpio_reg);
	pm_power_off = old_pm_power_off;
	return ret;
}


/* Main module exit point (called at unload) */

void __exit rpi_power_switch_cleanup(void)
{

//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
	sysfs_remove_group(&switch_dev->kobj,&rpi_power_switch_attribute_group);
	device_unregister(switch_dev);
	free_irq(__gpio_to_irq(gpio_pin), NULL);
//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);
	gpio_free(gpio_pin);
	pm_power_off = old_pm_power_off;
	class_unregister(&power_switch_class);
	iounmap(gpio_reg);
//printk(KERN_ALERT "DEBUG: Passed %s %d \n",__FUNCTION__,__LINE__);

}

module_init(rpi_power_switch_init);
module_exit(rpi_power_switch_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sean Cross <xobs@xoblo.gs> for Adafruit Industries <www.adafruit.com>");
MODULE_ALIAS("platform:bcm2708_power_switch");
module_param(gpio_pin, int, 0);
module_param(led_pin, int, 0);
module_param(mode, int, 0);
