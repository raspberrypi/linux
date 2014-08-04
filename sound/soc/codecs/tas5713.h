/*
 * ASoC Driver for TAS5713
 *
 * Author:      Sebastian Eickhoff <basti.eickhoff@googlemail.com>
 *              Copyright 2014
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _TAS5713_H
#define _TAS5713_H


// TAS5713 I2C-bus register addresses

#define TAS5713_CLOCK_CTRL              0x00
#define TAS5713_DEVICE_ID               0x01
#define TAS5713_ERROR_STATUS            0x02
#define TAS5713_SYSTEM_CTRL1            0x03
#define TAS5713_SERIAL_DATA_INTERFACE   0x04
#define TAS5713_SYSTEM_CTRL2            0x05
#define TAS5713_SOFT_MUTE               0x06
#define TAS5713_VOL_MASTER              0x07
#define TAS5713_VOL_CH1                 0x08
#define TAS5713_VOL_CH2                 0x09
#define TAS5713_VOL_HEADPHONE           0x0A
#define TAS5713_VOL_CONFIG              0x0E
#define TAS5713_MODULATION_LIMIT        0x10
#define TAS5713_IC_DLY_CH1              0x11
#define TAS5713_IC_DLY_CH2              0x12
#define TAS5713_IC_DLY_CH3              0x13
#define TAS5713_IC_DLY_CH4              0x14

#define TAS5713_START_STOP_PERIOD       0x1A
#define TAS5713_OSC_TRIM                0x1B
#define TAS5713_BKND_ERR                0x1C

#define TAS5713_INPUT_MUX               0x20
#define TAS5713_SRC_SELECT_CH4          0x21
#define TAS5713_PWM_MUX                 0x25

#define TAS5713_CH1_BQ0                 0x29
#define TAS5713_CH1_BQ1                 0x2A
#define TAS5713_CH1_BQ2                 0x2B
#define TAS5713_CH1_BQ3                 0x2C
#define TAS5713_CH1_BQ4                 0x2D
#define TAS5713_CH1_BQ5                 0x2E
#define TAS5713_CH1_BQ6                 0x2F
#define TAS5713_CH1_BQ7                 0x58
#define TAS5713_CH1_BQ8                 0x59

#define TAS5713_CH2_BQ0                 0x30
#define TAS5713_CH2_BQ1                 0x31
#define TAS5713_CH2_BQ2                 0x32
#define TAS5713_CH2_BQ3                 0x33
#define TAS5713_CH2_BQ4                 0x34
#define TAS5713_CH2_BQ5                 0x35
#define TAS5713_CH2_BQ6                 0x36
#define TAS5713_CH2_BQ7                 0x5C
#define TAS5713_CH2_BQ8                 0x5D

#define TAS5713_CH4_BQ0                 0x5A
#define TAS5713_CH4_BQ1                 0x5B
#define TAS5713_CH3_BQ0                 0x5E
#define TAS5713_CH3_BQ1                 0x5F

#define TAS5713_DRC1_SOFTENING_FILTER_ALPHA_OMEGA       0x3B
#define TAS5713_DRC1_ATTACK_RELEASE_RATE                0x3C
#define TAS5713_DRC2_SOFTENING_FILTER_ALPHA_OMEGA       0x3E
#define TAS5713_DRC2_ATTACK_RELEASE_RATE                0x3F
#define TAS5713_DRC1_ATTACK_RELEASE_THRES               0x40
#define TAS5713_DRC2_ATTACK_RELEASE_THRES               0x43
#define TAS5713_DRC_CTRL                                0x46

#define TAS5713_BANK_SW_CTRL            0x50
#define TAS5713_CH1_OUTPUT_MIXER        0x51
#define TAS5713_CH2_OUTPUT_MIXER        0x52
#define TAS5713_CH1_INPUT_MIXER         0x53
#define TAS5713_CH2_INPUT_MIXER         0x54
#define TAS5713_OUTPUT_POST_SCALE       0x56
#define TAS5713_OUTPUT_PRESCALE         0x57

#define TAS5713_IDF_POST_SCALE          0x62

#define TAS5713_CH1_INLINE_MIXER        0x70
#define TAS5713_CH1_INLINE_DRC_EN_MIXER 0x71
#define TAS5713_CH1_R_CHANNEL_MIXER     0x72
#define TAS5713_CH1_L_CHANNEL_MIXER     0x73
#define TAS5713_CH2_INLINE_MIXER        0x74
#define TAS5713_CH2_INLINE_DRC_EN_MIXER 0x75
#define TAS5713_CH2_L_CHANNEL_MIXER     0x76
#define TAS5713_CH2_R_CHANNEL_MIXER     0x77

#define TAS5713_UPDATE_DEV_ADDR_KEY     0xF8
#define TAS5713_UPDATE_DEV_ADDR_REG     0xF9

#define TAS5713_REGISTER_COUNT          0x46
#define TAS5713_MAX_REGISTER            0xF9


// Bitmasks for registers
#define TAS5713_SOFT_MUTE_ALL           0x07



struct tas5713_init_command {
        const int size;
        const char *const data;
};

static const struct tas5713_init_command tas5713_init_sequence[] = {
        
        // Trim oscillator
    { .size = 2,  .data = "\x1B\x00" }, 
    // System control register 1 (0x03): block DC
    { .size = 2,  .data = "\x03\x80" },
    // Mute everything
    { .size = 2,  .data = "\x05\x40" },
    // Modulation limit register (0x10): 97.7%
    { .size = 2,  .data = "\x10\x02" },
    // Interchannel delay registers
    // (0x11, 0x12, 0x13, and 0x14): BD mode
    { .size = 2,  .data = "\x11\xB8" },
    { .size = 2,  .data = "\x12\x60" },
    { .size = 2,  .data = "\x13\xA0" },
    { .size = 2,  .data = "\x14\x48" },
    // PWM shutdown group register (0x19): no shutdown
    { .size = 2,  .data = "\x19\x00" },
    // Input multiplexer register (0x20): BD mode
    { .size = 2,  .data = "\x20\x00\x89\x77\x72" },
    // PWM output mux register (0x25)
    // Channel 1 --> OUTA, channel 1 neg --> OUTB
    // Channel 2 --> OUTC, channel 2 neg --> OUTD
    { .size = 5,  .data = "\x25\x01\x02\x13\x45" },
    // DRC control (0x46): DRC off
    { .size = 5,  .data = "\x46\x00\x00\x00\x00" },
    // BKND_ERR register (0x1C): 299ms reset period
    { .size = 2,  .data = "\x1C\x07" }, 
    // Mute channel 3
    { .size = 2,  .data = "\x0A\xFF" },
    // Volume configuration register (0x0E): volume slew 512 steps
    { .size = 2,  .data = "\x0E\x90" },
    // Clock control register (0x00): 44/48kHz, MCLK=64xfs
    { .size = 2,  .data = "\x00\x60" }, 
    // Bank switch and eq control (0x50): no bank switching
    { .size = 5,  .data = "\x50\x00\x00\x00\x00" },
    // Volume registers (0x07, 0x08, 0x09, 0x0A)
    { .size = 2,  .data = "\x07\x20" },
    { .size = 2,  .data = "\x08\x30" },
    { .size = 2,  .data = "\x09\x30" },
    { .size = 2,  .data = "\x0A\xFF" },
    // 0x72, 0x73, 0x76, 0x77 input mixer:
    // no intermix between channels
    { .size = 5,  .data = "\x72\x00\x00\x00\x00" },
    { .size = 5,  .data = "\x73\x00\x80\x00\x00" },
    { .size = 5,  .data = "\x76\x00\x00\x00\x00" },
    { .size = 5,  .data = "\x77\x00\x80\x00\x00" },
    // 0x70, 0x71, 0x74, 0x75 inline DRC mixer:
    // no inline DRC inmix
    { .size = 5,  .data = "\x70\x00\x80\x00\x00" },
    { .size = 5,  .data = "\x71\x00\x00\x00\x00" },
    { .size = 5,  .data = "\x74\x00\x80\x00\x00" },
    { .size = 5,  .data = "\x75\x00\x00\x00\x00" },
    // 0x56, 0x57 Output scale
    { .size = 5,  .data = "\x56\x00\x80\x00\x00" },
    { .size = 5,  .data = "\x57\x00\x02\x00\x00" },
    // 0x3B, 0x3c
    { .size = 9,  .data = "\x3B\x00\x08\x00\x00\x00\x78\x00\x00" },
    { .size = 9,  .data = "\x3C\x00\x00\x01\x00\xFF\xFF\xFF\x00" },
    { .size = 9,  .data = "\x3E\x00\x08\x00\x00\x00\x78\x00\x00" },
    { .size = 9,  .data = "\x3F\x00\x00\x01\x00\xFF\xFF\xFF\x00" },
    { .size = 9,  .data = "\x40\x00\x00\x01\x00\xFF\xFF\xFF\x00" },
    { .size = 9,  .data = "\x43\x00\x00\x01\x00\xFF\xFF\xFF\x00" },
    // 0x51, 0x52: output mixer
    { .size = 9,  .data = "\x51\x00\x80\x00\x00\x00\x00\x00\x00" },
    { .size = 9,  .data = "\x52\x00\x80\x00\x00\x00\x00\x00\x00" },
    // PEQ defaults
    { .size = 21,  .data = "\x29\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x2A\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x2B\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x2C\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x2D\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x2E\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x2F\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x30\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x31\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x32\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x33\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x34\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x35\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x36\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x58\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x59\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x5C\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x5D\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x5E\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x5F\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x5A\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
    { .size = 21,  .data = "\x5B\x00\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00" },
};


#endif  /* _TAS5713_H */
