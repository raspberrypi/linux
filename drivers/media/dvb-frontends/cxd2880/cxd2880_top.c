/*
 * cxd2880_top.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 *
 * Copyright (C) 2016, 2017 Sony Semiconductor Solutions Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; version 2 of the License.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/spi/spi.h>

#include "dvb_frontend.h"

#include "cxd2880.h"
#include "cxd2880_tnrdmd_mon.h"
#include "cxd2880_tnrdmd_dvbt2_mon.h"
#include "cxd2880_tnrdmd_dvbt_mon.h"
#include "cxd2880_integ_dvbt2.h"
#include "cxd2880_integ_dvbt.h"
#include "cxd2880_devio_spi.h"
#include "cxd2880_spi_device.h"
#include "cxd2880_tnrdmd_driver_version.h"

struct cxd2880_priv {
	struct cxd2880_tnrdmd tnrdmd;
	struct spi_device *spi;
	struct cxd2880_io regio;
	struct cxd2880_spi_device spi_device;
	struct cxd2880_spi cxd2880_spi;
	struct cxd2880_dvbt_tune_param dvbt_tune_param;
	struct cxd2880_dvbt2_tune_param dvbt2_tune_param;
	struct mutex *spi_mutex; /* For SPI access exclusive control */
};

/*
 * return value conversion table
 */
static int return_tbl[] = {
	0,             /* CXD2880_RESULT_OK */
	-EINVAL,       /* CXD2880_RESULT_ERROR_ARG*/
	-EIO,          /* CXD2880_RESULT_ERROR_IO */
	-EPERM,        /* CXD2880_RESULT_ERROR_SW_STATE */
	-EBUSY,        /* CXD2880_RESULT_ERROR_HW_STATE */
	-ETIME,        /* CXD2880_RESULT_ERROR_TIMEOUT */
	-EAGAIN,       /* CXD2880_RESULT_ERROR_UNLOCK */
	-ERANGE,       /* CXD2880_RESULT_ERROR_RANGE */
	-EOPNOTSUPP,   /* CXD2880_RESULT_ERROR_NOSUPPORT */
	-ECANCELED,    /* CXD2880_RESULT_ERROR_CANCEL */
	-EPERM,        /* CXD2880_RESULT_ERROR_OTHER */
	-EOVERFLOW,    /* CXD2880_RESULT_ERROR_OVERFLOW */
	0,             /* CXD2880_RESULT_OK_CONFIRM */
};

static enum cxd2880_ret cxd2880_pre_bit_err_t(
		struct cxd2880_tnrdmd *tnrdmd, u32 *pre_bit_err,
		u32 *pre_bit_count)
{
	u8 rdata[2];

	if ((!tnrdmd) || (!pre_bit_err) || (!pre_bit_count))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnrdmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x10) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x39, rdata, 1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if ((rdata[0] & 0x01) == 0) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_HW_STATE;
	}

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x22, rdata, 2) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	*pre_bit_err = (rdata[0] << 8) | rdata[1];

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x6F, rdata, 1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	slvt_unfreeze_reg(tnrdmd);

	*pre_bit_count = ((rdata[0] & 0x07) == 0) ?
			256 : (0x1000 << (rdata[0] & 0x07));

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret cxd2880_pre_bit_err_t2(
		struct cxd2880_tnrdmd *tnrdmd, u32 *pre_bit_err,
		u32 *pre_bit_count)
{
	u32 period_exp = 0;
	u32 n_ldpc = 0;
	u8 data[5];

	if ((!tnrdmd) || (!pre_bit_err) || (!pre_bit_count))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (slvt_freeze_reg(tnrdmd) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x0B) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x3C, data, sizeof(data))
				 != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (!(data[0] & 0x01)) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_HW_STATE;
	}
	*pre_bit_err =
	((data[1] & 0x0F) << 24) | (data[2] << 16) | (data[3] << 8) | data[4];

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0xA0, data, 1) != CXD2880_RESULT_OK) {
		slvt_unfreeze_reg(tnrdmd);
		return CXD2880_RESULT_ERROR_IO;
	}

	if (((enum cxd2880_dvbt2_plp_fec)(data[0] & 0x03)) ==
	    CXD2880_DVBT2_FEC_LDPC_16K)
		n_ldpc = 16200;
	else
		n_ldpc = 64800;
	slvt_unfreeze_reg(tnrdmd);

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x20) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x6F, data, 1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	period_exp = data[0] & 0x0F;

	*pre_bit_count = (1U << period_exp) * n_ldpc;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret cxd2880_post_bit_err_t(struct cxd2880_tnrdmd *tnrdmd,
						u32 *post_bit_err,
						u32 *post_bit_count)
{
	u8 rdata[3];
	u32 bit_error = 0;
	u32 period_exp = 0;

	if ((!tnrdmd) || (!post_bit_err) || (!post_bit_count))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x0D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x15, rdata, 3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if ((rdata[0] & 0x40) == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	*post_bit_err = ((rdata[0] & 0x3F) << 16) | (rdata[1] << 8) | rdata[2];

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x60, rdata, 1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	period_exp = (rdata[0] & 0x1F);

	if ((period_exp <= 11) && (bit_error > (1U << period_exp) * 204 * 8))
		return CXD2880_RESULT_ERROR_HW_STATE;

	if (period_exp == 11)
		*post_bit_count = 3342336;
	else
		*post_bit_count = (1U << period_exp) * 204 * 81;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret cxd2880_post_bit_err_t2(struct cxd2880_tnrdmd *tnrdmd,
						u32 *post_bit_err,
						u32 *post_bit_count)
{
	u32 period_exp = 0;
	u32 n_bch = 0;

	if ((!tnrdmd) || (!post_bit_err) || (!post_bit_count))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 data[3];
		enum cxd2880_dvbt2_plp_fec plp_fec_type =
			CXD2880_DVBT2_FEC_LDPC_16K;
		enum cxd2880_dvbt2_plp_code_rate plp_code_rate =
			CXD2880_DVBT2_R1_2;

		static const u16 n_bch_bits_lookup[2][8] = {
			{7200, 9720, 10800, 11880, 12600, 13320, 5400, 6480},
			{32400, 38880, 43200, 48600, 51840, 54000, 21600, 25920}
		};

		if (slvt_freeze_reg(tnrdmd) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x00, 0x0B) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnrdmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
					 0x15, data, 3) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnrdmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		if (!(data[0] & 0x40)) {
			slvt_unfreeze_reg(tnrdmd);
			return CXD2880_RESULT_ERROR_HW_STATE;
		}

		*post_bit_err =
			((data[0] & 0x3F) << 16) | (data[1] << 8) | data[2];

		if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x9D, data, 1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnrdmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		plp_code_rate =
		(enum cxd2880_dvbt2_plp_code_rate)(data[0] & 0x07);

		if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0xA0, data, 1) != CXD2880_RESULT_OK) {
			slvt_unfreeze_reg(tnrdmd);
			return CXD2880_RESULT_ERROR_IO;
		}

		plp_fec_type = (enum cxd2880_dvbt2_plp_fec)(data[0] & 0x03);

		slvt_unfreeze_reg(tnrdmd);

		if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x00, 0x20) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x72, data, 1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		period_exp = data[0] & 0x0F;

		if ((plp_fec_type > CXD2880_DVBT2_FEC_LDPC_64K) ||
			(plp_code_rate > CXD2880_DVBT2_R2_5))
			return CXD2880_RESULT_ERROR_HW_STATE;

		n_bch = n_bch_bits_lookup[plp_fec_type][plp_code_rate];
	}

	if (*post_bit_err > ((1U << period_exp) * n_bch))
		return CXD2880_RESULT_ERROR_HW_STATE;

	*post_bit_count = (1U << period_exp) * n_bch;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret cxd2880_read_block_err_t(
					struct cxd2880_tnrdmd *tnrdmd,
					u32 *block_err,
					u32 *block_count)
{
	u8 rdata[3];

	if ((!tnrdmd) || (!block_err) || (!block_count))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->sys != CXD2880_DTV_SYS_DVBT)
		return CXD2880_RESULT_ERROR_SW_STATE;

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x0D) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x18, rdata, 3) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if ((rdata[0] & 0x01) == 0)
		return CXD2880_RESULT_ERROR_HW_STATE;

	*block_err = (rdata[1] << 8) | rdata[2];

	if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x00, 0x10) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
				0x5C, rdata, 1) != CXD2880_RESULT_OK)
		return CXD2880_RESULT_ERROR_IO;

	*block_count = 1U << (rdata[0] & 0x0F);

	if ((*block_count == 0) || (*block_err > *block_count))
		return CXD2880_RESULT_ERROR_HW_STATE;

	return CXD2880_RESULT_OK;
}

