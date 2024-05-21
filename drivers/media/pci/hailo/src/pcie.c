// SPDX-License-Identifier: GPL-2.0
/**
 * Copyright (c) 2019-2022 Hailo Technologies Ltd. All rights reserved.
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#include <linux/dma-direct.h>
#endif

#define KERNEL_CODE	1

#include "hailo_pcie_version.h"
#include "hailo_ioctl_common.h"
#include "pcie.h"
#include "fops.h"
#include "sysfs.h"
#include "utils/logs.h"
#include "utils/compact.h"
#include "vdma/vdma.h"

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

//Debug flag
static int force_desc_page_size = 0;
static bool g_is_power_mode_enabled = true;
static int force_allocation_from_driver = HAILO_NO_FORCE_BUFFER;

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

static int hailo_write_config(struct hailo_pcie_resources *resources, struct device *dev,
    const struct hailo_config_constants *config_consts)
{
    const struct firmware *config = NULL;
    int err = 0;

    if (NULL == config_consts->filename) {
        // Config not supported for platform
        return 0;
    }

    err = request_firmware_direct(&config, config_consts->filename, dev);
    if (err < 0) {
        hailo_dev_info(dev, "Config %s not found\n", config_consts->filename);
        return 0;
    }

    hailo_dev_notice(dev, "Writing config %s\n", config_consts->filename);

    err = hailo_pcie_write_config_common(resources, config->data, config->size, config_consts);
    if (err < 0) {
        if (-EINVAL == err) {
            hailo_dev_warn(dev, "Config size %zu is bigger than max %zu\n", config->size, config_consts->max_size);
        }
        release_firmware(config);
        return err;
    }

    release_firmware(config);
    return 0;
}

static bool wait_for_firmware_completion(struct completion *fw_load_completion)
{
    return (0 != wait_for_completion_timeout(fw_load_completion, FIRMWARE_WAIT_TIMEOUT_MS));
}

static int hailo_load_firmware(struct hailo_pcie_resources *resources,
    struct device *dev, struct completion *fw_load_completion)
{
    const struct firmware *firmware = NULL;
    int err = 0;

    if (hailo_pcie_is_firmware_loaded(resources)) {
        hailo_dev_warn(dev, "Firmware was already loaded\n");
        return 0;
    }

    reinit_completion(fw_load_completion);

    err = hailo_write_config(resources, dev, hailo_pcie_get_board_config_constants(resources->board_type));
    if (err < 0) {
        hailo_dev_err(dev, "Failed writing board config");
        return err;
    }

    err = hailo_write_config(resources, dev, hailo_pcie_get_user_config_constants(resources->board_type));
    if (err < 0) {
        hailo_dev_err(dev, "Failed writing fw config");
        return err;
    }

    // read firmware file
    err = request_firmware_direct(&firmware, hailo_pcie_get_fw_filename(resources->board_type), dev);
    if (err < 0) {
        hailo_dev_warn(dev, "Firmware file not found (/lib/firmware/%s), please upload the firmware manually \n",
            hailo_pcie_get_fw_filename(resources->board_type));
        return 0;
    }

    err = hailo_pcie_write_firmware(resources, firmware->data, firmware->size);
    if (err < 0) {
        hailo_dev_err(dev, "Failed writing firmware. err %d\n", err);
        release_firmware(firmware);
        return err;
    }

    release_firmware(firmware);

    if (!wait_for_firmware_completion(fw_load_completion)) {
        hailo_dev_err(dev, "Timeout waiting for firmware..\n");
        return -ETIMEDOUT;
    }

    hailo_dev_notice(dev, "Firmware was loaded successfully\n");
    return 0;
}

static int hailo_activate_board(struct hailo_pcie_board *board)
{
    int err = 0;

    (void)hailo_pcie_disable_aspm(board, PCIE_LINK_STATE_L0S, false);

    err = hailo_enable_interrupts(board);
    if (err < 0) {
        hailo_err(board, "Failed Enabling interrupts %d\n", err);
        return err;
    }

    err = hailo_load_firmware(&board->pcie_resources, &board->pDev->dev,
        &board->fw_loaded_completion);
    if (err < 0) {
        hailo_err(board, "Firmware load failed\n");
        hailo_disable_interrupts(board);
        return err;
    }

    hailo_disable_interrupts(board);

    if (power_mode_enabled()) {
        // Setting the device to low power state, until the user opens the device
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

    resources->board_type = board_type;

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
    init_completion(&pBoard->fw_loaded_completion);

    sema_init(&pBoard->mutex, 1);
    atomic_set(&pBoard->ref_count, 0);
    INIT_LIST_HEAD(&pBoard->open_files_list);

    sema_init(&pBoard->fw_control.mutex, 1);
    spin_lock_init(&pBoard->notification_read_spinlock);
    init_completion(&pBoard->fw_control.completion);

    init_completion(&pBoard->driver_down.reset_completed);

    INIT_LIST_HEAD(&pBoard->notification_wait_list);

    memset(&pBoard->notification_cache, 0, sizeof(pBoard->notification_cache));
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
    struct hailo_notification_wait *cursor = NULL;

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

        // Lock rcu_read_lock and send notification_completion to wake anyone waiting on the notification_wait_list when removed
        rcu_read_lock();
        list_for_each_entry_rcu(cursor, &pBoard->notification_wait_list, notification_wait_list) {
            cursor->is_disabled = true;
            complete(&cursor->notification_completion);
        }
        rcu_read_unlock();

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

#ifdef CONFIG_PM_SLEEP
static int hailo_pcie_suspend(struct device *dev)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board*) dev_get_drvdata(dev);
    struct hailo_file_context *cur = NULL;
    int err = 0;

    // lock board to wait for any pending operations
    down(&board->mutex);

    // Disable all interrupts. All interrupts from Hailo chip would be masked.
    hailo_disable_interrupts(board);

    // Close all vDMA channels
    if (board->vdma.used_by_filp != NULL) {
        err = hailo_pcie_driver_down(board);
        if (err < 0) {
            dev_notice(dev, "Error while trying to call FW to close vdma channels\n");
        }
    }

    // Un validate all activae file contexts so every new action would return error to the user.
    list_for_each_entry(cur, &board->open_files_list, open_files_list) {
        cur->is_valid = false;
    }

    // Release board
    up(&board->mutex);

    dev_notice(dev, "PM's suspend\n");
    // Continue system suspend
    return err;
}

static int hailo_pcie_resume(struct device *dev)
{
    struct hailo_pcie_board *board = (struct hailo_pcie_board*) dev_get_drvdata(dev);
    int err = 0;

    if ((err = hailo_activate_board(board)) < 0) {
        dev_err(dev, "Failed activating board %d\n", err);
        return err;
    }

    dev_notice(dev, "PM's resume\n");
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
            err = hailo_pcie_driver_down(board);
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
    {PCI_DEVICE_DATA(HAILO, HAILO15, HAILO_BOARD_TYPE_HAILO15)},
    {PCI_DEVICE_DATA(HAILO, PLUTO, HAILO_BOARD_TYPE_PLUTO)},
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

MODULE_AUTHOR("Hailo Technologies Ltd.");
MODULE_DESCRIPTION("Hailo PCIe driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(HAILO_DRV_VER);

