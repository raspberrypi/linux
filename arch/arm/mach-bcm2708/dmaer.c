#include <linux/init.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/timex.h>

#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <asm/cacheflush.h>
#include <asm/io.h>

#include <mach/dma.h>

MODULE_LICENSE("Dual BSD/GPL");

//#define inline 

/***** TYPES ****/
#define PAGES_PER_LIST 500
struct PageList
{
	struct page *m_pPages[PAGES_PER_LIST];
	unsigned int m_used;
	struct PageList *m_pNext;
};

struct VmaPageList
{
	//each vma has a linked list of pages associated with it
	struct PageList *m_pPageHead;
	struct PageList *m_pPageTail;
	unsigned int m_refCount;
};
struct DmaControlBlock
{
	unsigned int m_transferInfo;
	void __user *m_pSourceAddr;
	void __user *m_pDestAddr;
	unsigned int m_xferLen;
	unsigned int m_tdStride;
	struct DmaControlBlock *m_pNext;
	unsigned int m_blank1, m_blank2;
};

/***** DEFINES ******/
//magic number defining the module
#define DMA_MAGIC		0xdd

//do user virtual to physical translation of the CB chain
#define DMA_PREPARE		_IOWR(DMA_MAGIC, 0, struct DmaControlBlock *)

//kick the pre-prepared CB chain
#define DMA_KICK		_IOW(DMA_MAGIC, 1, struct DmaControlBlock *)

//prepare it, kick it, wait for it
#define DMA_PREPARE_KICK_WAIT	_IOWR(DMA_MAGIC, 2, struct DmaControlBlock *)

//prepare it, kick it, don't wait for it
#define DMA_PREPARE_KICK	_IOWR(DMA_MAGIC, 3, struct DmaControlBlock *)

//not currently implemented
#define DMA_WAIT_ONE		_IO(DMA_MAGIC, 4, struct DmaControlBlock *)

//wait on all kicked CB chains
#define DMA_WAIT_ALL		_IO(DMA_MAGIC, 5)

//in order to discover the largest AXI burst that should be programmed into the transfer params
#define DMA_MAX_BURST		_IO(DMA_MAGIC, 6)

//set the address range through which the user address is assumed to already by a physical address
#define DMA_SET_MIN_PHYS	_IOW(DMA_MAGIC, 7, unsigned long)
#define DMA_SET_MAX_PHYS	_IOW(DMA_MAGIC, 8, unsigned long)

#define VIRT_TO_BUS_CACHE_SIZE 8

/***** FILE OPS *****/
static int Open(struct inode *pInode, struct file *pFile);
static int Release(struct inode *pInode, struct file *pFile);
static long Ioctl(struct file *pFile, unsigned int cmd, unsigned long arg);
static ssize_t Read(struct file *pFile, char __user *pUser, size_t count, loff_t *offp);
static int Mmap(struct file *pFile, struct vm_area_struct *pVma);

/***** VMA OPS ****/
static void VmaOpen(struct vm_area_struct *pVma);
static void VmaClose(struct vm_area_struct *pVma);
static int VmaFault(struct vm_area_struct *pVma, struct vm_fault *pVmf);

/**** GENERIC ****/
static int __init dmaer_init(void);
static void __exit dmaer_exit(void);

/*** OPS ***/
static struct vm_operations_struct g_vmOps = {
	.open = VmaOpen,
	.close = VmaClose,
	.fault = VmaFault,
};

static struct file_operations g_fOps = {
	.owner = THIS_MODULE,
	.llseek = 0,
	.read = Read,
	.write = 0,
	.unlocked_ioctl = Ioctl,
	.open = Open,
	.release = Release,
	.mmap = Mmap,
};

/***** GLOBALS ******/
static dev_t g_majorMinor;
static atomic_t g_oneLock = ATOMIC_INIT(1);
static struct cdev g_cDev;
static int g_trackedPages = 0;
static unsigned int *g_pDmaChanBase;
static int g_dmaIrq;
static int g_dmaChan;

static unsigned long g_virtAddr[VIRT_TO_BUS_CACHE_SIZE];
static unsigned long g_busAddr[VIRT_TO_BUS_CACHE_SIZE];
static unsigned long g_cbVirtAddr;
static unsigned long g_cbBusAddr;
static int g_cacheInsertAt;
static int g_cacheHit, g_cacheMiss;

