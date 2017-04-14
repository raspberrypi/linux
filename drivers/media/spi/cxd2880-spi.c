/*
 * cxd2880-spi.c
 * Sony CXD2880 DVB-T2/T tuner + demodulator driver
 * SPI adapter
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

#include "dvb_demux.h"
#include "dmxdev.h"
#include "dvb_frontend.h"
#include "cxd2880.h"

#define CXD2880_MAX_FILTER_SIZE 32
#define BURST_WRITE_MAX 128
#define MAX_TRANS_PACKET 300

struct cxd2880_ts_buf_info {
	u8 read_ready;
	u8 almost_full;
	u8 almost_empty;
	u8 overflow;
	u8 underflow;
	u16 packet_num;
};

struct cxd2880_pid_config {
	u8 is_enable;
	u16 pid;
};

struct cxd2880_pid_filter_config {
	u8 is_negative;
	struct cxd2880_pid_config pid_config[CXD2880_MAX_FILTER_SIZE];
};

struct cxd2880_dvb_spi {
	struct dvb_frontend dvb_fe;
	struct dvb_adapter adapter;
	struct dvb_demux demux;
	struct dmxdev dmxdev;
	struct dmx_frontend dmx_fe;
	struct task_struct *cxd2880_ts_read_thread;
	struct spi_device *spi;
	struct mutex spi_mutex; /* For SPI access exclusive control */
	int feed_count;
	int all_pid_feed_count;
	u8 *ts_buf;
	struct cxd2880_pid_filter_config filter_config;
};

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static int cxd2880_write_spi(struct spi_device *spi, u8 *data, u32 size)
{
	struct spi_message msg;
	struct spi_transfer tx;
	int ret = 0;

	if ((!spi) || (!data)) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	memset(&tx, 0, sizeof(tx));
	tx.tx_buf = data;
	tx.len = size;

	spi_message_init(&msg);
	spi_message_add_tail(&tx, &msg);
	ret = spi_sync(spi, &msg);

	return ret;
}

static int cxd2880_write_reg(struct spi_device *spi,
				u8 subAddress, const u8 *data, u32 size)
{
	u8 send_data[BURST_WRITE_MAX + 4];
	const u8 *write_data_top = NULL;
	int ret = 0;

	if ((!spi) || (!data)) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}
	if (size > BURST_WRITE_MAX) {
		pr_err("%s: data size > WRITE_MAX\n", __func__);
		return -EINVAL;
	}

	if (subAddress + size > 0x100) {
		pr_err("%s: out of range\n", __func__);
		return -EINVAL;
	}

	send_data[0] = 0x0E;
	write_data_top = data;

	while (size > 0) {
		send_data[1] = subAddress;
		if (size > 255)
			send_data[2] = 255;
		else
			send_data[2] = (u8)size;

		memcpy(&send_data[3], write_data_top, send_data[2]);

		ret = cxd2880_write_spi(spi, send_data, send_data[2] + 3);
		if (ret) {
			dev_err(&spi->dev, "%s: write spi failed %d\n",
				__func__, ret);
			break;
		}
		subAddress += send_data[2];
		write_data_top += send_data[2];
		size -= send_data[2];
	}

	return ret;
}

static int cxd2880_spi_read_ts(struct spi_device *spi,
					u8 *read_data,
					u32 packet_num)
{
	int ret = 0;
	u8 data[3];
	struct spi_message message;
	struct spi_transfer transfer[2];

	if ((!spi) || (!read_data) || (!packet_num)) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}
	if (packet_num > 0xFFFF) {
		dev_err(&spi->dev, "%s: packet num > 0xFFFF\n", __func__);
		return -EINVAL;
	}

	data[0] = 0x10;
	data[1] = (u8)((packet_num >> 8) & 0xFF);
	data[2] = (u8)(packet_num & 0xFF);

	spi_message_init(&message);
	memset(transfer, 0, sizeof(transfer));

	transfer[0].len = 3;
	transfer[0].tx_buf = data;
	spi_message_add_tail(&transfer[0], &message);
	transfer[1].len = packet_num * 188;
	transfer[1].rx_buf = read_data;
	spi_message_add_tail(&transfer[1], &message);

	ret = spi_sync(spi, &message);
	if (ret)
		dev_err(&spi->dev, "%s: spi_write_then_read failed\n",
			__func__);

	return ret;
}

static int cxd2880_spi_read_ts_buffer_info(struct spi_device *spi,
					struct cxd2880_ts_buf_info *info)
{
	u8 send_data = 0x20;
	u8 recv_data[2];
	int ret = 0;

