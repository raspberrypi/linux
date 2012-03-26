/*****************************************************************************
* Copyright 2011 Broadcom Corporation.  All rights reserved.
*
* Unless you and Broadcom execute a separate written software license
* agreement governing use of this software, this software is licensed to you
* under the terms of the GNU General Public License version 2, available at
* http://www.broadcom.com/licenses/GPLv2.php (the "GPL").
*	
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a
* license other than the GPL, without Broadcom's express prior written
* consent.
*****************************************************************************/

#ifndef __SOUND_ARM_BCM2835_H
#define __SOUND_ARM_BCM2835_H 

#define SUBSTREAM_NUM 1

#include <linux/device.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/workqueue.h>

/* #define DUMP_RAW_DATA */
#define AUDIO_DEBUG_ENABLE
//#define AUDIO_VERBOSE_DEBUG_ENABLE

/* Debug macros */
#ifdef AUDIO_DEBUG_ENABLE

#ifdef AUDIO_VERBOSE_DEBUG_ENABLE

#define audio_debug(fmt, arg...)	\
	printk(KERN_INFO"%s:%d " fmt, __func__, __LINE__, ##arg)

#define audio_info(fmt, arg...)	\
	printk(KERN_INFO"%s:%d " fmt, __func__, __LINE__, ##arg)

#else

#define audio_debug(fmt, arg...)	do {} while (0)

#define audio_info(fmt, arg...)	        do {} while (0)

#endif /* AUDIO_VERBOSE_DEBUG_ENABLE */

#else

#define audio_debug(fmt, arg...)	do {} while (0)

#define audio_info(fmt, arg...)		do {} while (0)

#endif /* AUDIO_DEBUG_ENABLE */

#define audio_error(fmt, arg...)	\
	printk(KERN_ERR"%s:%d " fmt, __func__, __LINE__, ##arg)

#define audio_warning(fmt, arg...)	\
	printk(KERN_WARNING"%s:%d " fmt, __func__, __LINE__, ##arg)

#define audio_alert(fmt, arg...)	\
	printk(KERN_ALERT"%s:%d " fmt, __func__, __LINE__, ##arg)

#define MAX_SUBSTREAMS			(8)
#define AVAIL_SUBSTREAMS_MASK		(0xff)

#define AUDIO_IPC_BLOCK_NUM_BUFFERS    (8)
#define AUDIO_IPC_BLOCK_BUFFER_SIZE    (1024*8)

#define AUDIO_CONTROL_OFFSET			(0x00)
	#define CTRL_EN_SHIFT			(0)
	#define CTRL_EN_MASK			(0x00000001)
	#define CTRL_PLAY_SHIFT			(1)
	#define CTRL_PLAY_MASK			(0x00000002)
	#define CTRL_MUTE_SHIFT			(2)
	#define CTRL_MUTE_MASK			(0x00000004)
	#define CTRL_SETUP_SHIFT		(3)
	#define CTRL_SETUP_MASK			(0x00000008)
	#define CTRL_FLUSH_SHIFT	    (4)
	#define CTRL_FLUSH_MASK			(0x00000010)
    #define CTRL_STOPMODE_SHIFT	    (5)
	#define CTRL_STOPMODE_MASK		(0x00000020)

#define AUDIO_STATUS_OFFSET			(0x04)
	#define STAT_EN_SHIFT			(0)
	#define STAT_EN_MASK			(0x00000001)
	#define STAT_PLAY_SHIFT			(1)
	#define STAT_PLAY_MASK			(0x00000002)
	#define STAT_MUTE_SHIFT			(2)
	#define STAT_MUTE_MASK			(0x00000004)
	#define STAT_SETUP_SHIFT		(3)
	#define STAT_SETUP_MASK			(0x00000008)
	#define STAT_FLUSH_SHIFT	    (4)
	#define STAT_FLUSH_MASK			(0x00000010)
	#define STAT_STOPMODE_SHIFT	    (5)
	#define STAT_STOPMODE_MASK		(0x00000020)

/* Interrupt status */
#define AUDIO_INTSTAT_OFFSET			(0x08)
	#define INTSTAT_CONTROL_SHIFT		(0)
	#define INTSTAT_CONTROL_MASK		(0x0000000f)
	#define INTSTAT_FIFO_SHIFT		(4)
	#define INTSTAT_FIFO_MASK		(0x000000f0)

/* Configuration */
#define AUDIO_DESTINATION_OFFSET		(0x0C)
#define AUDIO_SAMPLE_RATE_OFFSET		(0x10)
#define AUDIO_BIT_RATE_OFFSET			(0x14)
#define AUDIO_VOLUME_OFFSET			(0x18)
#define AUDIO_CHANNELS_OFFSET			(0x1C)

/* Implemention of peterson's algorithm for shared memory semaphores */
#define AUDIO_FLAG0_OFFSET			(0x20)
#define AUDIO_FLAG1_OFFSET			(0x24)
#define AUDIO_TURN_OFFSET			(0x28)

/* Fifo registers */
#define AUDIO_IN_WRITE_PTR_OFFSET		(0x30)
#define AUDIO_IN_READ_PTR_OFFSET		(0x34)
#define AUDIO_IN_FIFO_SIZE_OFFSET		(0x38)
#define AUDIO_IN_FIFO_ENTRY_OFFSET		(0x3C)
#define AUDIO_IN_FIFO_START_OFFSET		(0x40)

/* 8 entries here of 4 words each = 0x80 gap from 0x50 */
#define AUDIO_IN_FIFO_OFFSET			(0x50)

#define AUDIO_OUT_WRITE_PTR_OFFSET		(0xD0)
#define AUDIO_OUT_READ_PTR_OFFSET		(0xD4)
#define AUDIO_OUT_FIFO_SIZE_OFFSET		(0xD8)
#define AUDIO_OUT_FIFO_ENTRY_OFFSET		(0xDC)
#define AUDIO_OUT_FIFO_START_OFFSET		(0xE0)

/* 8 entries here of 4 words each = 0x80 gap from 0xF0 */
#define AUDIO_OUT_FIFO_OFFSET			(0xF0)


/* Some constants for values .. */
typedef enum {
	AUDIO_DEST_LOCAL = 0,
	AUDIO_DEST_HDMI	= 2,
	AUDIO_DEST_ALL = 3,
} SND_BCM2835_ROUTE_T;

typedef enum {
	PCM_PLAYBACK_VOLUME,
	PCM_PLAYBACK_MUTE,
	PCM_PLAYBACK_DEVICE,
} SND_BCM2835_CTRL_T;

/* this struct is tightly packed - its size is 16bytes */
typedef struct
{
	uint32_t buffer_id;
	uint32_t buffer_size;
	uint32_t buffer_ptr;
	uint32_t spare;

} AUDIO_FIFO_ENTRY_T;

/* definition of the chip-specific record */
typedef struct bcm2835_chip {
	struct snd_card *card;
	struct snd_pcm *pcm;
	/* Bitmat for valid reg_base and irq numbers */
	uint32_t avail_substreams;
	struct platform_device *pdev[MAX_SUBSTREAMS];
	struct bcm2835_alsa_stream *alsa_stream[MAX_SUBSTREAMS];

	int volume;
	int dest;
	int mute;
} bcm2835_chip_t;

typedef struct bcm2835_audio_buffer {
	uint32_t buffer_id;
	phys_addr_t	bus_addr;
	uint8_t __iomem	*start;
	uint32_t size;
	uint32_t data_left;
	struct list_head link;

} bcm2835_audio_buffer_t; 

typedef struct bcm2835_alsa_stream {
	bcm2835_chip_t *chip;
	struct snd_pcm_substream *substream;

	struct semaphore buffers_update_sem;
	struct semaphore control_sem;
	spinlock_t lock;
	volatile uint32_t control;
	volatile uint32_t status;

	int open;
	int running;
	int draining;

#ifdef DUMP_RAW_DATA
	/* for debug */
	int file;
#endif
	unsigned int pos;
	unsigned int buffer_size;
	unsigned int period_size;

	uint32_t enable_fifo_irq;
	irq_handler_t fifo_irq_handler;

	atomic_t retrieved;
	struct opaque_AUDIO_INSTANCE_T *instance;
	struct workqueue_struct *my_wq;
} bcm2835_alsa_stream_t;

int snd_bcm2835_new_ctl(bcm2835_chip_t *chip);
int snd_bcm2835_new_pcm(bcm2835_chip_t *chip);

void bcm2835_audio_fifo_get_lock(bcm2835_alsa_stream_t *alsa_stream);
void bcm2835_audio_fifo_put_lock(bcm2835_alsa_stream_t *alsa_stream);

int bcm2835_audio_open(bcm2835_alsa_stream_t *alsa_stream);
int bcm2835_audio_close(bcm2835_alsa_stream_t *alsa_stream);
int bcm2835_audio_set_params(bcm2835_alsa_stream_t *alsa_stream, uint32_t channels, uint32_t samplerate, uint32_t bps);
int bcm2835_audio_setup(bcm2835_alsa_stream_t *alsa_stream);
int bcm2835_audio_start(bcm2835_alsa_stream_t *alsa_stream);
int bcm2835_audio_stop(bcm2835_alsa_stream_t *alsa_stream);
int bcm2835_audio_set_ctls(bcm2835_chip_t *chip);
int bcm2835_audio_write(bcm2835_alsa_stream_t *alsa_stream, uint32_t count, void *src);
//uint32_t bcm2835_audio_buffers_consumed_bytes(bcm2835_alsa_stream_t *alsa_stream);
uint32_t bcm2835_audio_retrieve_buffers(bcm2835_alsa_stream_t *alsa_stream);
void bcm2835_audio_flush_buffers(bcm2835_alsa_stream_t *alsa_stream);
void bcm2835_audio_flush_playback_buffers(bcm2835_alsa_stream_t *alsa_stream);

#endif /* __SOUND_ARM_BCM2835_H */