//off by default
static void __user *g_pMinPhys = (void __user *)-1;
static void __user *g_pMaxPhys = (void __user *)0;

/****** CACHE OPERATIONS ********/
static inline void FlushAddrCache(void)
{
	int count = 0;
	for (count = 0; count < VIRT_TO_BUS_CACHE_SIZE; count++)
		g_virtAddr[count] = 0xffffffff;			//never going to match as we always chop the bottom bits anyway

	g_cbVirtAddr = 0xffffffff;

	g_cacheInsertAt = 0;
}

//translate from a user virtual address to a bus address by mapping the page
//NB this won't lock a page in memory, so to avoid potential paging issues using kernel logical addresses
static inline void __iomem *UserVirtualToBus(void __user *pUser)
{
	int mapped;
	struct page *pPage;
	void *phys;

	//map it (requiring that the pointer points to something that does not hang off the page boundary)
	mapped = get_user_pages(current, current->mm,
		(unsigned long)pUser, 1,
		1, 0,
		&pPage,
		0);

	if (mapped <= 0)		//error
		return 0;

	//get the arm physical address
	phys = page_address(pPage) + offset_in_page(pUser);
	page_cache_release(pPage);

	//and now the bus address
	return (void __iomem *)__virt_to_bus(phys);
}

static inline void __iomem *UserVirtualToBusViaCbCache(void __user *pUser)
{
	unsigned long virtual_page = (unsigned long)pUser & ~4095;
	unsigned long page_offset = (unsigned long)pUser & 4095;
	unsigned long bus_addr;

	if (g_cbVirtAddr == virtual_page)
	{
		bus_addr = g_cbBusAddr + page_offset;
		g_cacheHit++;
		return (void __iomem *)bus_addr;
	}
	else
	{
		bus_addr = (unsigned long)UserVirtualToBus(pUser);
		
		if (!bus_addr)
			return 0;
		
		g_cbVirtAddr = virtual_page;
		g_cbBusAddr = bus_addr & ~4095;
		g_cacheMiss++;

		return (void __iomem *)bus_addr;
	}
}

//do the same as above, by query our virt->bus cache
static inline void __iomem *UserVirtualToBusViaCache(void __user *pUser)
{
	int count;
	//get the page and its offset
	unsigned long virtual_page = (unsigned long)pUser & ~4095;
	unsigned long page_offset = (unsigned long)pUser & 4095;
	unsigned long bus_addr;

	if (pUser >= g_pMinPhys && pUser < g_pMaxPhys)
	{
//		printk(KERN_DEBUG "user->phys passthrough on %p\n", pUser);
		return (void __iomem *)__virt_to_bus(pUser);
	}

	//check the cache for our entry
	for (count = 0; count < VIRT_TO_BUS_CACHE_SIZE; count++)
		if (g_virtAddr[count] == virtual_page)
		{
			bus_addr = g_busAddr[count] + page_offset;
			g_cacheHit++;
			return (void __iomem *)bus_addr;
		}

	//not found, look up manually and then insert its page address
	bus_addr = (unsigned long)UserVirtualToBus(pUser);

	if (!bus_addr)
		return 0;

	g_virtAddr[g_cacheInsertAt] = virtual_page;
	g_busAddr[g_cacheInsertAt] = bus_addr & ~4095;

	//round robin
	g_cacheInsertAt++;
	if (g_cacheInsertAt == VIRT_TO_BUS_CACHE_SIZE)
		g_cacheInsertAt = 0;

	g_cacheMiss++;

	return (void __iomem *)bus_addr;
}

/***** FILE OPERATIONS ****/
static int Open(struct inode *pInode, struct file *pFile)
{
	printk(KERN_DEBUG "file opening\n");
	
	//only one at a time
	if (!atomic_dec_and_test(&g_oneLock))
	{
		atomic_inc(&g_oneLock);
		return -EBUSY;
	}
	
	return 0;
}

static int Release(struct inode *pInode, struct file *pFile)
{
	printk(KERN_DEBUG "file closing, %d pages tracked\n", g_trackedPages);
	if (g_trackedPages)
		printk(KERN_ERR "we\'re leaking memory!\n");
	
	atomic_inc(&g_oneLock);
	return 0;
}