	if ((!spi) || (!info)) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	ret = spi_write_then_read(spi, &send_data, 1,
			recv_data, sizeof(recv_data));
	if (ret)
		dev_err(&spi->dev,
			"%s: spi_write_then_read failed\n", __func__);

	info->read_ready = (u8)((recv_data[0] & 0x80) ? 1 : 0);
	info->almost_full = (u8)((recv_data[0] & 0x40) ? 1 : 0);
	info->almost_empty = (u8)((recv_data[0] & 0x20) ? 1 : 0);
	info->overflow = (u8)((recv_data[0] & 0x10) ? 1 : 0);
	info->underflow = (u8)((recv_data[0] & 0x08) ? 1 : 0);
	info->packet_num = (u16)(((recv_data[0] & 0x07) << 8) | recv_data[1]);

	return ret;
}

static int cxd2880_spi_clear_ts_buffer(struct spi_device *spi)
{
	u8 data = 0x03;
	int ret = 0;

	ret = cxd2880_write_spi(spi, &data, 1);

	if (ret)
		pr_err("%s: write spi failed\n", __func__);

	return ret;
}

static int cxd2880_set_pid_filter(struct spi_device *spi,
				struct cxd2880_pid_filter_config *cfg)
{
	u8 data[65];

	if (!spi) {
		pr_err("%s: ivnalid arg\n", __func__);
		return -EINVAL;
	}

	data[0] = 0x00;
	if (cxd2880_write_reg(spi, 0x00, &data[0], 1) != 0)
		return -EIO;
	if (!cfg) {
		data[0] = 0x02;
		if (cxd2880_write_reg(spi, 0x50, &data[0], 1) != 0)
			return -EIO;
	} else {
		data[0] = (u8)(cfg->is_negative ? 0x01 : 0x00);
		{
			int i = 0;
			u16 pid = 0;

			for (i = 0; i < CXD2880_MAX_FILTER_SIZE; i++) {
				pid = cfg->pid_config[i].pid;
				if (cfg->pid_config[i].is_enable) {
					data[1 + (i * 2)] =
					    (u8)((u8)(pid >> 8) | 0x20);
					data[2 + (i * 2)] =
					    (u8)(pid & 0xFF);
				} else {
					data[1 + (i * 2)] = 0x00;
					data[2 + (i * 2)] = 0x00;
				}
			}
		}
		if (cxd2880_write_reg(spi, 0x50, data, 65) != 0)
			return -EIO;
	}

	return 0;
}