static enum cxd2880_ret cxd2880_read_block_err_t2(
					struct cxd2880_tnrdmd *tnrdmd,
					u32 *block_err,
					u32 *block_count)
{
	if ((!tnrdmd) || (!block_err) || (!block_count))
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->diver_mode == CXD2880_TNRDMD_DIVERMODE_SUB)
		return CXD2880_RESULT_ERROR_ARG;

	if (tnrdmd->state != CXD2880_TNRDMD_STATE_ACTIVE)
		return CXD2880_RESULT_ERROR_SW_STATE;
	if (tnrdmd->sys != CXD2880_DTV_SYS_DVBT2)
		return CXD2880_RESULT_ERROR_SW_STATE;

	{
		u8 rdata[3];

		if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x00, 0x0B) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x18, rdata, 3) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if ((rdata[0] & 0x01) == 0)
			return CXD2880_RESULT_ERROR_HW_STATE;

		*block_err = (rdata[1] << 8) | rdata[2];

		if (tnrdmd->io->write_reg(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0x00, 0x24) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		if (tnrdmd->io->read_regs(tnrdmd->io, CXD2880_IO_TGT_DMD,
					0xDC, rdata, 1) != CXD2880_RESULT_OK)
			return CXD2880_RESULT_ERROR_IO;

		*block_count = 1U << (rdata[0] & 0x0F);
	}

	if ((*block_count == 0) || (*block_err > *block_count))
		return CXD2880_RESULT_ERROR_HW_STATE;

	return CXD2880_RESULT_OK;
}

static void cxd2880_release(struct dvb_frontend *fe)
{
	struct cxd2880_priv *priv = NULL;

	if (!fe) {
		pr_err("%s: invalid arg.\n", __func__);
		return;
	}
	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	kfree(priv);
}