static struct DmaControlBlock __user *DmaPrepare(struct DmaControlBlock __user *pUserCB, int *pError)
{
	struct DmaControlBlock kernCB;
	struct DmaControlBlock __user *pUNext;
	void __iomem *pSourceBus, __iomem *pDestBus;
	
	
	//get the control block into kernel memory so we can work on it
	if (copy_from_user(&kernCB, pUserCB, sizeof(struct DmaControlBlock)) != 0)
	{
		printk(KERN_ERR "copy_from_user failed for user cb %p\n", pUserCB);
		*pError = 1;
		return 0;
	}
	
	if (kernCB.m_pSourceAddr == 0 || kernCB.m_pDestAddr == 0)
	{
		printk(KERN_ERR "faulty source (%p) dest (%p) addresses for user cb %p\n",
			kernCB.m_pSourceAddr, kernCB.m_pDestAddr, pUserCB);
		*pError = 1;
		return 0;
	}

	pSourceBus = UserVirtualToBusViaCache(kernCB.m_pSourceAddr);
	pDestBus = UserVirtualToBusViaCache(kernCB.m_pDestAddr);

	if (!pSourceBus || !pDestBus)
	{
		printk(KERN_ERR "virtual to bus translation failure for source/dest\n");
		*pError = 1;
		return 0;
	}
	
	//update the user structure with the new bus addresses
	kernCB.m_pSourceAddr = pSourceBus;
	kernCB.m_pDestAddr = pDestBus;
		
	//sort out the bus address for the next block
	pUNext = kernCB.m_pNext;
	
	if (kernCB.m_pNext)
	{
		void __iomem *pNextBus;
		pNextBus = UserVirtualToBusViaCbCache(kernCB.m_pNext);

		if (!pNextBus)
		{
			printk(KERN_ERR "virtual to bus translation failure for m_pNext\n");
			*pError = 1;
			return 0;
		}

		//update the pointer with the bus address
		kernCB.m_pNext = pNextBus;
	}
	
	//write it back to user space
	if (copy_to_user(pUserCB, &kernCB, sizeof(struct DmaControlBlock)) != 0)
	{
		printk(KERN_ERR "copy_to_user failed for cb %p\n", pUserCB);
		*pError = 1;
		return 0;
	}

	__cpuc_flush_dcache_area(pUserCB, 32);

	*pError = 0;
	return pUNext;
}

static int DmaKick(struct DmaControlBlock __user *pUserCB)
{
	void __iomem *pBusCB;
	
	pBusCB = UserVirtualToBusViaCbCache(pUserCB);
	if (!pBusCB)
	{
		printk(KERN_ERR "virtual to bus translation failure for cb\n");
		return 1;
	}

	//flush_cache_all();

	bcm_dma_start(g_pDmaChanBase, (dma_addr_t)pBusCB);
	
	return 0;
}

static void DmaWaitAll(void)
{
	int counter = 0;
	volatile int inner_count;
	volatile unsigned int cs;
	//unsigned long time_before, time_after;

	//time_before = jiffies;
	//bcm_dma_wait_idle(g_pDmaChanBase);
	dsb();
	
	cs = readl(g_pDmaChanBase);
	
	while ((cs & 1) == 1)
	{
		cs = readl(g_pDmaChanBase);
		counter++;

		for (inner_count = 0; inner_count < 32; inner_count++);

		asm volatile ("MCR p15,0,r0,c7,c0,4 \n");
		//cpu_do_idle();
		if (counter >= 1000000)
		{
			printk(KERN_WARNING "DMA failed to finish in a timely fashion\n");
			break;
		}
	}
	//time_after = jiffies;
	//printk(KERN_DEBUG "done, counter %d, cs %08x", counter, cs);
	//printk(KERN_DEBUG "took %ld jiffies, %d HZ\n", time_after - time_before, HZ);
}

