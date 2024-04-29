#include <asm/alternative-macros.h>
#include <asm/barrier.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pkvm_module.h>
#include <asm/io.h>

static unsigned long uart_addr;

static inline unsigned int __hyp_readw(void *ioaddr)
{
	unsigned int val;
	asm volatile("ldr %w0, [%1]" : "=r" (val) : "r" (ioaddr));
	return val;
}
static inline void __hyp_writew(unsigned int val, void *ioaddr)
{
	asm volatile("str %w0, [%1]" : : "r" (val), "r" (ioaddr));
}

static void pl011_hyp_putc(char c)
{
	void *base = (void *)uart_addr;
	unsigned int val;

	do {
		val = __hyp_readw(base + CONFIG_SERIAL_PKVM_PL011_UARTFR);
	} while (val & (1U <<CONFIG_SERIAL_PKVM_PL011_FULL));
	dmb(sy);

	__hyp_writew(c, base + CONFIG_SERIAL_PKVM_PL011_UARTTX);

	do {
		val = __hyp_readw(base + CONFIG_SERIAL_PKVM_PL011_UARTFR);
	} while (val & (1U << CONFIG_SERIAL_PKVM_PL011_BUSY));
	dmb(sy);
}

int pl011_hyp_init(const struct pkvm_module_ops *ops)
{
	int ret;

	ret = ops->create_private_mapping(CONFIG_SERIAL_PKVM_PL011_BASE_PHYS, PAGE_SIZE,
					  PAGE_HYP_DEVICE, &uart_addr);
	if (ret)
		return ret;

	ret = ops->register_serial_driver(pl011_hyp_putc);
	if (ret)
		return ret;

	ops->puts("pKVM pl011 UART driver loaded");

	return 0;
}