static int cxd2880_init(struct dvb_frontend *fe)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_priv *priv = NULL;
	struct cxd2880_tnrdmd_create_param create_param;

	if (!fe) {
		pr_err("%s: invalid arg.\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;

	create_param.ts_output_if = CXD2880_TNRDMD_TSOUT_IF_SPI;
	create_param.xtal_share_type = CXD2880_TNRDMD_XTAL_SHARE_NONE;
	create_param.en_internal_ldo = 1;
	create_param.xosc_cap = 18;
	create_param.xosc_i = 8;
	create_param.stationary_use = 1;

	mutex_lock(priv->spi_mutex);
	if (priv->tnrdmd.io != &priv->regio) {
		ret = cxd2880_tnrdmd_create(&priv->tnrdmd,
				&priv->regio, &create_param);
		if (ret != CXD2880_RESULT_OK) {
			mutex_unlock(priv->spi_mutex);
			dev_info(&priv->spi->dev,
				"%s: cxd2880 tnrdmd create failed %d\n",
				__func__, ret);
			return return_tbl[ret];
		}
	}
	ret = cxd2880_integ_init(&priv->tnrdmd);
	if (ret != CXD2880_RESULT_OK) {
		mutex_unlock(priv->spi_mutex);
		dev_err(&priv->spi->dev, "%s: cxd2880 integ init failed %d\n",
				__func__, ret);
		return return_tbl[ret];
	}
	mutex_unlock(priv->spi_mutex);

	dev_dbg(&priv->spi->dev, "%s: OK.\n", __func__);

	return return_tbl[ret];
}

static int cxd2880_sleep(struct dvb_frontend *fe)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_priv *priv = NULL;

	if (!fe) {
		pr_err("%s: inavlid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_sleep(&priv->tnrdmd);
	mutex_unlock(priv->spi_mutex);

	dev_dbg(&priv->spi->dev, "%s: tnrdmd_sleep ret %d\n",
		__func__, ret);

	return return_tbl[ret];
}

static int cxd2880_read_signal_strength(struct dvb_frontend *fe,
				u16 *strength)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_priv *priv = NULL;
	struct dtv_frontend_properties *c = NULL;
	int level = 0;

	if ((!fe) || (!strength)) {
		pr_err("%s: inavlid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	c = &fe->dtv_property_cache;

	mutex_lock(priv->spi_mutex);
	if ((c->delivery_system == SYS_DVBT) ||
		(c->delivery_system == SYS_DVBT2)) {
		ret = cxd2880_tnrdmd_mon_rf_lvl(&priv->tnrdmd, &level);
	} else {
		dev_dbg(&priv->spi->dev, "%s: invalid system\n", __func__);
		mutex_unlock(priv->spi_mutex);
		return -EINVAL;
	}
	mutex_unlock(priv->spi_mutex);

	level /= 125;
	/* -105dBm - -30dBm (-105000/125 = -840, -30000/125 = -240 */
	level = clamp(level, -840, -240);
	/* scale value to 0x0000-0xFFFF */
	*strength = (u16)(((level + 840) * 0xFFFF) / (-240 + 840));

	if (ret != CXD2880_RESULT_OK)
		dev_dbg(&priv->spi->dev, "%s: ret = %d\n", __func__, ret);

	return return_tbl[ret];
}

static int cxd2880_read_snr(struct dvb_frontend *fe, u16 *snr)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	int snrvalue = 0;
	struct cxd2880_priv *priv = NULL;
	struct dtv_frontend_properties *c = NULL;

	if ((!fe) || (!snr)) {
		pr_err("%s: inavlid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	c = &fe->dtv_property_cache;

	mutex_lock(priv->spi_mutex);
	if (c->delivery_system == SYS_DVBT) {
		ret = cxd2880_tnrdmd_dvbt_mon_snr(&priv->tnrdmd,
						&snrvalue);
	} else if (c->delivery_system == SYS_DVBT2) {
		ret = cxd2880_tnrdmd_dvbt2_mon_snr(&priv->tnrdmd,
						&snrvalue);
	} else {
		dev_err(&priv->spi->dev, "%s: invalid system\n", __func__);
		mutex_unlock(priv->spi_mutex);
		return -EINVAL;
	}
	mutex_unlock(priv->spi_mutex);

	if (snrvalue < 0)
		snrvalue = 0;
	*snr = (u16)snrvalue;

	if (ret != CXD2880_RESULT_OK)
		dev_dbg(&priv->spi->dev, "%s: ret = %d\n", __func__, ret);

	return return_tbl[ret];
}

static int cxd2880_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_priv *priv = NULL;
	struct dtv_frontend_properties *c = NULL;

	if ((!fe) || (!ucblocks)) {
		pr_err("%s: inavlid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	c = &fe->dtv_property_cache;

	mutex_lock(priv->spi_mutex);
	if (c->delivery_system == SYS_DVBT) {
		ret = cxd2880_tnrdmd_dvbt_mon_packet_error_number(
								&priv->tnrdmd,
								ucblocks);
	} else if (c->delivery_system == SYS_DVBT2) {
		ret = cxd2880_tnrdmd_dvbt2_mon_packet_error_number(
								&priv->tnrdmd,
								ucblocks);
	} else {
		dev_err(&priv->spi->dev, "%s: invlaid system\n", __func__);
		mutex_unlock(priv->spi_mutex);
		return -EINVAL;
	}
	mutex_unlock(priv->spi_mutex);

	if (ret != CXD2880_RESULT_OK)
		dev_dbg(&priv->spi->dev, "%s: ret = %d\n", __func__, ret);

	return return_tbl[ret];
}

static int cxd2880_read_ber(struct dvb_frontend *fe, u32 *ber)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct cxd2880_priv *priv = NULL;
	struct dtv_frontend_properties *c = NULL;

	if ((!fe) || (!ber)) {
		pr_err("%s: inavlid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	c = &fe->dtv_property_cache;

	mutex_lock(priv->spi_mutex);
	if (c->delivery_system == SYS_DVBT) {
		ret = cxd2880_tnrdmd_dvbt_mon_pre_rsber(&priv->tnrdmd,
						ber);
		/* x100 to change unit.(10^7 -> 10^9 */
		*ber *= 100;
	} else if (c->delivery_system == SYS_DVBT2) {
		ret = cxd2880_tnrdmd_dvbt2_mon_pre_bchber(&priv->tnrdmd,
						ber);
	} else {
		dev_err(&priv->spi->dev, "%s: invlaid system\n", __func__);
		mutex_unlock(priv->spi_mutex);
		return -EINVAL;
	}
	mutex_unlock(priv->spi_mutex);

	if (ret != CXD2880_RESULT_OK)
		dev_dbg(&priv->spi->dev, "%s: ret = %d\n", __func__, ret);

	return return_tbl[ret];
}

static int cxd2880_set_frontend(struct dvb_frontend *fe)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	struct dtv_frontend_properties *c;
	struct cxd2880_priv *priv;
	enum cxd2880_dtv_bandwidth bw = CXD2880_DTV_BW_1_7_MHZ;

	if (!fe) {
		pr_err("%s: inavlid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	c = &fe->dtv_property_cache;

	switch (c->bandwidth_hz) {
	case 1712000:
		bw = CXD2880_DTV_BW_1_7_MHZ;
		break;
	case 5000000:
		bw = CXD2880_DTV_BW_5_MHZ;
		break;
	case 6000000:
		bw = CXD2880_DTV_BW_6_MHZ;
		break;
	case 7000000:
		bw = CXD2880_DTV_BW_7_MHZ;
		break;
	case 8000000:
		bw = CXD2880_DTV_BW_8_MHZ;
		break;
	default:
		return -EINVAL;
	}

	dev_info(&priv->spi->dev, "%s: sys:%d freq:%d bw:%d\n", __func__,
			c->delivery_system, c->frequency, bw);
	mutex_lock(priv->spi_mutex);
	if (c->delivery_system == SYS_DVBT) {
		priv->tnrdmd.sys = CXD2880_DTV_SYS_DVBT;
		priv->dvbt_tune_param.center_freq_khz = c->frequency / 1000;
		priv->dvbt_tune_param.bandwidth = bw;
		priv->dvbt_tune_param.profile = CXD2880_DVBT_PROFILE_HP;
		ret = cxd2880_integ_dvbt_tune(&priv->tnrdmd,
						&priv->dvbt_tune_param);
	} else if (c->delivery_system == SYS_DVBT2) {
		priv->tnrdmd.sys = CXD2880_DTV_SYS_DVBT2;
		priv->dvbt2_tune_param.center_freq_khz = c->frequency / 1000;
		priv->dvbt2_tune_param.bandwidth = bw;
		priv->dvbt2_tune_param.data_plp_id = (u16)c->stream_id;
		ret = cxd2880_integ_dvbt2_tune(&priv->tnrdmd,
						&priv->dvbt2_tune_param);
	} else {
		dev_err(&priv->spi->dev, "%s: invalid system\n", __func__);
		mutex_unlock(priv->spi_mutex);
		return -EINVAL;
	}
	mutex_unlock(priv->spi_mutex);
	dev_info(&priv->spi->dev, "%s: tune result %d\n", __func__, ret);

	return return_tbl[ret];
}

static int cxd2880_read_status(struct dvb_frontend *fe,
				enum fe_status *status)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	u8 sync = 0;
	u8 lock = 0;
	u8 unlock = 0;
	struct cxd2880_priv *priv = NULL;
	struct dtv_frontend_properties *c = NULL;

	if ((!fe) || (!status)) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;
	c = &fe->dtv_property_cache;
	*status = 0;

	if (priv->tnrdmd.state == CXD2880_TNRDMD_STATE_ACTIVE) {
		mutex_lock(priv->spi_mutex);
		if (c->delivery_system == SYS_DVBT) {
			ret = cxd2880_tnrdmd_dvbt_mon_sync_stat(
							&priv->tnrdmd,
							&sync,
							&lock,
							&unlock);
		} else if (c->delivery_system == SYS_DVBT2) {
			ret = cxd2880_tnrdmd_dvbt2_mon_sync_stat(
							&priv->tnrdmd,
							&sync,
							&lock,
							&unlock);
		} else {
			dev_err(&priv->spi->dev,
				"%s: invlaid system", __func__);
			mutex_unlock(priv->spi_mutex);
			return -EINVAL;
		}

		mutex_unlock(priv->spi_mutex);
		if (ret != CXD2880_RESULT_OK) {
			dev_err(&priv->spi->dev, "%s: failed. sys = %d\n",
				__func__, priv->tnrdmd.sys);
			return  return_tbl[ret];
		}

		if (sync == 6) {
			*status = FE_HAS_SIGNAL |
					FE_HAS_CARRIER;
		}
		if (lock)
			*status |= FE_HAS_VITERBI |
					FE_HAS_SYNC |
					FE_HAS_LOCK;
	}

	dev_dbg(&priv->spi->dev, "%s: status %d result %d\n", __func__,
		*status, ret);

	return  return_tbl[CXD2880_RESULT_OK];
}

static int cxd2880_tune(struct dvb_frontend *fe,
			bool retune,
			unsigned int mode_flags,
			unsigned int *delay,
			enum fe_status *status)
{
	int ret = 0;

	if ((!fe) || (!delay) || (!status)) {
		pr_err("%s: invalid arg.", __func__);
		return -EINVAL;
	}

	if (retune) {
		ret = cxd2880_set_frontend(fe);
		if (ret) {
			pr_err("%s: cxd2880_set_frontend failed %d\n",
				__func__, ret);
			return ret;
		}
	}

	*delay = HZ / 5;

	return cxd2880_read_status(fe, status);
}

static int cxd2880_get_frontend_t(struct dvb_frontend *fe,
				struct dtv_frontend_properties *c)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	int result = 0;
	struct cxd2880_priv *priv = NULL;
	enum cxd2880_dvbt_mode mode = CXD2880_DVBT_MODE_2K;
	enum cxd2880_dvbt_guard guard = CXD2880_DVBT_GUARD_1_32;
	struct cxd2880_dvbt_tpsinfo tps;
	enum cxd2880_tnrdmd_spectrum_sense sense;
	u16 snr = 0;
	int strength = 0;
	u32 pre_bit_err = 0, pre_bit_count = 0;
	u32 post_bit_err = 0, post_bit_count = 0;
	u32 block_err = 0, block_count = 0;

	if ((!fe) || (!c)) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt_mon_mode_guard(&priv->tnrdmd,
						 &mode, &guard);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (mode) {
		case CXD2880_DVBT_MODE_2K:
			c->transmission_mode = TRANSMISSION_MODE_2K;
			break;
		case CXD2880_DVBT_MODE_8K:
			c->transmission_mode = TRANSMISSION_MODE_8K;
			break;
		default:
			c->transmission_mode = TRANSMISSION_MODE_2K;
			dev_err(&priv->spi->dev, "%s: get invalid mode %d\n",
					__func__, mode);
			break;
		}
		switch (guard) {
		case CXD2880_DVBT_GUARD_1_32:
			c->guard_interval = GUARD_INTERVAL_1_32;
			break;
		case CXD2880_DVBT_GUARD_1_16:
			c->guard_interval = GUARD_INTERVAL_1_16;
			break;
		case CXD2880_DVBT_GUARD_1_8:
			c->guard_interval = GUARD_INTERVAL_1_8;
			break;
		case CXD2880_DVBT_GUARD_1_4:
			c->guard_interval = GUARD_INTERVAL_1_4;
			break;
		default:
			c->guard_interval = GUARD_INTERVAL_1_32;
			dev_err(&priv->spi->dev, "%s: get invalid guard %d\n",
					__func__, guard);
			break;
		}
	} else {
		c->transmission_mode = TRANSMISSION_MODE_2K;
		c->guard_interval = GUARD_INTERVAL_1_32;
		dev_dbg(&priv->spi->dev,
			"%s: ModeGuard err %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt_mon_tps_info(&priv->tnrdmd, &tps);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (tps.hierarchy) {
		case CXD2880_DVBT_HIERARCHY_NON:
			c->hierarchy = HIERARCHY_NONE;
			break;
		case CXD2880_DVBT_HIERARCHY_1:
			c->hierarchy = HIERARCHY_1;
			break;
		case CXD2880_DVBT_HIERARCHY_2:
			c->hierarchy = HIERARCHY_2;
			break;
		case CXD2880_DVBT_HIERARCHY_4:
			c->hierarchy = HIERARCHY_4;
			break;
		default:
			c->hierarchy = HIERARCHY_NONE;
			dev_err(&priv->spi->dev,
				"%s: TPSInfo hierarchy invalid %d\n",
				__func__, tps.hierarchy);
			break;
		}

		switch (tps.rate_hp) {
		case CXD2880_DVBT_CODERATE_1_2:
			c->code_rate_HP = FEC_1_2;
			break;
		case CXD2880_DVBT_CODERATE_2_3:
			c->code_rate_HP = FEC_2_3;
			break;
		case CXD2880_DVBT_CODERATE_3_4:
			c->code_rate_HP = FEC_3_4;
			break;
		case CXD2880_DVBT_CODERATE_5_6:
			c->code_rate_HP = FEC_5_6;
			break;
		case CXD2880_DVBT_CODERATE_7_8:
			c->code_rate_HP = FEC_7_8;
			break;
		default:
			c->code_rate_HP = FEC_NONE;
			dev_err(&priv->spi->dev,
				"%s: TPSInfo rateHP invalid %d\n",
				__func__, tps.rate_hp);
			break;
		}
		switch (tps.rate_lp) {
		case CXD2880_DVBT_CODERATE_1_2:
			c->code_rate_LP = FEC_1_2;
			break;
		case CXD2880_DVBT_CODERATE_2_3:
			c->code_rate_LP = FEC_2_3;
			break;
		case CXD2880_DVBT_CODERATE_3_4:
			c->code_rate_LP = FEC_3_4;
			break;
		case CXD2880_DVBT_CODERATE_5_6:
			c->code_rate_LP = FEC_5_6;
			break;
		case CXD2880_DVBT_CODERATE_7_8:
			c->code_rate_LP = FEC_7_8;
			break;
		default:
			c->code_rate_LP = FEC_NONE;
			dev_err(&priv->spi->dev,
				"%s: TPSInfo rateLP invalid %d\n",
				__func__, tps.rate_lp);
			break;
		}
		switch (tps.constellation) {
		case CXD2880_DVBT_CONSTELLATION_QPSK:
			c->modulation = QPSK;
			break;
		case CXD2880_DVBT_CONSTELLATION_16QAM:
			c->modulation = QAM_16;
			break;
		case CXD2880_DVBT_CONSTELLATION_64QAM:
			c->modulation = QAM_64;
			break;
		default:
			c->modulation = QPSK;
			dev_err(&priv->spi->dev,
				"%s: TPSInfo constellation invalid %d\n",
				__func__, tps.constellation);
			break;
		}
	} else {
		c->hierarchy = HIERARCHY_NONE;
		c->code_rate_HP = FEC_NONE;
		c->code_rate_LP = FEC_NONE;
		c->modulation = QPSK;
		dev_dbg(&priv->spi->dev,
			"%s: TPS info err %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt_mon_spectrum_sense(&priv->tnrdmd, &sense);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (sense) {
		case CXD2880_TNRDMD_SPECTRUM_NORMAL:
			c->inversion = INVERSION_OFF;
			break;
		case CXD2880_TNRDMD_SPECTRUM_INV:
			c->inversion = INVERSION_ON;
			break;
		default:
			c->inversion = INVERSION_OFF;
			dev_err(&priv->spi->dev,
				"%s: spectrum sense invalid %d\n",
				__func__, sense);
			break;
		}
	} else {
		c->inversion = INVERSION_OFF;
		dev_dbg(&priv->spi->dev,
			"%s: spectrum_sense %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_mon_rf_lvl(&priv->tnrdmd, &strength);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->strength.len = 1;
		c->strength.stat[0].scale = FE_SCALE_DECIBEL;
		c->strength.stat[0].svalue = strength;
	} else {
		c->strength.len = 1;
		c->strength.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev, "%s: mon_rf_lvl %d\n",
			__func__, result);
	}

	result = cxd2880_read_snr(fe, &snr);
	if (!result) {
		c->cnr.len = 1;
		c->cnr.stat[0].scale = FE_SCALE_DECIBEL;
		c->cnr.stat[0].svalue = snr;
	} else {
		c->cnr.len = 1;
		c->cnr.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev, "%s: read_snr %d\n", __func__, result);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_pre_bit_err_t(&priv->tnrdmd, &pre_bit_err,
					&pre_bit_count);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->pre_bit_error.len = 1;
		c->pre_bit_error.stat[0].scale = FE_SCALE_COUNTER;
		c->pre_bit_error.stat[0].uvalue = pre_bit_err;
		c->pre_bit_count.len = 1;
		c->pre_bit_count.stat[0].scale = FE_SCALE_COUNTER;
		c->pre_bit_count.stat[0].uvalue = pre_bit_count;
	} else {
		c->pre_bit_error.len = 1;
		c->pre_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->pre_bit_count.len = 1;
		c->pre_bit_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: pre_bit_error_t failed %d\n",
			__func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_post_bit_err_t(&priv->tnrdmd,
				&post_bit_err, &post_bit_count);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->post_bit_error.len = 1;
		c->post_bit_error.stat[0].scale = FE_SCALE_COUNTER;
		c->post_bit_error.stat[0].uvalue = post_bit_err;
		c->post_bit_count.len = 1;
		c->post_bit_count.stat[0].scale = FE_SCALE_COUNTER;
		c->post_bit_count.stat[0].uvalue = post_bit_count;
	} else {
		c->post_bit_error.len = 1;
		c->post_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->post_bit_count.len = 1;
		c->post_bit_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: post_bit_err_t %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_read_block_err_t(&priv->tnrdmd,
					&block_err, &block_count);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->block_error.len = 1;
		c->block_error.stat[0].scale = FE_SCALE_COUNTER;
		c->block_error.stat[0].uvalue = block_err;
		c->block_count.len = 1;
		c->block_count.stat[0].scale = FE_SCALE_COUNTER;
		c->block_count.stat[0].uvalue = block_count;
	} else {
		c->block_error.len = 1;
		c->block_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->block_count.len = 1;
		c->block_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: read_block_err_t  %d\n", __func__, ret);
	}

	return 0;
}

static int cxd2880_get_frontend_t2(struct dvb_frontend *fe,
				struct dtv_frontend_properties *c)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	int result = 0;
	struct cxd2880_priv *priv = NULL;
	struct cxd2880_dvbt2_l1pre l1pre;
	enum cxd2880_dvbt2_plp_code_rate coderate;
	enum cxd2880_dvbt2_plp_constell qam;
	enum cxd2880_tnrdmd_spectrum_sense sense;
	u16 snr = 0;
	int strength = 0;
	u32 pre_bit_err = 0, pre_bit_count = 0;
	u32 post_bit_err = 0, post_bit_count = 0;
	u32 block_err = 0, block_count = 0;

	if ((!fe) || (!c)) {
		pr_err("%s: invalid arg.\n", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt2_mon_l1_pre(&priv->tnrdmd, &l1pre);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (l1pre.fft_mode) {
		case CXD2880_DVBT2_M2K:
			c->transmission_mode = TRANSMISSION_MODE_2K;
			break;
		case CXD2880_DVBT2_M8K:
			c->transmission_mode = TRANSMISSION_MODE_8K;
			break;
		case CXD2880_DVBT2_M4K:
			c->transmission_mode = TRANSMISSION_MODE_4K;
			break;
		case CXD2880_DVBT2_M1K:
			c->transmission_mode = TRANSMISSION_MODE_1K;
			break;
		case CXD2880_DVBT2_M16K:
			c->transmission_mode = TRANSMISSION_MODE_16K;
			break;
		case CXD2880_DVBT2_M32K:
			c->transmission_mode = TRANSMISSION_MODE_32K;
			break;
		default:
			c->transmission_mode = TRANSMISSION_MODE_2K;
			dev_err(&priv->spi->dev,
				"%s: L1Pre fft_mode invalid %d\n",
				__func__, l1pre.fft_mode);
			break;
		}
		switch (l1pre.gi) {
		case CXD2880_DVBT2_G1_32:
			c->guard_interval = GUARD_INTERVAL_1_32;
			break;
		case CXD2880_DVBT2_G1_16:
			c->guard_interval = GUARD_INTERVAL_1_16;
			break;
		case CXD2880_DVBT2_G1_8:
			c->guard_interval = GUARD_INTERVAL_1_8;
			break;
		case CXD2880_DVBT2_G1_4:
			c->guard_interval = GUARD_INTERVAL_1_4;
			break;
		case CXD2880_DVBT2_G1_128:
			c->guard_interval = GUARD_INTERVAL_1_128;
			break;
		case CXD2880_DVBT2_G19_128:
			c->guard_interval = GUARD_INTERVAL_19_128;
			break;
		case CXD2880_DVBT2_G19_256:
			c->guard_interval = GUARD_INTERVAL_19_256;
			break;
		default:
			c->guard_interval = GUARD_INTERVAL_1_32;
			dev_err(&priv->spi->dev,
				"%s: L1Pre gi invalid %d\n",
				__func__, l1pre.gi);
			break;
		}
	} else {
		c->transmission_mode = TRANSMISSION_MODE_2K;
		c->guard_interval = GUARD_INTERVAL_1_32;
		dev_dbg(&priv->spi->dev,
			"%s: L1Pre err %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt2_mon_code_rate(&priv->tnrdmd,
						CXD2880_DVBT2_PLP_DATA,
						&coderate);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (coderate) {
		case CXD2880_DVBT2_R1_2:
			c->fec_inner = FEC_1_2;
			break;
		case CXD2880_DVBT2_R3_5:
			c->fec_inner = FEC_3_5;
			break;
		case CXD2880_DVBT2_R2_3:
			c->fec_inner = FEC_2_3;
			break;
		case CXD2880_DVBT2_R3_4:
			c->fec_inner = FEC_3_4;
			break;
		case CXD2880_DVBT2_R4_5:
			c->fec_inner = FEC_4_5;
			break;
		case CXD2880_DVBT2_R5_6:
			c->fec_inner = FEC_5_6;
			break;
		default:
			c->fec_inner = FEC_NONE;
			dev_err(&priv->spi->dev,
				"%s: CodeRate invalid %d\n",
				__func__, coderate);
			break;
		}
	} else {
		c->fec_inner = FEC_NONE;
		dev_dbg(&priv->spi->dev, "%s: CodeRate %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt2_mon_qam(&priv->tnrdmd,
					CXD2880_DVBT2_PLP_DATA,
					&qam);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (qam) {
		case CXD2880_DVBT2_QPSK:
			c->modulation = QPSK;
			break;
		case CXD2880_DVBT2_QAM16:
			c->modulation = QAM_16;
			break;
		case CXD2880_DVBT2_QAM64:
			c->modulation = QAM_64;
			break;
		case CXD2880_DVBT2_QAM256:
			c->modulation = QAM_256;
			break;
		default:
			c->modulation = QPSK;
			dev_err(&priv->spi->dev,
				"%s: QAM invalid %d\n",
				__func__, qam);
			break;
		}
	} else {
		c->modulation = QPSK;
		dev_dbg(&priv->spi->dev, "%s: QAM %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_dvbt2_mon_spectrum_sense(&priv->tnrdmd, &sense);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		switch (sense) {
		case CXD2880_TNRDMD_SPECTRUM_NORMAL:
			c->inversion = INVERSION_OFF;
			break;
		case CXD2880_TNRDMD_SPECTRUM_INV:
			c->inversion = INVERSION_ON;
			break;
		default:
			c->inversion = INVERSION_OFF;
			dev_err(&priv->spi->dev,
				"%s: spectrum sense invalid %d\n",
				__func__, sense);
			break;
		}
	} else {
		c->inversion = INVERSION_OFF;
		dev_dbg(&priv->spi->dev,
			"%s: SpectrumSense %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_tnrdmd_mon_rf_lvl(&priv->tnrdmd, &strength);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->strength.len = 1;
		c->strength.stat[0].scale = FE_SCALE_DECIBEL;
		c->strength.stat[0].svalue = strength;
	} else {
		c->strength.len = 1;
		c->strength.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: mon_rf_lvl %d\n", __func__, ret);
	}

	result = cxd2880_read_snr(fe, &snr);
	if (!result) {
		c->cnr.len = 1;
		c->cnr.stat[0].scale = FE_SCALE_DECIBEL;
		c->cnr.stat[0].svalue = snr;
	} else {
		c->cnr.len = 1;
		c->cnr.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev, "%s: read_snr %d\n", __func__, result);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_pre_bit_err_t2(&priv->tnrdmd,
				&pre_bit_err,
				&pre_bit_count);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->pre_bit_error.len = 1;
		c->pre_bit_error.stat[0].scale = FE_SCALE_COUNTER;
		c->pre_bit_error.stat[0].uvalue = pre_bit_err;
		c->pre_bit_count.len = 1;
		c->pre_bit_count.stat[0].scale = FE_SCALE_COUNTER;
		c->pre_bit_count.stat[0].uvalue = pre_bit_count;
	} else {
		c->pre_bit_error.len = 1;
		c->pre_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->pre_bit_count.len = 1;
		c->pre_bit_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: read_bit_err_t2 %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_post_bit_err_t2(&priv->tnrdmd,
				&post_bit_err, &post_bit_count);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->post_bit_error.len = 1;
		c->post_bit_error.stat[0].scale = FE_SCALE_COUNTER;
		c->post_bit_error.stat[0].uvalue = post_bit_err;
		c->post_bit_count.len = 1;
		c->post_bit_count.stat[0].scale = FE_SCALE_COUNTER;
		c->post_bit_count.stat[0].uvalue = post_bit_count;
	} else {
		c->post_bit_error.len = 1;
		c->post_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->post_bit_count.len = 1;
		c->post_bit_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: post_bit_err_t2 %d\n", __func__, ret);
	}

	mutex_lock(priv->spi_mutex);
	ret = cxd2880_read_block_err_t2(&priv->tnrdmd,
					&block_err, &block_count);
	mutex_unlock(priv->spi_mutex);
	if (ret == CXD2880_RESULT_OK) {
		c->block_error.len = 1;
		c->block_error.stat[0].scale = FE_SCALE_COUNTER;
		c->block_error.stat[0].uvalue = block_err;
		c->block_count.len = 1;
		c->block_count.stat[0].scale = FE_SCALE_COUNTER;
		c->block_count.stat[0].uvalue = block_count;
	} else {
		c->block_error.len = 1;
		c->block_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		c->block_count.len = 1;
		c->block_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
		dev_dbg(&priv->spi->dev,
			"%s: read_block_err_t2 %d\n", __func__, ret);
	}

	return 0;
}

static int cxd2880_get_frontend(struct dvb_frontend *fe,
				struct dtv_frontend_properties *props)
{
	struct cxd2880_priv *priv = NULL;
	int result = 0;

	if ((!fe) || (!props)) {
		pr_err("%s: invalid arg.", __func__);
		return -EINVAL;
	}

	priv = (struct cxd2880_priv *)fe->demodulator_priv;

	dev_dbg(&priv->spi->dev, "%s: system=%d\n", __func__,
		fe->dtv_property_cache.delivery_system);
	switch (fe->dtv_property_cache.delivery_system) {
	case SYS_DVBT:
		result = cxd2880_get_frontend_t(fe, props);
		break;
	case SYS_DVBT2:
		result = cxd2880_get_frontend_t2(fe, props);
		break;
	default:
		result = -EINVAL;
		break;
	}

	return result;
}

static enum dvbfe_algo cxd2880_get_frontend_algo(struct dvb_frontend *fe)
{
	return DVBFE_ALGO_HW;
}

static struct dvb_frontend_ops cxd2880_dvbt_t2_ops;

struct dvb_frontend *cxd2880_attach(struct dvb_frontend *fe,
				struct cxd2880_config *cfg)
{
	enum cxd2880_ret ret = CXD2880_RESULT_OK;
	enum cxd2880_tnrdmd_chip_id chipid =
					CXD2880_TNRDMD_CHIP_ID_UNKNOWN;
	static struct cxd2880_priv *priv;
	u8 data = 0;

	if (!fe) {
		pr_err("%s: invalid arg.\n", __func__);
		return NULL;
	}

	priv = kzalloc(sizeof(struct cxd2880_priv), GFP_KERNEL);
	if (!priv)
		return NULL;

	priv->spi = cfg->spi;
	priv->spi_mutex = cfg->spi_mutex;
	priv->spi_device.spi = cfg->spi;

	memcpy(&fe->ops, &cxd2880_dvbt_t2_ops,
			sizeof(struct dvb_frontend_ops));

	ret = cxd2880_spi_device_initialize(&priv->spi_device,
						CXD2880_SPI_MODE_0,
						55000000);
	if (ret != CXD2880_RESULT_OK) {
		dev_err(&priv->spi->dev,
			"%s: spi_device_initialize failed. %d\n",
			__func__, ret);
		kfree(priv);
		return NULL;
	}

	ret = cxd2880_spi_device_create_spi(&priv->cxd2880_spi,
					&priv->spi_device);
	if (ret != CXD2880_RESULT_OK) {
		dev_err(&priv->spi->dev,
			"%s: spi_device_create_spi failed. %d\n",
			__func__, ret);
		kfree(priv);
		return NULL;
	}

	ret = cxd2880_io_spi_create(&priv->regio, &priv->cxd2880_spi, 0);
	if (ret != CXD2880_RESULT_OK) {
		dev_err(&priv->spi->dev,
			"%s: io_spi_create failed. %d\n", __func__, ret);
		kfree(priv);
		return NULL;
	}
	if (priv->regio.write_reg(&priv->regio, CXD2880_IO_TGT_SYS, 0x00, 0x00)
		!= CXD2880_RESULT_OK) {
		dev_err(&priv->spi->dev,
			"%s: set bank to 0x00 failed.\n", __func__);
		kfree(priv);
		return NULL;
	}
	if (priv->regio.read_regs(&priv->regio,
					CXD2880_IO_TGT_SYS, 0xFD, &data, 1)
					!= CXD2880_RESULT_OK) {
		dev_err(&priv->spi->dev,
			"%s: read chip id failed.\n", __func__);
		kfree(priv);
		return NULL;
	}

	chipid = (enum cxd2880_tnrdmd_chip_id)data;
	if ((chipid != CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_0X) &&
		(chipid != CXD2880_TNRDMD_CHIP_ID_CXD2880_ES1_11)) {
		dev_err(&priv->spi->dev,
			"%s: chip id invalid.\n", __func__);
		kfree(priv);
		return NULL;
	}

	fe->demodulator_priv = priv;
	dev_info(&priv->spi->dev,
		"CXD2880 driver version: Ver %s\n",
		CXD2880_TNRDMD_DRIVER_VERSION);

	return fe;
}
EXPORT_SYMBOL(cxd2880_attach);

static struct dvb_frontend_ops cxd2880_dvbt_t2_ops = {
	.info = {
		.name = "Sony CXD2880",
		.frequency_min =  174000000,
		.frequency_max = 862000000,
		.frequency_stepsize = 1000,
		.caps = FE_CAN_INVERSION_AUTO |
				FE_CAN_FEC_1_2 |
				FE_CAN_FEC_2_3 |
				FE_CAN_FEC_3_4 |
				FE_CAN_FEC_4_5 |
				FE_CAN_FEC_5_6	|
				FE_CAN_FEC_7_8	|
				FE_CAN_FEC_AUTO |
				FE_CAN_QPSK |
				FE_CAN_QAM_16 |
				FE_CAN_QAM_32 |
				FE_CAN_QAM_64 |
				FE_CAN_QAM_128 |
				FE_CAN_QAM_256 |
				FE_CAN_QAM_AUTO |
				FE_CAN_TRANSMISSION_MODE_AUTO |
				FE_CAN_GUARD_INTERVAL_AUTO |
				FE_CAN_2G_MODULATION |
				FE_CAN_RECOVER |
				FE_CAN_MUTE_TS,
	},
	.delsys = { SYS_DVBT, SYS_DVBT2 },

	.release = cxd2880_release,
	.init = cxd2880_init,
	.sleep = cxd2880_sleep,
	.tune = cxd2880_tune,
	.set_frontend = cxd2880_set_frontend,
	.get_frontend = cxd2880_get_frontend,
	.read_status = cxd2880_read_status,
	.read_ber = cxd2880_read_ber,
	.read_signal_strength = cxd2880_read_signal_strength,
	.read_snr = cxd2880_read_snr,
	.read_ucblocks = cxd2880_read_ucblocks,
	.get_frontend_algo = cxd2880_get_frontend_algo,
};

MODULE_DESCRIPTION(
"Sony CXD2880 DVB-T2/T tuner + demodulator drvier");
MODULE_AUTHOR("Sony Semiconductor Solutions Corporation");
MODULE_LICENSE("GPL v2");