static long Ioctl(struct file *pFile, unsigned int cmd, unsigned long arg)
{
	int error = 0;
	//printk(KERN_DEBUG "ioctl cmd %x arg %lx\n", cmd, arg);

	switch (cmd)
	{
	case DMA_PREPARE:
	case DMA_PREPARE_KICK:
	case DMA_PREPARE_KICK_WAIT:
		{
			struct DmaControlBlock __user *pUCB = (struct DmaControlBlock *)arg;
			int steps = 0;
			//unsigned long start_time = jiffies;

			//flush our address cache
			FlushAddrCache();

//			printk(KERN_DEBUG "dma prepare\n");
			
			//do virtual to bus translation for each entry
			do
			{
				pUCB = DmaPrepare(pUCB, &error);
			} while (error == 0 && ++steps && pUCB);
			//printk(KERN_DEBUG "prepare done in %d steps, %ld\n", steps, jiffies - start_time);

			//carry straight on if we want to kick too
			if (cmd == DMA_PREPARE || error)
			{
//				printk(KERN_DEBUG "falling out\n");
				break;
			}
		};
	case DMA_KICK:
//		printk(KERN_DEBUG "dma begin\n");

		if (cmd == DMA_KICK)
			FlushAddrCache();

		DmaKick((struct DmaControlBlock __user *)arg);
		
		if (cmd != DMA_PREPARE_KICK_WAIT)
			break;
/*	case DMA_WAIT_ONE:
		//printk(KERN_DEBUG "dma wait one\n");
		break;*/
	case DMA_WAIT_ALL:
		//printk(KERN_DEBUG "dma wait all\n");
		DmaWaitAll();
		break;
	case DMA_MAX_BURST:
		if (g_dmaChan == 0)
			return 10;
		else
			return 5;
	case DMA_SET_MIN_PHYS:
		g_pMinPhys = (void __user *)arg;
		printk("min/max user/phys bypass set to %p %p\n", g_pMinPhys, g_pMaxPhys);
		break;
	case DMA_SET_MAX_PHYS:
		g_pMaxPhys = (void __user *)arg;
		printk("min/max user/phys bypass set to %p %p\n", g_pMinPhys, g_pMaxPhys);
		break;
	default:
		printk(KERN_DEBUG "unknown ioctl: %d\n", cmd);
		return -EINVAL;
	}

	return 0;
}

static ssize_t Read(struct file *pFile, char __user *pUser, size_t count, loff_t *offp)
{
	/*printk(KERN_DEBUG "file read pFile %p pUser %p count %ld offp %p\n",
		pFile, pUser, count, offp);
	printk(KERN_DEBUG "phys pFile %lx pUser %lx offp %lx\n",
		__pa(pFile), __pa(pUser), __pa(offp));
	printk(KERN_DEBUG "bus pFile %lx pUser %lx offp %lx\n",
		virt_to_bus(pFile), virt_to_bus(pUser), virt_to_bus(offp));*/
	return -EIO;
}

static int Mmap(struct file *pFile, struct vm_area_struct *pVma)
{
	struct PageList *pPages;
	struct VmaPageList *pVmaList;
	
//	printk(KERN_DEBUG "MMAP vma %p, length %ld (%s %d)\n",
//		pVma, pVma->vm_end - pVma->vm_start,
//		current->comm, current->pid);
//	printk(KERN_DEBUG "MMAP %p %d (tracked %d)\n", pVma, current->pid, g_trackedPages);
	
	//make a new page list
	pPages = (struct PageList *)kmalloc(sizeof(struct PageList), GFP_KERNEL);
	if (!pPages)
	{
		printk(KERN_ERR "couldn\'t allocate a new page list (%s %d)\n",
			current->comm, current->pid);
		return -ENOMEM;
	}
	
	//clear the page list
	pPages->m_used = 0;
	pPages->m_pNext = 0;
	
	//insert our vma and new page list somewhere
	if (!pVma->vm_private_data)
	{
		struct VmaPageList *pList;

//		printk(KERN_DEBUG "new vma list, making new one (%s %d)\n",
//			current->comm, current->pid);
		
		//make a new vma list
		pList = (struct VmaPageList *)kmalloc(sizeof(struct VmaPageList), GFP_KERNEL);
		if (!pList)
		{
			printk(KERN_ERR "couldn\'t allocate vma page list (%s %d)\n",
				current->comm, current->pid);
			kfree(pPages);
			return -ENOMEM;
		}
		
		//clear this list
		pVma->vm_private_data = (void *)pList;
		pList->m_refCount = 0;
	}
	
	pVmaList = (struct VmaPageList *)pVma->vm_private_data;
	
	//add it to the vma list
	pVmaList->m_pPageHead = pPages;
	pVmaList->m_pPageTail = pPages;
	
	pVma->vm_ops = &g_vmOps;
	pVma->vm_flags |= VM_RESERVED;
	
	VmaOpen(pVma);
	
	return 0;
}