static int cxd2880_update_pid_filter(struct cxd2880_dvb_spi *dvb_spi,
				struct cxd2880_pid_filter_config *cfg,
				bool is_all_pid_filter)
{
	int ret = 0;

	if ((!dvb_spi) || (!cfg)) {
		pr_err("%s: invalid arg.\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&dvb_spi->spi_mutex);
	if (is_all_pid_filter) {
		struct cxd2880_pid_filter_config tmpcfg;

		memset(&tmpcfg, 0, sizeof(tmpcfg));
		tmpcfg.is_negative = 1;
		tmpcfg.pid_config[0].is_enable = 1;
		tmpcfg.pid_config[0].pid = 0x1FFF;

		ret = cxd2880_set_pid_filter(dvb_spi->spi, &tmpcfg);
	} else {
		ret = cxd2880_set_pid_filter(dvb_spi->spi, cfg);
	}
	mutex_unlock(&dvb_spi->spi_mutex);

	if (ret) {
		dev_err(&dvb_spi->spi->dev,
			"%s: set_pid_filter failed\n", __func__);
	}

	return ret;
}

static int cxd2880_ts_read(void *arg)
{
	struct cxd2880_dvb_spi *dvb_spi = NULL;
	struct cxd2880_ts_buf_info info;
	struct timespec ts;
	long elapsed = 0;
	long starttime = 0;
	u32 i;
	int ret;

	dvb_spi = (struct cxd2880_dvb_spi *)arg;
	if (!dvb_spi) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	ret = cxd2880_spi_clear_ts_buffer(dvb_spi->spi);
	if (ret) {
		dev_err(&dvb_spi->spi->dev,
			"%s: set_clear_ts_buffer failed\n", __func__);
		return ret;
	}

	getnstimeofday(&ts);
	starttime = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
	while (!kthread_should_stop()) {
		getnstimeofday(&ts);
		elapsed =
			((ts.tv_sec * 1000) + (ts.tv_nsec / 1000000))
			- starttime;
		ret = cxd2880_spi_read_ts_buffer_info(dvb_spi->spi,
							&info);
		if (ret) {
			pr_err("%s: spi_read_ts_buffer_info error\n",
				__func__);
			return ret;
		}

		if (info.packet_num > MAX_TRANS_PACKET) {
			for (i = 0; i < info.packet_num / MAX_TRANS_PACKET;
				i++) {
				cxd2880_spi_read_ts(dvb_spi->spi,
							dvb_spi->ts_buf,
							MAX_TRANS_PACKET);
				dvb_dmx_swfilter(&dvb_spi->demux,
						dvb_spi->ts_buf,
						MAX_TRANS_PACKET * 188);
			}
			starttime = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
		} else if ((info.packet_num > 0) && (elapsed >= 500)) {
			cxd2880_spi_read_ts(dvb_spi->spi,
						dvb_spi->ts_buf,
						info.packet_num);
			dvb_dmx_swfilter(&dvb_spi->demux,
					dvb_spi->ts_buf,
					info.packet_num * 188);
			starttime = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
		} else {
			usleep_range(10000, 11000);
		}
	}

	return 0;
}

static int cxd2880_start_feed(struct dvb_demux_feed *feed)
{
	int ret = 0;
	int i = 0;
	struct dvb_demux *demux = NULL;
	struct cxd2880_dvb_spi *dvb_spi = NULL;

	if (!feed) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	demux = feed->demux;
	if (!demux) {
		pr_err("%s: feed->demux is NULL\n", __func__);
		return -EINVAL;
	}
	dvb_spi = (struct cxd2880_dvb_spi *)demux->priv;

	if (dvb_spi->feed_count == CXD2880_MAX_FILTER_SIZE) {
		dev_err(&dvb_spi->spi->dev,
			"%s: Exceeded maximum PID count (32).", __func__);
		dev_err(&dvb_spi->spi->dev,
			"Selected PID cannot be enabled.\n");
		return -EBUSY;
	}

	if (feed->pid == 0x2000) {
		if (dvb_spi->all_pid_feed_count == 0) {
			ret = cxd2880_update_pid_filter(dvb_spi,
							&dvb_spi->filter_config,
							true);
			if (ret) {
				dev_err(&dvb_spi->spi->dev,
					"%s: update pid filter failed\n",
					__func__);
				return ret;
			}
		}
		dvb_spi->all_pid_feed_count++;

		dev_dbg(&dvb_spi->spi->dev,
			"%s: all PID feed (count = %d)\n",
			__func__, dvb_spi->all_pid_feed_count);
	} else {
		struct cxd2880_pid_filter_config cfgtmp;

		cfgtmp = dvb_spi->filter_config;

		for (i = 0; i < CXD2880_MAX_FILTER_SIZE; i++) {
			if (cfgtmp.pid_config[i].is_enable == 0) {
				cfgtmp.pid_config[i].is_enable = 1;
				cfgtmp.pid_config[i].pid = feed->pid;
				dev_dbg(&dvb_spi->spi->dev,
				"%s: store PID %d to #%d\n",
				__func__, feed->pid, i);
				break;
			}
		}
		if (i == CXD2880_MAX_FILTER_SIZE) {
			dev_err(&dvb_spi->spi->dev,
				"%s: PID filter is full. Assumed bug.\n",
				__func__);
			return -EBUSY;
		}
		if (!dvb_spi->all_pid_feed_count)
			ret = cxd2880_update_pid_filter(dvb_spi,
							&cfgtmp,
							false);
		if (ret)
			return ret;

		dvb_spi->filter_config = cfgtmp;
	}

	if (dvb_spi->feed_count == 0) {
		dvb_spi->ts_buf =
			kmalloc(sizeof(u8) * MAX_TRANS_PACKET * 188,
				GFP_KERNEL | GFP_DMA);
		if (!dvb_spi->ts_buf) {
			dev_err(&dvb_spi->spi->dev,
			"%s: ts buffer allocate failed\n", __func__);
			memset(&dvb_spi->filter_config, 0,
				sizeof(dvb_spi->filter_config));
			dvb_spi->all_pid_feed_count = 0;
			return -ENOMEM;
		}
		dvb_spi->cxd2880_ts_read_thread = kthread_run(cxd2880_ts_read,
								dvb_spi,
								"cxd2880_ts_read");
		if (IS_ERR(dvb_spi->cxd2880_ts_read_thread)) {
			dev_err(&dvb_spi->spi->dev,
				"%s: kthread_run failed/\n",
				__func__);
			kfree(dvb_spi->ts_buf);
			dvb_spi->ts_buf = NULL;
			memset(&dvb_spi->filter_config, 0,
				sizeof(dvb_spi->filter_config));
			dvb_spi->all_pid_feed_count = 0;
			return PTR_ERR(dvb_spi->cxd2880_ts_read_thread);
		}
	}

	dvb_spi->feed_count++;

	dev_dbg(&dvb_spi->spi->dev, "%s: start feed (count %d)\n",
		__func__, dvb_spi->feed_count);
	return 0;
}

static int cxd2880_stop_feed(struct dvb_demux_feed *feed)
{
	int i = 0;
	int ret = 0;
	struct dvb_demux *demux = NULL;
	struct cxd2880_dvb_spi *dvb_spi = NULL;

	if (!feed) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	demux = feed->demux;
	if (!demux) {
		pr_err("%s: feed->demux is NULL\n", __func__);
		return -EINVAL;
	}
	dvb_spi = (struct cxd2880_dvb_spi *)demux->priv;

	if (!dvb_spi->feed_count) {
		dev_warn(&dvb_spi->spi->dev,
			"%s: no feed is started\n", __func__);
		return -EINVAL;
	}

	if (feed->pid == 0x2000) {
		/*
		 * Special PID case.
		 * Number of 0x2000 feed request was stored
		 * in dvb_spi->all_pid_feed_count.
		 */
		if (dvb_spi->all_pid_feed_count <= 0) {
			dev_warn(&dvb_spi->spi->dev,
				"%s: PID %d not found.\n",
				__func__, feed->pid);
			return -EINVAL;
		}
		dvb_spi->all_pid_feed_count--;
	} else {
		struct cxd2880_pid_filter_config cfgtmp;

		cfgtmp = dvb_spi->filter_config;

		for (i = 0; i < CXD2880_MAX_FILTER_SIZE; i++) {
			if (feed->pid == cfgtmp.pid_config[i].pid &&
				cfgtmp.pid_config[i].is_enable != 0) {
				cfgtmp.pid_config[i].is_enable = 0;
				cfgtmp.pid_config[i].pid = 0;
				dev_dbg(&dvb_spi->spi->dev,
					"%s: removed PID %d from #%d\n",
					__func__, feed->pid, i);
				break;
			}
		}
		dvb_spi->filter_config = cfgtmp;

		if (i == CXD2880_MAX_FILTER_SIZE) {
			dev_warn(&dvb_spi->spi->dev, "%s: PID %d not found\n",
				__func__, feed->pid);
			return -EINVAL;
		}
	}

	ret = cxd2880_update_pid_filter(dvb_spi,
					&dvb_spi->filter_config,
					dvb_spi->all_pid_feed_count > 0);
	dvb_spi->feed_count--;

	if (dvb_spi->feed_count == 0) {
		int ret_stop = 0;

		ret_stop = kthread_stop(dvb_spi->cxd2880_ts_read_thread);
		if (ret_stop) {
			dev_err(&dvb_spi->spi->dev,
			"%s: cxd2880_ts_read thread didn't terminate normally\n",
			__func__);
			ret = ret_stop;
		}
		kfree(dvb_spi->ts_buf);
		dvb_spi->ts_buf = NULL;
	}

	dev_dbg(&dvb_spi->spi->dev, "%s: stop feed ok.(count %d)\n",
		__func__, dvb_spi->feed_count);

	return ret;
}

static const struct of_device_id cxd2880_spi_of_match[] = {
	{ .compatible = "sony,cxd2880" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, cxd2880_spi_of_match);

static int
cxd2880_spi_probe(struct spi_device *spi)
{
	int ret = 0;
	struct cxd2880_dvb_spi *dvb_spi = NULL;
	struct cxd2880_config config;

	if (!spi) {
		pr_err("%s: invalid arg.\n", __func__);
		return -EINVAL;
	}

	dvb_spi = kzalloc(sizeof(struct cxd2880_dvb_spi), GFP_KERNEL);
	if (!dvb_spi)
		return -ENOMEM;

	dvb_spi->spi = spi;
	mutex_init(&dvb_spi->spi_mutex);
	dev_set_drvdata(&spi->dev, dvb_spi);
	config.spi = spi;
	config.spi_mutex = &dvb_spi->spi_mutex;

	ret = dvb_register_adapter(&dvb_spi->adapter,
					"CXD2880",
					THIS_MODULE,
					&spi->dev,
					adapter_nr);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: dvb_register_adapter() failed\n",
			__func__);
		goto fail_adapter;
	}

	if (!dvb_attach(cxd2880_attach, &dvb_spi->dvb_fe, &config)) {
		dev_err(&spi->dev, "%s: cxd2880_attach failed\n", __func__);
		goto fail_attach;
	}

	ret = dvb_register_frontend(&dvb_spi->adapter,
					&dvb_spi->dvb_fe);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: dvb_register_frontend() failed\n",
			__func__);
		goto fail_frontend;
	}

	dvb_spi->demux.dmx.capabilities = DMX_TS_FILTERING;
	dvb_spi->demux.priv = dvb_spi;
	dvb_spi->demux.filternum = CXD2880_MAX_FILTER_SIZE;
	dvb_spi->demux.feednum = CXD2880_MAX_FILTER_SIZE;
	dvb_spi->demux.start_feed = cxd2880_start_feed;
	dvb_spi->demux.stop_feed = cxd2880_stop_feed;

	ret = dvb_dmx_init(&dvb_spi->demux);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: dvb_dmx_init() failed\n", __func__);
		goto fail_dmx;
	}

	dvb_spi->dmxdev.filternum = CXD2880_MAX_FILTER_SIZE;
	dvb_spi->dmxdev.demux = &dvb_spi->demux.dmx;
	dvb_spi->dmxdev.capabilities = 0;
	ret = dvb_dmxdev_init(&dvb_spi->dmxdev,
				&dvb_spi->adapter);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: dvb_dmxdev_init() failed\n", __func__);
		goto fail_dmxdev;
	}

	dvb_spi->dmx_fe.source = DMX_FRONTEND_0;
	ret = dvb_spi->demux.dmx.add_frontend(&dvb_spi->demux.dmx,
						&dvb_spi->dmx_fe);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: add_frontend() failed\n", __func__);
		goto fail_dmx_fe;
	}

	ret = dvb_spi->demux.dmx.connect_frontend(&dvb_spi->demux.dmx,
						&dvb_spi->dmx_fe);
	if (ret < 0) {
		dev_err(&spi->dev, "%s: dvb_register_frontend() failed\n",
			__func__);
		goto fail_fe_conn;
	}

	dev_info(&spi->dev, "Sony CXD2880 has successfully attached.\n");

	return 0;

