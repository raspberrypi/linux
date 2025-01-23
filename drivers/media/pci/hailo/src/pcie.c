// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2024 Hailo Technologies Ltd. All rights reserved.
 **/

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/pagemap.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#include <linux/dma-direct.h>
#endif

#define KERNEL_CODE	1

#include "hailo_ioctl_common.h"
#include "pcie.h"
#include "nnc.h"
#include "soc.h"
#include "fops.h"
#include "sysfs.h"
#include "utils/logs.h"
#include "utils/compact.h"
#include "vdma/vdma.h"
#include "vdma/memory.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION( 5, 4, 0 )
#include <linux/pci-aspm.h>
#endif

// enum that represents values for the driver parameter to either force buffer from driver , userspace or not force
// and let driver decide
enum hailo_allocate_driver_buffer_driver_param {
    HAILO_NO_FORCE_BUFFER = 0,
    HAILO_FORCE_BUFFER_FROM_USERSPACE = 1,
    HAILO_FORCE_BUFFER_FROM_DRIVER = 2,
};

// Debug flag
static int force_desc_page_size = 0;
static bool g_is_power_mode_enabled = true;
static int force_allocation_from_driver = HAILO_NO_FORCE_BUFFER;
static bool force_hailo10h_legacy_mode = false;
static bool force_boot_linux_from_eemc = false;
static bool support_soft_reset = true;

#define DEVICE_NODE_NAME "hailo"
static int char_major = 0;
static struct class *chardev_class;

static LIST_HEAD(g_hailo_board_list);
static struct semaphore g_hailo_add_board_mutex = __SEMAPHORE_INITIALIZER(g_hailo_add_board_mutex, 1);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22))
#define HAILO_IRQ_FLAGS (SA_SHIRQ | SA_INTERRUPT)
#elif (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 22) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0))
#define HAILO_IRQ_FLAGS (IRQF_SHARED | IRQF_DISABLED)
#else
#define HAILO_IRQ_FLAGS (IRQF_SHARED)
#endif

 /* ****************************
  ******************************* */
bool power_mode_enabled(void)
{
#if !defined(HAILO_EMULATOR)
    return g_is_power_mode_enabled;
#else /* !defined(HAILO_EMULATOR) */
    return false;
#endif /* !defined(HAILO_EMULATOR) */
}


/**
 * Due to an HW bug, on system with low MaxReadReq ( < 512) we need to use different descriptors size.
 * Returns the max descriptor size or 0 on failure.
 */
static int hailo_get_desc_page_size(struct pci_dev *pdev, u32 *out_page_size)
{
    u16 pcie_device_control = 0;
    int err = 0;
    // The default page size must be smaller/equal to 32K (due to PLDA registers limit).
    const u32 max_page_size = 32u * 1024u;
    const u32 defualt_page_size = min((u32)PAGE_SIZE, max_page_size);

    if (force_desc_page_size != 0) {
        // The user given desc_page_size as a module parameter
        if ((force_desc_page_size & (force_desc_page_size - 1)) != 0) {
            pci_err(pdev, "force_desc_page_size must be a power of 2\n");
            return -EINVAL;
        }

        if (force_desc_page_size > max_page_size) {
            pci_err(pdev, "force_desc_page_size %d mustn't be larger than %u", force_desc_page_size, max_page_size);
            return -EINVAL;
        }

        pci_notice(pdev, "Probing: Force setting max_desc_page_size to %d (recommended value is %lu)\n",
            force_desc_page_size, PAGE_SIZE);
        *out_page_size = force_desc_page_size;
        return 0;
    }

    err = pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &pcie_device_control);
    if (err < 0) {
        pci_err(pdev, "Couldn't read DEVCTL capability\n");
        return err;
    }

    switch (pcie_device_control & PCI_EXP_DEVCTL_READRQ) {
    case PCI_EXP_DEVCTL_READRQ_128B:
        pci_notice(pdev, "Probing: Setting max_desc_page_size to 128 (recommended value is %u)\n", defualt_page_size);
        *out_page_size = 128;
        return 0;
    case PCI_EXP_DEVCTL_READRQ_256B:
        pci_notice(pdev, "Probing: Setting max_desc_page_size to 256 (recommended value is %u)\n", defualt_page_size);
        *out_page_size = 256;
        return 0;
    default:
        pci_notice(pdev, "Probing: Setting max_desc_page_size to %u, (page_size=%lu)\n", defualt_page_size, PAGE_SIZE);
        *out_page_size = defualt_page_size;
        return 0;
    };
}

// should be called only from fops_open (once)
struct hailo_pcie_board* hailo_pcie_get_board_index(u32 index)
{
    struct hailo_pcie_board *pBoard, *pRet = NULL;

    down(&g_hailo_add_board_mutex);
    list_for_each_entry(pBoard, &g_hailo_board_list, board_list)
    {
        if ( index == pBoard->board_index )
        {
            atomic_inc(&pBoard->ref_count);
            pRet = pBoard;
            break;
        }
    }
    up(&g_hailo_add_board_mutex);

    return pRet;
}

/**
 * hailo_pcie_disable_aspm - Disable ASPM states
 * @board: pointer to PCI board struct
 * @state: bit-mask of ASPM states to disable
 * @locked: indication if this context holds pci_bus_sem locked.
 *
 * Some devices *must* have certain ASPM states disabled per hardware errata.
 **/
static int hailo_pcie_disable_aspm(struct hailo_pcie_board *board, u16 state, bool locked)
{
    struct pci_dev *pdev = board->pDev;
    struct pci_dev *parent = pdev->bus->self;
    u16 aspm_dis_mask = 0;
    u16 pdev_aspmc = 0;
    u16 parent_aspmc = 0;
    int err = 0;

    switch (state) {
    case PCIE_LINK_STATE_L0S:
        aspm_dis_mask |= PCI_EXP_LNKCTL_ASPM_L0S;
        break;
    case PCIE_LINK_STATE_L1:
        aspm_dis_mask |= PCI_EXP_LNKCTL_ASPM_L1;
        break;
    default:
        break;
    }

    err = pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &pdev_aspmc);
    if (err < 0) {
        hailo_err(board, "Couldn't read LNKCTL capability\n");
        return err;
    }

    pdev_aspmc &= PCI_EXP_LNKCTL_ASPMC;

    if (parent) {
        err = pcie_capability_read_word(parent, PCI_EXP_LNKCTL, &parent_aspmc);
        if (err < 0) {
            hailo_err(board, "Couldn't read slot LNKCTL capability\n");
            return err;
        }
        parent_aspmc &= PCI_EXP_LNKCTL_ASPMC;
    }

    hailo_notice(board, "Disabling ASPM %s %s\n",
        (aspm_dis_mask & PCI_EXP_LNKCTL_ASPM_L0S) ? "L0s" : "",
        (aspm_dis_mask & PCI_EXP_LNKCTL_ASPM_L1) ? "L1" : "");

    // Disable L0s even if it is currently disabled as ASPM states can be enabled by the kernel when changing power modes
#ifdef CONFIG_PCIEASPM
    if (locked) {
        // Older kernel versions (<5.2.21) don't return value for this functions, so we try manual disabling anyway
        (void)pci_disable_link_state_locked(pdev, state);
    } else {
        (void)pci_disable_link_state(pdev, state);
    }

    /* Double-check ASPM control.  If not disabled by the above, the
     * BIOS is preventing that from happening (or CONFIG_PCIEASPM is
     * not enabled); override by writing PCI config space directly.
     */
    err = pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &pdev_aspmc);
    if (err < 0) {
        hailo_err(board, "Couldn't read LNKCTL capability\n");
        return err;
    }
    pdev_aspmc &= PCI_EXP_LNKCTL_ASPMC;

    if (!(aspm_dis_mask & pdev_aspmc)) {
        hailo_notice(board, "Successfully disabled ASPM %s %s\n",
            (aspm_dis_mask & PCI_EXP_LNKCTL_ASPM_L0S) ? "L0s" : "",
            (aspm_dis_mask & PCI_EXP_LNKCTL_ASPM_L1) ? "L1" : "");
        return 0;
    }
