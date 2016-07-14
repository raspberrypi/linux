/*
 * Driver for the ESS SABRE9018K2M
 *
 * Author: Howard Qiao (howard.qiao@aoide.cc)
 *
 * Based on Sabre9018q2c Codec Driver
 * Satoru Kawase <satoru.kawase@gmail.com>, Takahito Nishiara 
 *      Copyright 2016
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _SND_SOC_SABRE9018K2M
#define _SND_SOC_SABRE9018K2M


/* SABRE9018K2M Register Address */
#define SABRE9018K2M_REG_0	0
#define SABRE9018K2M_REG_1	1
#define SABRE9018K2M_REG_4	4
#define SABRE9018K2M_REG_5	5
#define SABRE9018K2M_REG_6	6
#define SABRE9018K2M_REG_7	7
#define SABRE9018K2M_REG_8	8
#define SABRE9018K2M_REG_10	10
#define SABRE9018K2M_REG_11	11
#define SABRE9018K2M_REG_12	12
#define SABRE9018K2M_REG_13	13
#define SABRE9018K2M_REG_14	14
#define SABRE9018K2M_REG_15	15
#define SABRE9018K2M_REG_16	16
#define SABRE9018K2M_REG_17	17
#define SABRE9018K2M_REG_18	18
#define SABRE9018K2M_REG_19	19
#define SABRE9018K2M_REG_20	20
#define SABRE9018K2M_REG_21	21
#define SABRE9018K2M_REG_22	22
#define SABRE9018K2M_REG_23	23
#define SABRE9018K2M_REG_24	24
#define SABRE9018K2M_REG_25	25
#define SABRE9018K2M_REG_26	26
#define SABRE9018K2M_REG_27	27
#define SABRE9018K2M_REG_28	28
#define SABRE9018K2M_REG_29	29
#define SABRE9018K2M_REG_30	30
#define SABRE9018K2M_REG_39	39
#define SABRE9018K2M_REG_40	40
#define SABRE9018K2M_REG_41	41
#define SABRE9018K2M_REG_42	42
#define SABRE9018K2M_REG_43	43
#define SABRE9018K2M_REG_64	64
#define SABRE9018K2M_REG_65	65
#define SABRE9018K2M_REG_66	66
#define SABRE9018K2M_REG_67	67
#define SABRE9018K2M_REG_68	68
#define SABRE9018K2M_REG_69	69
#define SABRE9018K2M_REG_70	70
#define SABRE9018K2M_REG_71	71
#define SABRE9018K2M_REG_72	72
#define SABRE9018K2M_REG_73	73
#define SABRE9018K2M_REG_74	74
#define SABRE9018K2M_MAX_REG	74	/* Maximum Register Number */


/* SABRE9018K2M Chip ID Check Function */
extern bool sabre9018k2m_check_chip_id(struct snd_soc_codec *codec);


#endif /* _SND_SOC_SABRE9018K2M */