fail_fe_conn:
	dvb_spi->demux.dmx.remove_frontend(&dvb_spi->demux.dmx,
			&dvb_spi->dmx_fe);
fail_dmx_fe:
	dvb_dmxdev_release(&dvb_spi->dmxdev);
fail_dmxdev:
	dvb_dmx_release(&dvb_spi->demux);
fail_dmx:
	dvb_unregister_frontend(&dvb_spi->dvb_fe);
fail_frontend:
	dvb_frontend_detach(&dvb_spi->dvb_fe);
fail_attach:
	dvb_unregister_adapter(&dvb_spi->adapter);
fail_adapter:
	kfree(dvb_spi);
	return ret;
}

static int
cxd2880_spi_remove(struct spi_device *spi)
{
	struct cxd2880_dvb_spi *dvb_spi;

	if (!spi) {
		pr_err("%s: invalid arg\n", __func__);
		return -EINVAL;
	}

	dvb_spi = (struct cxd2880_dvb_spi *)dev_get_drvdata(&spi->dev);

	if (!dvb_spi) {
		pr_err("%s: failed\n", __func__);
		return -EINVAL;
	}
	dvb_spi->demux.dmx.remove_frontend(&dvb_spi->demux.dmx,
					&dvb_spi->dmx_fe);
	dvb_dmxdev_release(&dvb_spi->dmxdev);
	dvb_dmx_release(&dvb_spi->demux);
	dvb_unregister_frontend(&dvb_spi->dvb_fe);
	dvb_frontend_detach(&dvb_spi->dvb_fe);
	dvb_unregister_adapter(&dvb_spi->adapter);

	kfree(dvb_spi);
	dev_info(&spi->dev, "%s: cxd2880_spi remove ok.\n", __func__);

	return 0;
}

static const struct spi_device_id cxd2880_spi_id[] = {
	{ "cxd2880", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, cxd2880_spi_id);

static struct spi_driver cxd2880_spi_driver = {
	.driver	= {
		.name	= "cxd2880",
		.of_match_table = cxd2880_spi_of_match,
	},
	.id_table = cxd2880_spi_id,
	.probe    = cxd2880_spi_probe,
	.remove   = cxd2880_spi_remove,
};
module_spi_driver(cxd2880_spi_driver);

MODULE_DESCRIPTION(
"Sony CXD2880 DVB-T2/T tuner + demodulator drvier SPI adapter");
MODULE_AUTHOR("Sony Semiconductor Solutions Corporation");
MODULE_LICENSE("GPL v2");