#endif

    /* Both device and parent should have the same ASPM setting.
     * Disable ASPM in downstream component first and then upstream.
     */
    err = pcie_capability_clear_word(pdev, PCI_EXP_LNKCTL, aspm_dis_mask);
    if (err < 0) {
        hailo_err(board, "Couldn't read LNKCTL capability\n");
        return err;
    }
    if (parent) {
        err = pcie_capability_clear_word(parent, PCI_EXP_LNKCTL, aspm_dis_mask);
        if (err < 0) {
            hailo_err(board, "Couldn't read slot LNKCTL capability\n");
            return err;
        }
    }
    hailo_notice(board, "Manually disabled ASPM %s %s\n",
        (aspm_dis_mask & PCI_EXP_LNKCTL_ASPM_L0S) ? "L0s" : "",
        (aspm_dis_mask & PCI_EXP_LNKCTL_ASPM_L1) ? "L1" : "");

    return 0;
}

static void hailo_pcie_insert_board(struct hailo_pcie_board* pBoard)
{
    u32 index = 0;
    struct hailo_pcie_board *pCurrent, *pNext;


    down(&g_hailo_add_board_mutex);
    if ( list_empty(&g_hailo_board_list)  ||
            list_first_entry(&g_hailo_board_list, struct hailo_pcie_board, board_list)->board_index > 0)
    {
        pBoard->board_index = 0;
        list_add(&pBoard->board_list, &g_hailo_board_list);

        up(&g_hailo_add_board_mutex);
        return;
    }

    list_for_each_entry_safe(pCurrent, pNext, &g_hailo_board_list, board_list)
    {
        index = pCurrent->board_index+1;
        if( list_is_last(&pCurrent->board_list, &g_hailo_board_list) || (index != pNext->board_index))
        {
            break;
        }
    }

    pBoard->board_index = index;
    list_add(&pBoard->board_list, &pCurrent->board_list);

    up(&g_hailo_add_board_mutex);

    return;
}

static void hailo_pcie_remove_board(struct hailo_pcie_board* pBoard)
{
    down(&g_hailo_add_board_mutex);
    if (pBoard)
    {
        list_del(&pBoard->board_list);
    }
    up(&g_hailo_add_board_mutex);
}

/**
 * Wait until the relevant completion is done.
 *
 * @param completion - pointer to the completion struct to wait for.
 * @param msecs - the amount of time to wait in milliseconds.
 * @return false if timed out, true if completed.
 */
static bool wait_for_firmware_completion(struct completion *completion, unsigned int msecs)
{
    return (0 != wait_for_completion_timeout(completion, msecs_to_jiffies(msecs)));
}

/**
 * Program one FW file descriptors to the vDMA engine.
 *
 * @param dev - pointer to the device struct we are working on.
 * @param boot_dma_state - pointer to the boot dma state struct which includes all of the boot resources.
 * @param file_address - the address of the file in the device memory.
 * @param transfer_buffer - the buffer to program to the vDMA engine.
 * @param channel_index - the index of the channel to program.
 * @param filename - the name of the file to program.
 * @param raise_int_on_completion - true if this is the last descriptors chunk in the specific channel in the boot flow, false otherwise. If true - will enable
 * an IRQ for the relevant channel when the transfer is finished.
 * @return the amount of descriptors programmed on success, negative error code on failure.
 */
static int pcie_vdma_program_one_file_descriptors(struct device *dev, struct hailo_pcie_boot_dma_channel_state *boot_channel_state,
    u32 file_address, struct hailo_vdma_mapped_transfer_buffer transfer_buffer, u8 channel_index, const char *filename, bool raise_int_on_completion)
{
    int device_desc = 0, host_desc = 0;
    enum hailo_vdma_interrupts_domain interrupts_domain = raise_int_on_completion ? HAILO_VDMA_INTERRUPTS_DOMAIN_HOST :
        HAILO_VDMA_INTERRUPTS_DOMAIN_NONE;

    hailo_dev_dbg(dev, "channel_index = %d, file_name = %s, file_address = 0x%x, transfer_buffer.offset = 0x%x,\
        size_to_program = 0x%x, starting_desc/desc_index = 0x%x\n", channel_index, filename, file_address,
        transfer_buffer.offset, transfer_buffer.size, boot_channel_state->desc_program_num);

    // program descriptors
    device_desc = hailo_vdma_program_descriptors_in_chunk(&hailo_pcie_vdma_hw, file_address, transfer_buffer.size,
        &boot_channel_state->device_descriptors_buffer.desc_list, boot_channel_state->desc_program_num,
        (boot_channel_state->device_descriptors_buffer.desc_list.desc_count - 1), channel_index, HAILO_PCI_EP_HOST_DMA_DATA_ID);
    if (device_desc < 0) {
        hailo_dev_err(dev, "Failed to program device descriptors, error = %u\n", device_desc);
        return device_desc;
    }

    host_desc = hailo_vdma_program_descriptors_list(&hailo_pcie_vdma_hw, &boot_channel_state->host_descriptors_buffer.desc_list,
        boot_channel_state->desc_program_num, &transfer_buffer, true, channel_index, interrupts_domain, false);
    if (host_desc < 0) {
        hailo_dev_err(dev, "Failed to program host descriptors, error = %u\n", host_desc);
        return host_desc;
    }
    
    // checks that same amount of decsriptors were programmed on device side and host side
    if (host_desc != device_desc) {
        hailo_dev_err(dev, "Host and device descriptors should be the same\n");
        return -EINVAL;
    }

    return host_desc;
}

/**
 * Program one FW file to the vDMA engine.
 *
 * @param board - pointer to the board struct we are working on.
 * @param boot_dma_state - pointer to the boot dma state struct which includes all of the boot resources.
 * @param file_address - the address of the file in the device memory.
 * @param filename - the name of the file to program.
 * @param raise_int_on_completion - true if this is the last file in the boot flow, false otherwise. uses to enable an IRQ for the
 * relevant channel when the transfer is finished.
 * @return 0 on success, negative error code on failure. at the end of the function the firmware is released.
 */
