#ifndef __MEMORY_API_H
#define __MEMORY_API_H

/*
 * Bus Interface Unit control register setup, must happen early during boot,
 * before SMP is brought up, called by machine entry point.
 */
void brcmstb_biuctrl_init(void);

#ifdef CONFIG_SOC_BRCMSTB
int brcmstb_memory_phys_addr_to_memc(phys_addr_t pa);
u64 brcmstb_memory_memc_size(int memc);
#else
static inline int brcmstb_memory_phys_addr_to_memc(phys_addr_t pa)
{
	return -EINVAL;
}

static inline u64 brcmstb_memory_memc_size(int memc)
{
	return -1;
}
#endif

#endif /* __MEMORY_API_H */
