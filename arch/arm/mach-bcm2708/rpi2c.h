#define DRIVER_NAME "rpi2c"

#define GPIOFSEL(x)  (0x00+(x)*4)
#define GPIOSET(x)   (0x1c+(x)*4)
#define GPIOCLR(x)   (0x28+(x)*4)
#define GPIOLEV(x)   (0x34+(x)*4)
#define GPIOEDS(x)   (0x40+(x)*4)
#define GPIOREN(x)   (0x4c+(x)*4)
#define GPIOFEN(x)   (0x58+(x)*4)
#define GPIOHEN(x)   (0x64+(x)*4)
#define GPIOLEN(x)   (0x70+(x)*4)
#define GPIOAREN(x)  (0x7c+(x)*4)
#define GPIOAFEN(x)  (0x88+(x)*4)
#define GPIOUD(x)    (0x94+(x)*4)
#define GPIOUDCLK(x) (0x98+(x)*4)

#define PM_PADS(x) (0x2c+(x)*4)

enum { GPIO_FSEL_INPUT, GPIO_FSEL_OUTPUT,
        GPIO_FSEL_ALT5, GPIO_FSEL_ALT_4,
        GPIO_FSEL_ALT0, GPIO_FSEL_ALT1,
        GPIO_FSEL_ALT2, GPIO_FSEL_ALT3,
};

//#define RPI2C_DEBUG
#define MAX_I2C_DATA 10
#define MAX_FIQ_PACKETS 10
#define MAX_I2C_DATA_HEX ((MAX_I2C_DATA * 2) + 2)


struct i2c_data {
        unsigned char bytes[MAX_I2C_DATA];
        unsigned int count;
};

struct fiq_stack_s {
        unsigned int magic1;
        uint8_t stack[2048];
        unsigned int magic2;
};