static int pcie_vdma_program_one_file(struct hailo_pcie_board *board, struct hailo_pcie_boot_dma_state *boot_dma_state, u32 file_address,
    const char *filename, bool raise_int_on_completion)
{
    const struct firmware *firmware = NULL;
    struct hailo_vdma_mapped_transfer_buffer transfer_buffer = {0};
    int desc_programmed = 0;
    int err = 0;
    size_t bytes_copied = 0, remaining_size = 0, data_offset = 0, desc_num_left = 0, current_desc_to_program = 0;

    hailo_notice(board, "Programing file %s for dma transfer\n", filename);

    // load firmware directly without usermode helper for the relevant file
    err = request_firmware_direct(&firmware, filename, board->vdma.dev);
    if (err < 0) {
        hailo_err(board, "Failed to allocate memory for file %s\n", filename);
        return err;
    }

    // set the remaining size as the whole file size to begin with
    remaining_size = firmware->size;

    while (remaining_size > 0) {
        struct hailo_pcie_boot_dma_channel_state *channel = &boot_dma_state->channels[boot_dma_state->curr_channel_index];
        bool is_last_desc_chunk_of_curr_channel = false;
        bool rais_interrupt_on_last_chunk = false;
        
        hailo_dbg(board, "desc_program_num = 0x%x, desc_page_size = 0x%x, on channel = %d\n",
            channel->desc_program_num, HAILO_PCI_OVER_VDMA_PAGE_SIZE, boot_dma_state->curr_channel_index);

        // increment the channel index if the current channel is full
        if ((MAX_SG_DESCS_COUNT - 1) == channel->desc_program_num) {
            boot_dma_state->curr_channel_index++;
            channel = &boot_dma_state->channels[boot_dma_state->curr_channel_index];
            board->fw_boot.boot_used_channel_bitmap |= (1 << boot_dma_state->curr_channel_index);
        }

        // calculate the number of descriptors left to program and the number of bytes left to program
        desc_num_left = (MAX_SG_DESCS_COUNT - 1) - channel->desc_program_num;

        // prepare the transfer buffer to make sure all the fields are initialized
        transfer_buffer.sg_table = &channel->sg_table; 
        transfer_buffer.size = min(remaining_size, (desc_num_left * HAILO_PCI_OVER_VDMA_PAGE_SIZE));
        // no need to check for overflow since the variables are constant and always desc_program_num <= max u16 (65536)
        // & the buffer max size is 256 Mb << 4G (max u32)
        transfer_buffer.offset = (channel->desc_program_num * HAILO_PCI_OVER_VDMA_PAGE_SIZE);

        // check if this is the last descriptor chunk to program in the whole boot flow
        current_desc_to_program = (transfer_buffer.size / HAILO_PCI_OVER_VDMA_PAGE_SIZE);
        is_last_desc_chunk_of_curr_channel = ((MAX_SG_DESCS_COUNT - 1) ==
            (current_desc_to_program + channel->desc_program_num));
        rais_interrupt_on_last_chunk = (is_last_desc_chunk_of_curr_channel || (raise_int_on_completion &&
            (remaining_size == transfer_buffer.size)));

        // try to copy the file to the buffer, if failed, release the firmware and return
        bytes_copied = sg_pcopy_from_buffer(transfer_buffer.sg_table->sgl, transfer_buffer.sg_table->orig_nents,
            &firmware->data[data_offset], transfer_buffer.size, transfer_buffer.offset);
        if (transfer_buffer.size != bytes_copied) {
            hailo_err(board, "There is not enough memory allocated to copy file %s\n", filename);
            release_firmware(firmware);
            return -EFBIG;
        }

        // program the descriptors
        desc_programmed = pcie_vdma_program_one_file_descriptors(&board->pDev->dev, channel, (file_address + data_offset),
            transfer_buffer, boot_dma_state->curr_channel_index, filename, rais_interrupt_on_last_chunk);
        if (desc_programmed < 0) {
            hailo_err(board, "Failed to program descriptors for file %s, on cahnnel = %d\n", filename,
                boot_dma_state->curr_channel_index);
            release_firmware(firmware);
            return desc_programmed;
        }

        // Update remaining size, data_offset and desc_program_num for the next iteration
        remaining_size -= transfer_buffer.size;
        data_offset += transfer_buffer.size;
        channel->desc_program_num += desc_programmed;
    }
    
    hailo_notice(board, "File %s programed successfully\n", filename);

    release_firmware(firmware);

    return desc_programmed;
}

/**
 * Program the entire batch of firmware files to the vDMA engine.
 *
 * @param board - pointer to the board struct we are working on.
 * @param boot_dma_state - pointer to the boot dma state struct which includes all of the boot resources.
 * @param resources - pointer to the hailo_pcie_resources struct.
 * @param stage - the stage to program.
 * @return 0 on success, negative error code on failure.
 */
static long pcie_vdma_program_entire_batch(struct hailo_pcie_board *board, struct hailo_pcie_boot_dma_state *boot_dma_state,
    struct hailo_pcie_resources *resources, u32 stage)
{
    long err = 0;
    int file_index = 0;
    const struct hailo_pcie_loading_stage *stage_info = hailo_pcie_get_loading_stage_info(resources->board_type, stage);
    const struct hailo_file_batch *files_batch = stage_info->batch;
    const u8 amount_of_files = stage_info->amount_of_files_in_stage;
    const char *filename = NULL;
    u32 file_address = 0;

    for (file_index = 0; file_index < amount_of_files; file_index++)
    {
        filename = files_batch[file_index].filename;
        file_address = files_batch[file_index].address;
        
        if (NULL == filename) {
            hailo_err(board, "The amount of files wasn't specified for stage %d\n", stage);
            break;
        }

        err = pcie_vdma_program_one_file(board, boot_dma_state, file_address, filename,
            (file_index == (amount_of_files - 1)));
        if (err < 0) {
            hailo_err(board, "Failed to program file %s\n", filename);
            return err;
        }
    }

    return 0;
}

/**
 * Release noncontinuous memory (virtual continuous memory). (sg table and kernel_addrs)
 *
 * @param dev - pointer to the device struct we are working on.
 * @param sg_table - the sg table to release.
 * @param kernel_addrs - the kernel address to release.
 */
static void pcie_vdma_release_noncontinuous_memory(struct device *dev, struct sg_table *sg_table, void *kernel_addrs)
{
    dma_unmap_sg(dev, sg_table->sgl, sg_table->orig_nents, DMA_TO_DEVICE);
    sg_free_table(sg_table);
    vfree(kernel_addrs);
}

/**
 * Allocate noncontinuous memory (virtual continuous memory).
 *
 * @param dev - pointer to the device struct we are working on.
 * @param buffer_size - the size of the buffer to allocate.
 * @param kernel_addrs - pointer to the allocated buffer.
 * @param sg_table - pointer to the sg table struct.
 * @return 0 on success, negative error code on failure. on failure all resurces are released. (pages array, sg table, kernel_addrs)
 */
static long pcie_vdma_allocate_noncontinuous_memory(struct device *dev, u64 buffer_size, void **kernel_addrs, struct sg_table *sg_table)
{
    struct page **pages = NULL;
    size_t npages = 0;
    struct scatterlist *sgl = NULL;
    long err = 0;
    size_t i = 0;

    // allocate noncontinuous memory for the kernel address (virtual continuous memory)
    *kernel_addrs = vmalloc(buffer_size);
    if (NULL == *kernel_addrs) {
        hailo_dev_err(dev, "Failed to allocate memory for kernel_addrs\n");
        err = -ENOMEM;
        goto exit;
    }

    // map the memory to pages
    npages = DIV_ROUND_UP(buffer_size, PAGE_SIZE);

    // allocate memory for a virtually contiguous array for the pages
    pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
    if (!pages) {
        err = -ENOMEM;
        hailo_dev_err(dev, "Failed to allocate memory for pages\n");
        goto release_user_addrs;
    }

    // walk a vmap address to the struct page it maps
    for (i = 0; i < npages; i++) {
        pages[i] = vmalloc_to_page(*kernel_addrs + (i * PAGE_SIZE));
        if (!pages[i]) {
            err = -ENOMEM;
            hailo_dev_err(dev, "Failed to get page from vmap address\n");
            goto release_array;
        }
    }

    // allocate and initialize the sg table from a list of pages
    sgl = sg_alloc_table_from_pages_segment_compat(sg_table, pages, npages, 0, buffer_size, SGL_MAX_SEGMENT_SIZE, NULL,
        0, GFP_KERNEL);
    if (IS_ERR(sgl)) {
        err = PTR_ERR(sgl);
        hailo_dev_err(dev, "sg table alloc failed (err %ld)..\n", err);
        goto release_array;
    }

    // map the sg list
    sg_table->nents = dma_map_sg(dev, sg_table->sgl, sg_table->orig_nents, DMA_TO_DEVICE);
    if (0 == sg_table->nents) {
        hailo_dev_err(dev, "failed to map sg list for user buffer\n");
        err = -ENXIO;
        goto release_sg_table;
    }

    // clean exit - just release the pages array & return err = 0
    err = 0;
    kfree(pages);
    goto exit;

release_sg_table:
    dma_unmap_sg(dev, sg_table->sgl, sg_table->orig_nents, DMA_TO_DEVICE);
release_array:
    kfree(pages);
release_user_addrs:
    vfree(*kernel_addrs);
exit:
    return err;
}

