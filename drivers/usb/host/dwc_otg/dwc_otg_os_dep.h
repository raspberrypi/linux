#ifndef _DWC_OS_DEP_H_
#define _DWC_OS_DEP_H_

/**
 * @file
 *
 * This file contains OS dependent structures.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/stat.h>
#include <linux/pci.h>

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,20)
# include <linux/irq.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,21)
# include <linux/usb/ch9.h>
#else
# include <linux/usb_ch9.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
# include <linux/usb/gadget.h>
#else
# include <linux/usb_gadget.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
# include <asm/irq.h>
#endif

#ifdef PCI_INTERFACE
# include <asm/io.h>
#endif

#ifdef LM_INTERFACE
# include <asm/unaligned.h>
# include <asm/sizes.h>
# include <asm/param.h>
# include <asm/io.h>
# if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30))
#  include <asm/arch/hardware.h>
#  include <asm/arch/lm.h>
#  include <asm/arch/irqs.h>
#  include <asm/arch/regs-irq.h>
# else
/* in 2.6.31, at least, we seem to have lost the generic LM infrastructure -
   here we assume that the machine architecture provides definitions
   in its own header
*/
#  include <mach/lm.h>
#  include <mach/hardware.h>
# endif
#endif

#ifdef PLATFORM_INTERFACE
#include <linux/platform_device.h>
#ifdef CONFIG_ARM
#include <asm/mach/map.h>
#endif
#endif

/** The OS page size */
#define DWC_OS_PAGE_SIZE	PAGE_SIZE

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,14)
typedef int gfp_t;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
# define IRQF_SHARED SA_SHIRQ
#endif

typedef struct os_dependent {
	/** Base address returned from ioremap() */
	void *base;

	/** Register offset for Diagnostic API */
	uint32_t reg_offset;

	/** Base address for MPHI peripheral */
	void *mphi_base;

#ifdef LM_INTERFACE
	struct lm_device *lmdev;
#elif  defined(PCI_INTERFACE)
	struct pci_dev *pcidev;

	/** Start address of a PCI region */
	resource_size_t rsrc_start;

	/** Length address of a PCI region */
	resource_size_t rsrc_len;
#elif  defined(PLATFORM_INTERFACE)
	struct platform_device *platformdev;
#endif

} os_dependent_t;

#ifdef __cplusplus
}
#endif



/* Type for the our device on the chosen bus */
#if   defined(LM_INTERFACE)
typedef struct lm_device       dwc_bus_dev_t;
#elif defined(PCI_INTERFACE)
typedef struct pci_dev         dwc_bus_dev_t;
#elif defined(PLATFORM_INTERFACE)
typedef struct platform_device dwc_bus_dev_t;
#endif

/* Helper macro to retrieve drvdata from the device on the chosen bus */
#if    defined(LM_INTERFACE)
#define DWC_OTG_BUSDRVDATA(_dev) lm_get_drvdata(_dev)
#elif  defined(PCI_INTERFACE)
#define DWC_OTG_BUSDRVDATA(_dev) pci_get_drvdata(_dev)
#elif  defined(PLATFORM_INTERFACE)
#define DWC_OTG_BUSDRVDATA(_dev) platform_get_drvdata(_dev)
#endif

/**
 * Helper macro returning the otg_device structure of a given struct device
 *
 * c.f. static dwc_otg_device_t *dwc_otg_drvdev(struct device *_dev)
 */
#ifdef LM_INTERFACE
#define DWC_OTG_GETDRVDEV(_var, _dev) do { \
                struct lm_device *lm_dev = \
                        container_of(_dev, struct lm_device, dev); \
                _var = lm_get_drvdata(lm_dev); \
        } while (0)

#elif defined(PCI_INTERFACE)
#define DWC_OTG_GETDRVDEV(_var, _dev) do { \
                _var = dev_get_drvdata(_dev); \
        } while (0)

#elif defined(PLATFORM_INTERFACE)
#define DWC_OTG_GETDRVDEV(_var, _dev) do { \
                struct platform_device *platform_dev = \
                        container_of(_dev, struct platform_device, dev); \
                _var = platform_get_drvdata(platform_dev); \
        } while (0)
#endif


/**
 * Helper macro returning the struct dev of the given struct os_dependent
 *
 * c.f. static struct device *dwc_otg_getdev(struct os_dependent *osdep)
 */
#ifdef LM_INTERFACE
#define DWC_OTG_OS_GETDEV(_osdep) \
        ((_osdep).lmdev == NULL? NULL: &(_osdep).lmdev->dev)
#elif defined(PCI_INTERFACE)
#define DWC_OTG_OS_GETDEV(_osdep) \
        ((_osdep).pci_dev == NULL? NULL: &(_osdep).pci_dev->dev)
#elif defined(PLATFORM_INTERFACE)
#define DWC_OTG_OS_GETDEV(_osdep) \
        ((_osdep).platformdev == NULL? NULL: &(_osdep).platformdev->dev)
#endif




#endif /* _DWC_OS_DEP_H_ */
