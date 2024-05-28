#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <asm/io.h>

#define BCM2837_GPIO_BASE             0xFE200000
#define BCM2837_GPIO_FSEL0_OFFSET     0x0   // GPIO功能选择寄存器0
#define BCM2837_GPIO_SET0_OFFSET      0x1C  // GPIO置位寄存器0
#define BCM2837_GPIO_CLR0_OFFSET      0x28  // GPIO清零寄存器0

#define LED_ON  '1'
#define LED_OFF '0'

static void *gpio_base = NULL; // GPIO起始地址映射
static char led_value = '0';

// 打开设备，初始化GPIO LED设备状态，设置为输出模式
int rgbled_open(struct inode *inode, struct file *filp)
{
  // 映射GPIO物理内存到虚拟地址，并将其置为“输出模式”
  // 就是先把一个GPIO的“功能选择位”全部置000
  // 然后再将其置为001
  printk(KERN_INFO "led_open");
  int val = ioread32(gpio_base + BCM2837_GPIO_FSEL0_OFFSET);
  val &= ~(7 << 6);
  val |= 1 << 6;
  iowrite32(val, gpio_base);
  return 0;
}

ssize_t rgbled_read(struct file* filp, char __user* buf, size_t len, loff_t* off)
{
	printk(KERN_INFO "led_read start\n");
  return 0;
}

// 通过向文件写入value，控制LED灯状态
ssize_t rgbled_write(struct file* filp, const char __user* buf, size_t len, loff_t* off)
{
  printk("led_writer start");
	unsigned long count = 1; 
	if(copy_from_user((void *)&led_value, buf, count))
	{
		return -1;
  }
  printk(KERN_ERR"led_write led_num = %c,len = %zu\n",led_value, len);
  if(led_value == LED_ON) {
    // GPIO bit1 输出1
    iowrite32(1 << 2, gpio_base + BCM2837_GPIO_SET0_OFFSET);
  } else if (led_value == LED_OFF){
    // GPIO bit1 输出0
    iowrite32(1 << 2, gpio_base + BCM2837_GPIO_CLR0_OFFSET);
  } else {
    return -EINVAL;
  }
  printk(KERN_INFO "led_write end \n");
  return len;
}

static struct file_operations fops = {
  .owner = THIS_MODULE,
  .open = rgbled_open,
  .read = rgbled_read,
  .write = rgbled_write,
};

static dev_t devno = 0;   // 设备编号
static struct cdev cdev;  // 字符设备结构体

static int __init rgbled_init(void)
{
  // 1. 获取GPIO对应的Linux虚拟内存地址
  gpio_base = ioremap(BCM2837_GPIO_BASE, 0xB0);
  printk (KERN_INFO"global_gpio = 0x%lx\n", (unsigned long)gpio_base);

  // 2. 将该模块注册为一个字符设备，并动态分配设备号
  if (alloc_chrdev_region(&devno, 0, 1, "rgbled")) {
    printk(KERN_ERR"failed to register kernel module!\n");
    return -1;
  }
  cdev_init(&cdev, &fops);
  cdev_add(&cdev, devno, 1);
  printk(KERN_INFO"rgbled device major & minor is [%d:%d]\n", MAJOR(devno), MINOR(devno));
  return 0;
}
module_init(rgbled_init);

static void __exit rgbled_exit(void)
{
  // 1. 取消gpio物理内存映射
  iounmap(gpio_base);
  // 2. 释放字符设备
  cdev_del(&cdev);
  unregister_chrdev_region(devno, 1);
  printk(KERN_INFO"rgbled free\n");
}
module_exit(rgbled_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("exercise 3");