/**
 * Release all boot resources.
 *
 * @param board - pointer to the board struct we are working on.
 * @param engine - pointer to the vdma engine struct.
 * @param boot_dma_state - pointer to the boot dma state struct which includes all of the boot resources.
 */
static void pcie_vdme_release_boot_resources(struct hailo_pcie_board *board, struct hailo_vdma_engine *engine,
    struct hailo_pcie_boot_dma_state *boot_dma_state)
{
    u8 channel_index = 0;

    // release all the resources
    for (channel_index = 0; channel_index < HAILO_PCI_OVER_VDMA_NUM_CHANNELS; channel_index++) {
        struct hailo_pcie_boot_dma_channel_state *channel = &boot_dma_state->channels[channel_index];
        // release descriptor lists
        if (channel->host_descriptors_buffer.kernel_address != NULL) {
            hailo_desc_list_release(&board->pDev->dev, &channel->host_descriptors_buffer);
        }
        if (channel->device_descriptors_buffer.kernel_address != NULL) {
            hailo_desc_list_release(&board->pDev->dev, &channel->device_descriptors_buffer);
        }

        // stops all boot vDMA channels
        hailo_vdma_stop_channel(engine->channels[channel_index].host_regs);
        hailo_vdma_stop_channel(engine->channels[channel_index].device_regs);

        // release noncontinuous memory (virtual continuous memory)
        if (channel->kernel_addrs != NULL) {
            pcie_vdma_release_noncontinuous_memory(&board->pDev->dev, &channel->sg_table, channel->kernel_addrs);
        }
    }
}

/**
 * Allocate boot resources for vDMA transfer.
 *
 * @param desc_page_size - the size of the descriptor page.
 * @param board - pointer to the board struct we are working on.
 * @param boot_dma_state - pointer to the boot dma state struct which includes all of the boot resources.
 * @param engine - pointer to the vDMA engine struct.
 * @return 0 on success, negative error code on failure. in case of failure descriptor lists are released,
 *  boot vDMA channels are stopped and memory is released. 
 */
static long pcie_vdme_allocate_boot_resources(u32 desc_page_size, struct hailo_pcie_board *board,
    struct hailo_pcie_boot_dma_state *boot_dma_state, struct hailo_vdma_engine *engine)
{
    long err = 0;
    uintptr_t device_handle = 0, host_handle = 0;
    u8 channel_index = 0;
    
    for (channel_index = 0; channel_index < HAILO_PCI_OVER_VDMA_NUM_CHANNELS; channel_index++) {    
        struct hailo_pcie_boot_dma_channel_state *channel = &boot_dma_state->channels[channel_index];

        // create 2 descriptors list - 1 for the host & 1 for the device for each channel
        err = hailo_desc_list_create(&board->pDev->dev, MAX_SG_DESCS_COUNT, desc_page_size, host_handle, false, 
            &channel->host_descriptors_buffer);
        if (err < 0) {
            hailo_err(board, "failed to allocate host descriptors list buffer\n");
            goto release_all_resources;
        }

        err = hailo_desc_list_create(&board->pDev->dev, MAX_SG_DESCS_COUNT, desc_page_size, device_handle, false,
            &channel->device_descriptors_buffer);
        if (err < 0) {
            hailo_err(board, "failed to allocate device descriptors list buffer\n");
            goto release_all_resources;
        }

        // start vDMA channels - both sides with DDR at the host side (AKA ID 0)
        err = hailo_vdma_start_channel(engine->channels[channel_index].host_regs,
            channel->host_descriptors_buffer.dma_address,
            channel->host_descriptors_buffer.desc_list.desc_count, board->vdma.hw->ddr_data_id);
        if (err < 0) {
            hailo_err(board, "Error starting host vdma channel\n");
            goto release_all_resources;
        }

        err = hailo_vdma_start_channel(engine->channels[channel_index].device_regs,
            channel->device_descriptors_buffer.dma_address,
            channel->device_descriptors_buffer.desc_list.desc_count, board->vdma.hw->ddr_data_id);
        if (err < 0) {
            hailo_err(board, "Error starting device vdma channel\n");
            goto release_all_resources;
        }

        // initialize the buffer size per channel
        channel->buffer_size = (MAX_SG_DESCS_COUNT * desc_page_size);

        // allocate noncontinuous memory (virtual continuous memory)
        err = pcie_vdma_allocate_noncontinuous_memory(&board->pDev->dev, channel->buffer_size, &channel->kernel_addrs,
            &channel->sg_table);
        if (err < 0) {
            hailo_err(board, "Failed to allocate noncontinuous memory\n");
            goto release_all_resources;
        }
    }

    return 0;

release_all_resources:
    pcie_vdme_release_boot_resources(board, engine, boot_dma_state);
    return err;
}

/**
 * Write FW boot files over vDMA using multiple channels for timing optimizations.
 * 
 * The function is divided into the following steps:
 * 1) Allocate resources for the boot process.
 * 2) Programs descriptors to point to the memory and start the vDMA.
 * 3) Waits until the vDMA is done and triggers the device to start the boot process.
 * 4) Releases all the resources.
 *
 * @param board - pointer to the board struct.
 * @param stage - the stage of the boot process.
 * @param desc_page_size - the size of the descriptor page.
 * @return 0 on success, negative error code on failure. in any case all resurces are released.
 */
static long pcie_write_firmware_batch_over_dma(struct hailo_pcie_board *board, u32 stage, u32 desc_page_size)
{
    long err = 0;
    struct hailo_vdma_engine *engine = &board->vdma.vdma_engines[PCI_VDMA_ENGINE_INDEX];
    u8 channel_index = 0;

    err = pcie_vdme_allocate_boot_resources(desc_page_size, board, &board->fw_boot.boot_dma_state, engine);
    if (err < 0) {
        hailo_err(board, "Failed to create descriptors and start channels\n");
        return err;
    }

    // initialize the completion for the vDMA boot data completion
    reinit_completion(&board->fw_boot.vdma_boot_completion);

    err = pcie_vdma_program_entire_batch(board, &board->fw_boot.boot_dma_state, &board->pcie_resources, stage);
    if (err < 0) {
        hailo_err(board, "Failed to program entire batch\n");
        goto release_all;
    }

    // sync the sg tables for the device before statirng the vDMA
    for (channel_index = 0; channel_index < HAILO_PCI_OVER_VDMA_NUM_CHANNELS; channel_index++) {
        dma_sync_sgtable_for_device(&board->pDev->dev, &board->fw_boot.boot_dma_state.channels[channel_index].sg_table,
        DMA_TO_DEVICE);
    }

    // start the vDMA transfer on all channels
    for (channel_index = 0; channel_index < HAILO_PCI_OVER_VDMA_NUM_CHANNELS; channel_index++) {
        struct hailo_pcie_boot_dma_channel_state *channel = &board->fw_boot.boot_dma_state.channels[channel_index];
        if (channel->desc_program_num != 0) {
            hailo_vdma_set_num_avail(engine->channels[channel_index].host_regs, channel->desc_program_num);
            hailo_vdma_set_num_avail(engine->channels[channel_index].device_regs, channel->desc_program_num);
            hailo_dbg(board, "Set num avail to %u, on channel %u\n", channel->desc_program_num, channel_index);
        }
    }

    if (!wait_for_firmware_completion(&board->fw_boot.vdma_boot_completion, hailo_pcie_get_loading_stage_info(board->pcie_resources.board_type, SECOND_STAGE)->timeout)) {
        hailo_err(board, "Timeout waiting for vDMA boot data completion\n");
        err = -ETIMEDOUT;
        goto release_all;
    }

    hailo_notice(board, "vDMA transfer completed, triggering boot\n");
    reinit_completion(&board->fw_boot.fw_loaded_completion);
    hailo_trigger_firmware_boot(&board->pcie_resources, stage);

release_all:
    pcie_vdme_release_boot_resources(board, engine, &board->fw_boot.boot_dma_state);
    return err;
}