/****** VMA OPERATIONS ******/

static void VmaOpen(struct vm_area_struct *pVma)
{
	struct VmaPageList *pVmaList;

//	printk(KERN_DEBUG "vma open %p private %p (%s %d), %d live pages\n", pVma, pVma->vm_private_data, current->comm, current->pid, g_trackedPages);
//	printk(KERN_DEBUG "OPEN %p %d %ld pages (tracked pages %d)\n",
//		pVma, current->pid, (pVma->vm_end - pVma->vm_start) >> 12,
//		g_trackedPages);

	pVmaList = (struct VmaPageList *)pVma->vm_private_data;

	if (pVmaList)
	{
		pVmaList->m_refCount++;
//		printk(KERN_DEBUG "ref count is now %d\n", pVmaList->m_refCount);
	}
//	else
//		printk(KERN_DEBUG "err, open but no vma page list\n");
}

static void VmaClose(struct vm_area_struct *pVma)
{
	struct VmaPageList *pVmaList;
	int freed = 0;
	
//	printk(KERN_DEBUG "vma close %p private %p (%s %d)\n", pVma, pVma->vm_private_data, current->comm, current->pid);
	
	//find our vma in the list
	pVmaList = (struct VmaPageList *)pVma->vm_private_data;

	//may be a fork
	if (pVmaList)
	{
		struct PageList *pPages;
		
		pVmaList->m_refCount--;

		if (pVmaList->m_refCount == 0)
		{
//			printk(KERN_DEBUG "found vma, freeing pages (%s %d)\n",
//				current->comm, current->pid);

			pPages = pVmaList->m_pPageHead;

			if (!pPages)
			{
				printk(KERN_ERR "no page list (%s %d)!\n",
					current->comm, current->pid);
				return;
			}

			while (pPages)
			{
				struct PageList *next;
				int count;

//				printk(KERN_DEBUG "page list (%s %d)\n",
//					current->comm, current->pid);

				next = pPages->m_pNext;
				for (count = 0; count < pPages->m_used; count++)
				{
//					printk(KERN_DEBUG "freeing page %p (%s %d)\n",
//						pPages->m_pPages[count],
//						current->comm, current->pid);
					__free_pages(pPages->m_pPages[count], 0);
					g_trackedPages--;
					freed++;
				}

//				printk(KERN_DEBUG "freeing page list (%s %d)\n",
//					current->comm, current->pid);
				kfree(pPages);
				pPages = next;
			}
			
			//remove our vma from the list
			kfree(pVmaList);
			pVma->vm_private_data = 0;
		}
//		else
//			printk(KERN_DEBUG "ref count is %d, not closing\n", pVmaList->m_refCount);
	}
	else
	{
//		printk(KERN_ERR "uh-oh, vma %p not found (%s %d)!\n", pVma, current->comm, current->pid);
//		printk(KERN_ERR "CLOSE ERR\n");
	}

//	printk(KERN_DEBUG "CLOSE %p %d %d pages (tracked pages %d)",
//		pVma, current->pid, freed, g_trackedPages);

//	printk(KERN_DEBUG "%d pages open\n", g_trackedPages);
}