static int load_soc_firmware(struct hailo_pcie_board *board, struct hailo_pcie_resources *resources,
    struct device *dev, struct completion *fw_load_completion)
{
    u32 boot_status = 0;
    int err = 0;
    u32 second_stage = force_boot_linux_from_eemc ? SECOND_STAGE_LINUX_IN_EMMC : SECOND_STAGE;

    if (hailo_pcie_is_firmware_loaded(resources)) {
        hailo_dev_warn(dev, "SOC Firmware batch was already loaded\n");
        return 0;
    }

    // configure the EP registers for the DMA transaction
    hailo_pcie_configure_ep_registers_for_dma_transaction(resources);

    init_completion(fw_load_completion);
    init_completion(&board->fw_boot.vdma_boot_completion);

    err = hailo_pcie_write_firmware_batch(dev, resources, FIRST_STAGE);
    if (err < 0) {
        hailo_dev_err(dev, "Failed writing SOC FIRST_STAGE firmware files. err %d\n", err);
        return err;
    }

    if (!wait_for_firmware_completion(fw_load_completion, hailo_pcie_get_loading_stage_info(resources->board_type, FIRST_STAGE)->timeout)) {
        boot_status = hailo_get_boot_status(resources);
        hailo_dev_err(dev, "Timeout waiting for SOC FIRST_STAGE firmware file, boot status %u\n", boot_status);
        return -ETIMEDOUT;
    }

    reinit_completion(fw_load_completion);
    
    err = (int)pcie_write_firmware_batch_over_dma(board, second_stage, HAILO_PCI_OVER_VDMA_PAGE_SIZE);
    if (err < 0) {
        hailo_dev_err(dev, "Failed writing SOC SECOND_STAGE firmware files over vDMA. err %d\n", err);
        return err;
    }

    if (!wait_for_firmware_completion(fw_load_completion, hailo_pcie_get_loading_stage_info(resources->board_type, SECOND_STAGE)->timeout)) {
        boot_status = hailo_get_boot_status(resources);
        hailo_dev_err(dev, "Timeout waiting for SOC SECOND_STAGE firmware file, boot status %u\n", boot_status);
        return -ETIMEDOUT;
    }

    reinit_completion(fw_load_completion);
    reinit_completion(&board->fw_boot.vdma_boot_completion);

    hailo_dev_notice(dev, "SOC Firmware Batch loaded successfully\n");

    return 0;
}
static int load_nnc_firmware(struct hailo_pcie_board *board)
{
    u32 boot_status = 0;
    int err = 0;
    struct device *dev = &board->pDev->dev;

    if (hailo_pcie_is_firmware_loaded(&board->pcie_resources)) {
        if (support_soft_reset) {
            err = hailo_pcie_soft_reset(&board->pcie_resources, &board->soft_reset.reset_completed); // send control, wait for done
            if (err < 0) {
                hailo_dev_err(dev, "Failed hailo pcie soft reset. err %d\n", err);
                return 0;
            }
            hailo_dev_notice(dev, "Soft reset done\n");
        } else {
            hailo_dev_warn(dev, "NNC Firmware batch was already loaded\n");
            return 0;
        }
    }

    init_completion(&board->fw_boot.fw_loaded_completion);

    err = hailo_pcie_write_firmware_batch(dev, &board->pcie_resources, FIRST_STAGE);
    if (err < 0) {
        hailo_dev_err(dev, "Failed writing NNC firmware files. err %d\n", err);
        return err;
    }

    if (!wait_for_firmware_completion(&board->fw_boot.fw_loaded_completion, hailo_pcie_get_loading_stage_info(board->pcie_resources.board_type, FIRST_STAGE)->timeout)) {
        boot_status = hailo_get_boot_status(&board->pcie_resources);
        hailo_dev_err(dev, "Timeout waiting for NNC firmware file, boot status %u\n", boot_status);
        return -ETIMEDOUT;
    }

    hailo_dev_notice(dev, "NNC Firmware loaded successfully\n");

    return 0;
}

int hailo_pcie_soft_reset(struct hailo_pcie_resources *resources, struct completion *reset_completed)
{
    bool completion_result = false;
    int err = 0;

    hailo_pcie_write_firmware_soft_reset(resources);

    reinit_completion(reset_completed);

    // Wait for response
    completion_result =
        wait_for_firmware_completion(reset_completed, msecs_to_jiffies(FIRMWARE_WAIT_TIMEOUT_MS));
    if (completion_result == false) {
        pr_warn("hailo reset firmware, timeout waiting for shutdown response (timeout_ms=%d)\n", FIRMWARE_WAIT_TIMEOUT_MS);
        err = -ETIMEDOUT;
        return err;
    }

    msleep(TIME_UNTIL_REACH_BOOTLOADER);
    pr_notice("hailo_driver_down finished\n");

    return err;
}

static int load_firmware(struct hailo_pcie_board *board)
{
    switch (board->pcie_resources.accelerator_type) {
    case HAILO_ACCELERATOR_TYPE_SOC:
        return load_soc_firmware(board, &board->pcie_resources, &board->pDev->dev, &board->fw_boot.fw_loaded_completion);
    case HAILO_ACCELERATOR_TYPE_NNC:
        return load_nnc_firmware(board);
    default:
        hailo_err(board, "Invalid board type %d\n", board->pcie_resources.accelerator_type);
        return -EINVAL;
    }
}

static int enable_boot_interrupts(struct hailo_pcie_board *board)
{
    int err = hailo_enable_interrupts(board);
    if (err < 0) {
        hailo_err(board, "Failed enabling interrupts %d\n", err);
        return err;
    }

    board->fw_boot.is_in_boot = true;
    return 0;
}

static void disable_boot_interrupts(struct hailo_pcie_board *board)
{
    board->fw_boot.is_in_boot = false;
    hailo_disable_interrupts(board);
}

static int hailo_activate_board(struct hailo_pcie_board *board)
{
    int err = 0;
    ktime_t start_time = 0, end_time = 0;

    (void)hailo_pcie_disable_aspm(board, PCIE_LINK_STATE_L0S, false);

    err = enable_boot_interrupts(board);
    if (err < 0) {
        return err;
    }

    start_time = ktime_get();
    err = load_firmware(board);
    end_time = ktime_get();
    hailo_notice(board, "FW loaded, took %lld ms\n", ktime_to_ms(ktime_sub(end_time, start_time)));
    disable_boot_interrupts(board);

    if (err < 0) {
        hailo_err(board, "Firmware load failed\n");
        return err;
    }

    if (power_mode_enabled()) {
        // Setting the device to low power state, until the user opens the device
        hailo_info(board, "Power change state  to PCI_D3hot\n");
        err = pci_set_power_state(board->pDev, PCI_D3hot);
        if (err < 0) {
            hailo_err(board, "Set power state failed %d\n", err);
            return err;
        }
    }

    return 0;
}

int hailo_enable_interrupts(struct hailo_pcie_board *board)
{
    int err = 0;

    if (board->interrupts_enabled) {
        hailo_crit(board, "Failed enabling interrupts (already enabled)\n");
        return -EINVAL;
    }

    // TODO HRT-2253: use new api for enabling msi: (pci_alloc_irq_vectors)
    if ((err = pci_enable_msi(board->pDev))) {
        hailo_err(board, "Failed to enable MSI %d\n", err);
        return err;
    }
    hailo_info(board, "Enabled MSI interrupt\n");

    err = request_irq(board->pDev->irq, hailo_irqhandler, HAILO_IRQ_FLAGS, DRIVER_NAME, board);
    if (err) {
        hailo_err(board, "request_irq failed %d\n", err);
        pci_disable_msi(board->pDev);
        return err;
    }
    hailo_info(board, "irq enabled %u\n", board->pDev->irq);

    hailo_pcie_enable_interrupts(&board->pcie_resources);

    board->interrupts_enabled = true;
    return 0;
}

void hailo_disable_interrupts(struct hailo_pcie_board *board)
{
    // Sanity Check
    if ((NULL == board) || (NULL == board->pDev)) {
        pr_err("Failed to access board or device\n");
        return;
    }

    if (!board->interrupts_enabled) {
        return;
    }

    board->interrupts_enabled = false;
    hailo_pcie_disable_interrupts(&board->pcie_resources);
    free_irq(board->pDev->irq, board);
    pci_disable_msi(board->pDev);
}

static int hailo_bar_iomap(struct pci_dev *pdev, int bar, struct hailo_resource *resource)
{
    resource->size = pci_resource_len(pdev, bar);
    resource->address = (uintptr_t)(pci_iomap(pdev, bar, resource->size));

    if (!resource->size || !resource->address) {
        pci_err(pdev, "Probing: Invalid PCIe BAR %d", bar);
        return -EINVAL;
    }

    pci_notice(pdev, "Probing: mapped bar %d - %p %zu\n", bar,
        (void*)resource->address, resource->size);
    return 0;
}

static void hailo_bar_iounmap(struct pci_dev *pdev, struct hailo_resource *resource)
{
    if (resource->address) {
        pci_iounmap(pdev, (void*)resource->address);
        resource->address = 0;
        resource->size = 0;
    }
}

static int pcie_resources_init(struct pci_dev *pdev, struct hailo_pcie_resources *resources,
    enum hailo_board_type board_type)
{
    int err = -EINVAL;
    if (board_type >= HAILO_BOARD_TYPE_COUNT) {
        pci_err(pdev, "Probing: Invalid board type %d\n", (int)board_type);
        err = -EINVAL;
        goto failure_exit;
    }

    err = pci_request_regions(pdev, DRIVER_NAME);
    if (err < 0) {
        pci_err(pdev, "Probing: Error allocating bars %d\n", err);
        goto failure_exit;
    }

    err = hailo_bar_iomap(pdev, HAILO_PCIE_CONFIG_BAR, &resources->config);
    if (err < 0) {
        goto failure_release_regions;
    }

    err = hailo_bar_iomap(pdev, HAILO_PCIE_VDMA_REGS_BAR, &resources->vdma_registers);
    if (err < 0) {
        goto failure_release_config;
    }

    err = hailo_bar_iomap(pdev, HAILO_PCIE_FW_ACCESS_BAR, &resources->fw_access);
    if (err < 0) {
        goto failure_release_vdma_regs;
    }


    if (HAILO_BOARD_TYPE_HAILO10H == board_type){
        if (true == force_hailo10h_legacy_mode) {
            board_type = HAILO_BOARD_TYPE_HAILO10H_LEGACY;
        }
    }

    resources->board_type = board_type;

    err = hailo_set_device_type(resources);
    if (err < 0) {
        goto failure_release_fw_access;
    }

    if (!hailo_pcie_is_device_connected(resources)) {
        pci_err(pdev, "Probing: Failed reading device BARs, device may be disconnected\n");
        err = -ENODEV;
        goto failure_release_fw_access;
    }

    return 0;

failure_release_fw_access:
    hailo_bar_iounmap(pdev, &resources->fw_access);
failure_release_vdma_regs:
    hailo_bar_iounmap(pdev, &resources->vdma_registers);
failure_release_config:
    hailo_bar_iounmap(pdev, &resources->config);
failure_release_regions:
    pci_release_regions(pdev);
failure_exit:
    return err;
}

static void pcie_resources_release(struct pci_dev *pdev, struct hailo_pcie_resources *resources)
{
    hailo_bar_iounmap(pdev, &resources->config);
    hailo_bar_iounmap(pdev, &resources->vdma_registers);
    hailo_bar_iounmap(pdev, &resources->fw_access);
    pci_release_regions(pdev);
}

static void update_channel_interrupts(struct hailo_vdma_controller *controller,
    size_t engine_index, u32 channels_bitmap)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board*) dev_get_drvdata(controller->dev);
    if (engine_index >= board->vdma.vdma_engines_count) {
        hailo_err(board, "Invalid engine index %zu", engine_index);
        return;
    }

    hailo_pcie_update_channel_interrupts_mask(&board->pcie_resources, channels_bitmap);
}

static struct hailo_vdma_controller_ops pcie_vdma_controller_ops = {
    .update_channel_interrupts = update_channel_interrupts,
};


static int hailo_pcie_vdma_controller_init(struct hailo_vdma_controller *controller,
    struct device *dev, struct hailo_resource *vdma_registers)
{
    const size_t engines_count = 1;
    return hailo_vdma_controller_init(controller, dev, &hailo_pcie_vdma_hw,
        &pcie_vdma_controller_ops, vdma_registers, engines_count);
}

// Tries to check if address allocated with kmalloc is dma capable.
// If kmalloc address is not dma capable we assume other addresses
// won't be dma capable as well.
static bool is_kmalloc_dma_capable(struct device *dev)
{
    void *check_addr = NULL;
    dma_addr_t dma_addr = 0;
    phys_addr_t phys_addr = 0;
    bool capable = false;

    if (!dev->dma_mask) {
        return false;
    }

    check_addr = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (NULL == check_addr) {
        dev_err(dev, "failed allocating page!\n");
        return false;
    }

    phys_addr = virt_to_phys(check_addr);
    dma_addr = phys_to_dma(dev, phys_addr);

    capable = is_dma_capable(dev, dma_addr, PAGE_SIZE);
    kfree(check_addr);
    return capable;
}