static int VmaFault(struct vm_area_struct *pVma, struct vm_fault *pVmf)
{
//	printk(KERN_DEBUG "vma fault for vma %p private %p at offset %ld (%s %d)\n", pVma, pVma->vm_private_data, pVmf->pgoff,
//		current->comm, current->pid);
	//printk(KERN_DEBUG "FAULT\n");
	pVmf->page = alloc_page(GFP_KERNEL);
	/*if (pVmf->page)
		printk(KERN_DEBUG "alloc page virtual %p\n", page_address(pVmf->page));*/
	
	if (!pVmf->page)
	{
		printk(KERN_ERR "vma fault oom (%s %d)\n", current->comm, current->pid);
		return VM_FAULT_OOM;
	}
	else
	{
		struct VmaPageList *pVmaList;
		
		get_page(pVmf->page);
		g_trackedPages++;
		
		//find our vma in the list
		pVmaList = (struct VmaPageList *)pVma->vm_private_data;
		
		if (pVmaList)
		{
//			printk(KERN_DEBUG "vma found (%s %d)\n", current->comm, current->pid);

			if (pVmaList->m_pPageTail->m_used == PAGES_PER_LIST)
			{
//				printk(KERN_DEBUG "making new page list (%s %d)\n", current->comm, current->pid);
				//making a new page list
				pVmaList->m_pPageTail->m_pNext = (struct PageList *)kmalloc(sizeof(struct PageList), GFP_KERNEL);
				if (!pVmaList->m_pPageTail->m_pNext)
					return -ENOMEM;
				
				//update the tail pointer
				pVmaList->m_pPageTail = pVmaList->m_pPageTail->m_pNext;
				pVmaList->m_pPageTail->m_used = 0;
				pVmaList->m_pPageTail->m_pNext = 0;
			}

//			printk(KERN_DEBUG "adding page to list (%s %d)\n", current->comm, current->pid);
			
			pVmaList->m_pPageTail->m_pPages[pVmaList->m_pPageTail->m_used] = pVmf->page;
			pVmaList->m_pPageTail->m_used++;
		}
		else
			printk(KERN_ERR "returned page for vma we don\'t know %p (%s %d)\n", pVma, current->comm, current->pid);
		
		return 0;
	}
}

/****** GENERIC FUNCTIONS ******/
static int __init dmaer_init(void)
{
	int result = alloc_chrdev_region(&g_majorMinor, 0, 1, "dmaer");
	if (result < 0)
	{
		printk(KERN_ERR "unable to get major device number\n");
		return result;
	}
	else
		printk(KERN_DEBUG "major device number %d\n", MAJOR(g_majorMinor));
	
	printk(KERN_DEBUG "vma list size %d, page list size %d, page size %ld\n",
		sizeof(struct VmaPageList), sizeof(struct PageList), PAGE_SIZE);
	
	
	//get a dma channel to work with
	result = bcm_dma_chan_alloc(BCM_DMA_FEATURE_FAST, (void **)&g_pDmaChanBase, &g_dmaIrq);
	//result = 0;
	//g_pDmaChanBase = 0xce808000;
	
	if (result < 0)
	{
		printk(KERN_ERR "failed to allocate dma channel\n");
		cdev_del(&g_cDev);
		unregister_chrdev_region(g_majorMinor, 1);
	}
	
	//reset the channel
	printk(KERN_DEBUG "allocated dma channel %d (%p), initial state %08x\n", result, g_pDmaChanBase, *g_pDmaChanBase);
	*g_pDmaChanBase = 1 << 31;
	printk(KERN_DEBUG "post-reset %08x\n", *g_pDmaChanBase);
	
	g_dmaChan = result;

	//clear the cache stats
	g_cacheHit = 0;
	g_cacheMiss = 0;

	//register our device - after this we are go go go
	cdev_init(&g_cDev, &g_fOps);
	g_cDev.owner = THIS_MODULE;
	g_cDev.ops = &g_fOps;
	
	result = cdev_add(&g_cDev, g_majorMinor, 1);
	if (result < 0)
	{
		printk(KERN_ERR "failed to add character device\n");
		unregister_chrdev_region(g_majorMinor, 1);
		bcm_dma_chan_free(g_dmaChan);
		return result;
	}
		
	return 0;
}

static void __exit dmaer_exit(void)
{
	printk(KERN_INFO "closing dmaer device, cache stats: %d hits %d misses\n", g_cacheHit, g_cacheMiss);
	//unregister the device
	cdev_del(&g_cDev);
	unregister_chrdev_region(g_majorMinor, 1);
	//free the dma channel
	bcm_dma_chan_free(g_dmaChan);
}

MODULE_AUTHOR("Simon Hall");
module_init(dmaer_init);
module_exit(dmaer_exit);