static int hailo_get_allocation_mode(struct pci_dev *pdev, enum hailo_allocation_mode *allocation_mode)
{
    // Check if module paramater was given to override driver choice
    if (HAILO_NO_FORCE_BUFFER != force_allocation_from_driver) {
        if (HAILO_FORCE_BUFFER_FROM_USERSPACE == force_allocation_from_driver) {
            *allocation_mode = HAILO_ALLOCATION_MODE_USERSPACE;
            pci_notice(pdev, "Probing: Using userspace allocated vdma buffers\n");
        }
        else if (HAILO_FORCE_BUFFER_FROM_DRIVER == force_allocation_from_driver) {
            *allocation_mode = HAILO_ALLOCATION_MODE_DRIVER;
            pci_notice(pdev, "Probing: Using driver allocated vdma buffers\n");
        }
        else {
            pci_err(pdev, "Invalid value for force allocation driver paramater - value given: %d!\n",
                force_allocation_from_driver);
            return -EINVAL;
        }

        return 0;
    }

    if (is_kmalloc_dma_capable(&pdev->dev)) {
        *allocation_mode = HAILO_ALLOCATION_MODE_USERSPACE;
        pci_notice(pdev, "Probing: Using userspace allocated vdma buffers\n");
    } else {
        *allocation_mode = HAILO_ALLOCATION_MODE_DRIVER;
        pci_notice(pdev, "Probing: Using driver allocated vdma buffers\n");
    }

    return 0;
}

static int hailo_pcie_probe(struct pci_dev* pDev, const struct pci_device_id* id)
{
    struct hailo_pcie_board * pBoard;
    struct device *char_device = NULL;
    int err = -EINVAL;

    pci_notice(pDev, "Probing on: %04x:%04x...\n", pDev->vendor, pDev->device);
#ifdef HAILO_EMULATOR
    pci_notice(pDev, "PCIe driver was compiled in emulator mode\n");
#endif /* HAILO_EMULATOR */
    if (!g_is_power_mode_enabled) {
        pci_notice(pDev, "PCIe driver was compiled with power modes disabled\n");
    }

    /* Initialize device extension for the board*/
    pci_notice(pDev, "Probing: Allocate memory for device extension, %zu\n", sizeof(struct hailo_pcie_board));
    pBoard = (struct hailo_pcie_board*) kzalloc( sizeof(struct hailo_pcie_board), GFP_KERNEL);
    if (pBoard == NULL)
    {
        pci_err(pDev, "Probing: Failed to allocate memory for device extension structure\n");
        err = -ENOMEM;
        goto probe_exit;
    }

    pBoard->pDev = pDev;

    if ( (err = pci_enable_device(pDev)) )
    {
        pci_err(pDev, "Probing: Failed calling pci_enable_device %d\n", err);
        goto probe_free_board;
    }
    pci_notice(pDev, "Probing: Device enabled\n");

    pci_set_master(pDev);

    err = pcie_resources_init(pDev, &pBoard->pcie_resources, id->driver_data);
    if (err < 0) {
        pci_err(pDev, "Probing: Failed init pcie resources");
        goto probe_disable_device;
    }

    err = hailo_get_desc_page_size(pDev, &pBoard->desc_max_page_size);
    if (err < 0) {
        goto probe_release_pcie_resources;
    }

    pBoard->interrupts_enabled = false;
    pBoard->fw_boot.is_in_boot = false;
    init_completion(&pBoard->fw_boot.fw_loaded_completion);

    sema_init(&pBoard->mutex, 1);
    atomic_set(&pBoard->ref_count, 0);
    INIT_LIST_HEAD(&pBoard->open_files_list);

    // Init both soc and nnc, since the interrupts are shared.
    hailo_nnc_init(&pBoard->nnc);
    hailo_soc_init(&pBoard->soc);

    init_completion(&pBoard->driver_down.reset_completed);
    init_completion(&pBoard->soft_reset.reset_completed);

    memset(&pBoard->memory_transfer_params, 0, sizeof(pBoard->memory_transfer_params));

    err = hailo_pcie_vdma_controller_init(&pBoard->vdma, &pBoard->pDev->dev,
        &pBoard->pcie_resources.vdma_registers);
    if (err < 0) {
        hailo_err(pBoard, "Failed init vdma controller %d\n", err);
        goto probe_release_pcie_resources;
    }

    // Checks the dma mask => it must be called after the device's dma_mask is set by hailo_pcie_vdma_controller_init
    err = hailo_get_allocation_mode(pDev, &pBoard->allocation_mode);
    if (err < 0) {
        pci_err(pDev, "Failed determining allocation of buffers from driver. error type: %d\n", err);
        goto probe_release_pcie_resources;
    }

    // Initialize the boot channel bitmap to 1 since channel 0 is always used for boot 
    // (we will always use at least 1 channel which is LSB in the bitmap)
    pBoard->fw_boot.boot_used_channel_bitmap = (1 << 0);
    memset(&pBoard->fw_boot.boot_dma_state, 0, sizeof(pBoard->fw_boot.boot_dma_state));
    err = hailo_activate_board(pBoard);
    if (err < 0) {
        hailo_err(pBoard, "Failed activating board %d\n", err);
        goto probe_release_pcie_resources;
    }

    /* Keep track on the device, in order, to be able to remove it later */
    pci_set_drvdata(pDev, pBoard);
    hailo_pcie_insert_board(pBoard);

    /* Create dynamically the device node*/
    char_device = device_create_with_groups(chardev_class, NULL,
                                            MKDEV(char_major, pBoard->board_index),
                                            pBoard,
                                            g_hailo_dev_groups,
                                            DEVICE_NODE_NAME"%d", pBoard->board_index);
    if (IS_ERR(char_device)) {
        hailo_err(pBoard, "Failed creating dynamic device %d\n", pBoard->board_index);
        err = PTR_ERR(char_device);
        goto probe_remove_board;
    }

    hailo_notice(pBoard, "Probing: Added board %0x-%0x, /dev/hailo%d\n", pDev->vendor, pDev->device, pBoard->board_index);

    return 0;

probe_remove_board:
    hailo_pcie_remove_board(pBoard);

probe_release_pcie_resources:
    pcie_resources_release(pBoard->pDev, &pBoard->pcie_resources);

probe_disable_device:
    pci_disable_device(pDev);

probe_free_board:
    kfree(pBoard);

probe_exit:

    return err;
}

static void hailo_pcie_remove(struct pci_dev* pDev)
{
    struct hailo_pcie_board* pBoard = (struct hailo_pcie_board*) pci_get_drvdata(pDev);

    pci_notice(pDev, "Remove: Releasing board\n");

    if (pBoard)
    {

        // lock board to wait for any pending operations and for synchronization with open
        down(&pBoard->mutex);


        // remove board from active boards list
        hailo_pcie_remove_board(pBoard);


        /* Delete the device node */
        device_destroy(chardev_class, MKDEV(char_major, pBoard->board_index));

        // disable interrupts - will only disable if they have not been disabled in release already
        hailo_disable_interrupts(pBoard);

        pcie_resources_release(pBoard->pDev, &pBoard->pcie_resources);

        // deassociate device from board to be picked up by char device
        pBoard->pDev = NULL;

        pBoard->vdma.dev = NULL;

        pci_disable_device(pDev);

        pci_set_drvdata(pDev, NULL);

        hailo_nnc_finalize(&pBoard->nnc);

        up(&pBoard->mutex);

        if ( 0 == atomic_read(&pBoard->ref_count) )
        {
            // nobody has the board open - free
            pci_notice(pDev, "Remove: Freed board, /dev/hailo%d\n", pBoard->board_index);
            kfree(pBoard);
        }
        else
        {
            // board resources are freed on last close
            pci_notice(pDev, "Remove: Scheduled for board removal, /dev/hailo%d\n", pBoard->board_index);
        }
    }

}

inline int driver_down(struct hailo_pcie_board *board)
{
    if (board->pcie_resources.accelerator_type == HAILO_ACCELERATOR_TYPE_NNC) {
        return hailo_nnc_driver_down(board);
    } else {
        return hailo_soc_driver_down(board);
    }
}

#ifdef CONFIG_PM_SLEEP
static int hailo_pcie_suspend(struct device *dev)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board*) dev_get_drvdata(dev);
    struct hailo_file_context *cur = NULL;
    int err = 0;

    // lock board to wait for any pending operations
    down(&board->mutex);

    if (board->vdma.used_by_filp != NULL) {
        err = driver_down(board);
        if (err < 0) {
            dev_notice(dev, "Error while trying to call FW to close vdma channels\n");
        }
    }

    // Disable all interrupts. All interrupts from Hailo chip would be masked.
    hailo_disable_interrupts(board);

    // Un validate all activae file contexts so every new action would return error to the user.
    list_for_each_entry(cur, &board->open_files_list, open_files_list) {
        cur->is_valid = false;
    }

    // Release board
    up(&board->mutex);

    dev_notice(dev, "PM's suspend\n");
    // Success Oriented - Continue system suspend even in case of error (otherwise system will not suspend correctly)
    return 0;
}

static int hailo_pcie_resume(struct device *dev)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board*) dev_get_drvdata(dev);
    int err = 0;

    if ((err = hailo_activate_board(board)) < 0) {
        dev_err(dev, "Failed activating board %d\n", err);
    }

    dev_notice(dev, "PM's resume\n");
    // Success Oriented - Continue system resume even in case of error (otherwise system will not suspend correctly)
    return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(hailo_pcie_pm_ops, hailo_pcie_suspend, hailo_pcie_resume);

#if LINUX_VERSION_CODE >= KERNEL_VERSION( 3, 16, 0 )
static void hailo_pci_reset_prepare(struct pci_dev *pdev)
{
    struct hailo_pcie_board* board = (struct hailo_pcie_board*) pci_get_drvdata(pdev);
    int err = 0;
    /* Reset preparation logic goes here */
    pci_err(pdev, "Reset preparation for PCI device \n");

    if (board)
    {
        // lock board to wait for any pending operations and for synchronization with open
        down(&board->mutex);
        if (board->vdma.used_by_filp != NULL) {
            // Try to close all vDMA channels before reset
            err = driver_down(board);
            if (err < 0) {
                pci_err(pdev, "Error while trying to call FW to close vdma channels (errno %d)\n", err);
            }
        }
        up(&board->mutex);
    }
}
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION( 3, 16, 0 ) */

#if LINUX_VERSION_CODE < KERNEL_VERSION( 4, 13, 0 ) && LINUX_VERSION_CODE >= KERNEL_VERSION( 3, 16, 0 )
static void hailo_pci_reset_notify(struct pci_dev *pdev, bool prepare)
{
    if (prepare) {
        hailo_pci_reset_prepare(pdev);
    }
}
#endif

static const struct pci_error_handlers hailo_pcie_err_handlers = {
#if LINUX_VERSION_CODE < KERNEL_VERSION( 3, 16, 0 )
/* No FLR callback */
#elif LINUX_VERSION_CODE < KERNEL_VERSION( 4, 13, 0 )
/* FLR Callback is reset_notify */
	.reset_notify	= hailo_pci_reset_notify,
#else
/* FLR Callback is reset_prepare */
	.reset_prepare	= hailo_pci_reset_prepare,
#endif
};

static struct pci_device_id hailo_pcie_id_table[] =
{
    {PCI_DEVICE_DATA(HAILO, HAILO8, HAILO_BOARD_TYPE_HAILO8)},
    {PCI_DEVICE_DATA(HAILO, HAILO10H, HAILO_BOARD_TYPE_HAILO10H)},
    {PCI_DEVICE_DATA(HAILO, HAILO15L, HAILO_BOARD_TYPE_HAILO15L)},
    {0,0,0,0,0,0,0 },
};

static struct file_operations hailo_pcie_fops =
{
    owner:              THIS_MODULE,
    unlocked_ioctl:     hailo_pcie_fops_unlockedioctl,
    mmap:               hailo_pcie_fops_mmap,
    open:               hailo_pcie_fops_open,
    release:            hailo_pcie_fops_release
};


static struct pci_driver hailo_pci_driver =
{
    name:		 DRIVER_NAME,
    id_table:    hailo_pcie_id_table,
    probe:		 hailo_pcie_probe,
    remove:		 hailo_pcie_remove,
    driver: {
        pm: &hailo_pcie_pm_ops,
    },
    err_handler: &hailo_pcie_err_handlers,
};

MODULE_DEVICE_TABLE (pci, hailo_pcie_id_table);

static int hailo_pcie_register_chrdev(unsigned int major, const char *name)
{
    int char_major;

    char_major = register_chrdev(major, name, &hailo_pcie_fops);

    chardev_class = class_create_compat("hailo_chardev");

    return char_major;
}

static void hailo_pcie_unregister_chrdev(unsigned int major, const char *name)
{
    class_destroy(chardev_class);
    unregister_chrdev(major, name);
}

static int __init hailo_pcie_module_init(void)
{
    int err;

    pr_notice(DRIVER_NAME ": Init module. driver version %s\n", HAILO_DRV_VER);

    if ( 0 > (char_major = hailo_pcie_register_chrdev(0, DRIVER_NAME)) )
    {
        pr_err(DRIVER_NAME ": Init Error, failed to call register_chrdev.\n");

        return char_major;
    }

    if ( 0 != (err = pci_register_driver(&hailo_pci_driver)))
    {
        pr_err(DRIVER_NAME ": Init Error, failed to call pci_register_driver.\n");
        class_destroy(chardev_class);
        hailo_pcie_unregister_chrdev(char_major, DRIVER_NAME);
        return err;
    }

    return 0;
}

static void __exit hailo_pcie_module_exit(void)
{

    pr_notice(DRIVER_NAME ": Exit module.\n");

    // Unregister the driver from pci bus
    pci_unregister_driver(&hailo_pci_driver);
    hailo_pcie_unregister_chrdev(char_major, DRIVER_NAME);

    pr_notice(DRIVER_NAME ": Hailo PCIe driver unloaded.\n");
}


module_init(hailo_pcie_module_init);
module_exit(hailo_pcie_module_exit);

module_param(o_dbg, int, S_IRUGO | S_IWUSR);

module_param_named(no_power_mode, g_is_power_mode_enabled, invbool, S_IRUGO);
MODULE_PARM_DESC(no_power_mode, "Disables automatic D0->D3 PCIe transactions");

module_param(force_allocation_from_driver, int, S_IRUGO);
MODULE_PARM_DESC(force_allocation_from_driver, "Determines whether to force buffer allocation from driver or userspace");

module_param(force_desc_page_size, int, S_IRUGO);
MODULE_PARM_DESC(force_desc_page_size, "Determines the maximum DMA descriptor page size (must be a power of 2)");

module_param(force_hailo10h_legacy_mode, bool, S_IRUGO);
MODULE_PARM_DESC(force_hailo10h_legacy_mode, "Forces work with Hailo10h in legacy mode(relevant for emulators)");

module_param(force_boot_linux_from_eemc, bool, S_IRUGO);
MODULE_PARM_DESC(force_boot_linux_from_eemc, "Boot the linux image from eemc (Requires special Image)");

module_param(support_soft_reset, bool, S_IRUGO);
MODULE_PARM_DESC(support_soft_reset, "enables driver reload to reload a new firmware as well");

MODULE_AUTHOR("Hailo Technologies Ltd.");
MODULE_DESCRIPTION("Hailo PCIe driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(HAILO_DRV_VER);

