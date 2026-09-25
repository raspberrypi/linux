// SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause)
/*
 * NETC NTMP (NETC Table Management Protocol) 2.0 Library
 * Copyright 2025 NXP
 */

#include <linux/dma-mapping.h>
#include <linux/fsl/netc_global.h>
#include <linux/iopoll.h>
#include <linux/vmalloc.h>

#include "ntmp_private.h"

#define NETC_CBDR_TIMEOUT		1000 /* us */
#define NETC_CBDR_DELAY_US		10
#define NETC_CBDR_MR_EN			BIT(31)

#define NTMP_BASE_ADDR_ALIGN		128
#define NTMP_DATA_ADDR_ALIGN		32

/* Define NTMP Table ID */
#define NTMP_MAFT_ID			1
#define NTMP_RSST_ID			3

/* Generic Update Actions for most tables */
#define NTMP_GEN_UA_CFGEU		BIT(0)
#define NTMP_GEN_UA_STSEU		BIT(1)

#define NTMP_ENTRY_ID_SIZE		4
#define RSST_ENTRY_NUM			64
#define RSST_STSE_DATA_SIZE(n)		((n) * 8)
#define RSST_CFGE_DATA_SIZE(n)		(n)

int ntmp_init_cbdr(struct netc_cbdr *cbdr, struct device *dev,
		   const struct netc_cbdr_regs *regs)
{
	int cbd_num = NETC_CBDR_BD_NUM;
	size_t size;

	size = cbd_num * sizeof(union netc_cbd) + NTMP_BASE_ADDR_ALIGN;
	cbdr->addr_base = dma_alloc_coherent(dev, size, &cbdr->dma_base,
					     GFP_KERNEL);
	if (!cbdr->addr_base)
		return -ENOMEM;

	cbdr->swcbd = vcalloc(cbd_num, sizeof(struct netc_swcbd));
	if (!cbdr->swcbd) {
		dma_free_coherent(dev, size, cbdr->addr_base, cbdr->dma_base);
		return -ENOMEM;
	}

	cbdr->dma_size = size;
	cbdr->bd_num = cbd_num;
	cbdr->regs = *regs;
	cbdr->dev = dev;

	/* The base address of the Control BD Ring must be 128 bytes aligned */
	cbdr->dma_base_align =  ALIGN(cbdr->dma_base,  NTMP_BASE_ADDR_ALIGN);
	cbdr->addr_base_align = PTR_ALIGN(cbdr->addr_base,
					  NTMP_BASE_ADDR_ALIGN);

	mutex_init(&cbdr->ring_lock);

	cbdr->next_to_use = netc_read(cbdr->regs.pir);
	cbdr->next_to_clean = netc_read(cbdr->regs.cir) & NETC_CBDRCIR_INDEX;

	/* Step 1: Configure the base address of the Control BD Ring */
	netc_write(cbdr->regs.bar0, lower_32_bits(cbdr->dma_base_align));
	netc_write(cbdr->regs.bar1, upper_32_bits(cbdr->dma_base_align));

	/* Step 2: Configure the number of BDs of the Control BD Ring */
	netc_write(cbdr->regs.lenr, cbdr->bd_num);

	/* Step 3: Enable the Control BD Ring */
	netc_write(cbdr->regs.mr, NETC_CBDR_MR_EN);

	return 0;
}
EXPORT_SYMBOL_GPL(ntmp_init_cbdr);

static void ntmp_free_data_mem(struct device *dev, struct netc_swcbd *swcbd)
{
	if (unlikely(!swcbd->buf))
		return;

	dma_free_coherent(dev, swcbd->size + NTMP_DATA_ADDR_ALIGN,
			  swcbd->buf, swcbd->dma);
}

void ntmp_free_cbdr(struct netc_cbdr *cbdr)
{
	/* Disable the Control BD Ring */
	netc_write(cbdr->regs.mr, 0);

	for (int i = 0; i < cbdr->bd_num; i++)
		ntmp_free_data_mem(cbdr->dev, &cbdr->swcbd[i]);

	vfree(cbdr->swcbd);
	dma_free_coherent(cbdr->dev, cbdr->dma_size, cbdr->addr_base,
			  cbdr->dma_base);
	memset(cbdr, 0, sizeof(*cbdr));
}
EXPORT_SYMBOL_GPL(ntmp_free_cbdr);

static int ntmp_get_free_cbd_num(struct netc_cbdr *cbdr)
{
	return (cbdr->next_to_clean - cbdr->next_to_use - 1 +
		cbdr->bd_num) % cbdr->bd_num;
}

static union netc_cbd *ntmp_get_cbd(struct netc_cbdr *cbdr, int index)
{
	return &((union netc_cbd *)(cbdr->addr_base_align))[index];
}

static void ntmp_clean_cbdr(struct netc_cbdr *cbdr)
{
	int i = cbdr->next_to_clean;

	while ((netc_read(cbdr->regs.cir) & NETC_CBDRCIR_INDEX) != i) {
		union netc_cbd *cbd = ntmp_get_cbd(cbdr, i);
		struct netc_swcbd *swcbd = &cbdr->swcbd[i];

		ntmp_free_data_mem(cbdr->dev, swcbd);
		memset(swcbd, 0, sizeof(*swcbd));
		memset(cbd, 0, sizeof(*cbd));
		i = (i + 1) % cbdr->bd_num;
	}

	dma_wmb();
	cbdr->next_to_clean = i;
}

static void ntmp_select_and_lock_cbdr(struct ntmp_user *user,
				      struct netc_cbdr **cbdr)
{
	/* Currently only ENETC is supported, and it has only one command
	 * BD ring.
	 */
	*cbdr = &user->ring[0];

	mutex_lock(&(*cbdr)->ring_lock);
}

static void ntmp_unlock_cbdr(struct netc_cbdr *cbdr)
{
	mutex_unlock(&cbdr->ring_lock);
}

static int netc_xmit_ntmp_cmd(struct netc_cbdr *cbdr, union netc_cbd *cbd,
			      struct netc_swcbd *swcbd)
{
	union netc_cbd *cur_cbd;
	int i, err, used_bds;
	u16 status;
	u32 val;

	used_bds = cbdr->bd_num - ntmp_get_free_cbd_num(cbdr);
	if (unlikely(used_bds >= NETC_CBDR_CLEAN_WORK)) {
		ntmp_clean_cbdr(cbdr);
		if (unlikely(!ntmp_get_free_cbd_num(cbdr))) {
			ntmp_free_data_mem(cbdr->dev, swcbd);
			return -EBUSY;
		}
	}

	i = cbdr->next_to_use;
	cur_cbd = ntmp_get_cbd(cbdr, i);
	*cur_cbd = *cbd;
	cbdr->swcbd[i] = *swcbd;
	dma_wmb();

	/* Update producer index of both software and hardware */
	i = (i + 1) % cbdr->bd_num;
	cbdr->next_to_use = i;
	netc_write(cbdr->regs.pir, i);

	err = read_poll_timeout(netc_read, val,
				(val & NETC_CBDRCIR_INDEX) == i,
				NETC_CBDR_DELAY_US, NETC_CBDR_TIMEOUT,
				true, cbdr->regs.cir);
	if (unlikely(err))
		return err;

	if (unlikely(val & NETC_CBDRCIR_SBE)) {
		dev_err(cbdr->dev, "Command BD system bus error\n");
		return -EIO;
	}

	dma_rmb();
	/* Get the writeback command BD, because the caller may need
	 * to check some other fields of the response header.
	 */
	*cbd = *cur_cbd;

	/* Check the writeback error status */
	status = le16_to_cpu(cbd->resp_hdr.error_rr) & NTMP_RESP_ERROR;
	if (unlikely(status)) {
		dev_err(cbdr->dev, "Command BD error: 0x%04x\n", status);
		return -EIO;
	}

	return 0;
}

static int ntmp_alloc_data_mem(struct device *dev, struct netc_swcbd *swcbd,
			       void **buf_align)
{
	void *buf;

	buf = dma_alloc_coherent(dev, swcbd->size + NTMP_DATA_ADDR_ALIGN,
				 &swcbd->dma, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	swcbd->buf = buf;
	*buf_align = PTR_ALIGN(buf, NTMP_DATA_ADDR_ALIGN);

	return 0;
}

static void ntmp_fill_request_hdr(union netc_cbd *cbd, dma_addr_t dma,
				  int len, int table_id, int cmd,
				  int access_method)
{
	dma_addr_t dma_align;

	memset(cbd, 0, sizeof(*cbd));
	dma_align = ALIGN(dma, NTMP_DATA_ADDR_ALIGN);
	cbd->req_hdr.addr = cpu_to_le64(dma_align);
	cbd->req_hdr.len = cpu_to_le32(len);
	cbd->req_hdr.cmd = cmd;
	cbd->req_hdr.access_method = FIELD_PREP(NTMP_ACCESS_METHOD,
						access_method);
	cbd->req_hdr.table_id = table_id;
	cbd->req_hdr.ver_cci_rr = FIELD_PREP(NTMP_HDR_VERSION,
					     NTMP_HDR_VER2);
	/* For NTMP version 2.0 or later version */
	cbd->req_hdr.npf = cpu_to_le32(NTMP_NPF);
}

static void ntmp_fill_crd(struct ntmp_cmn_req_data *crd, u8 tblv,
			  u8 qa, u16 ua)
{
	crd->update_act = cpu_to_le16(ua);
	crd->tblv_qact = NTMP_TBLV_QACT(tblv, qa);
}

static void ntmp_fill_crd_eid(struct ntmp_req_by_eid *rbe, u8 tblv,
			      u8 qa, u16 ua, u32 entry_id)
{
	ntmp_fill_crd(&rbe->crd, tblv, qa, ua);
	rbe->entry_id = cpu_to_le32(entry_id);
}

static const char *ntmp_table_name(int tbl_id)
{
	switch (tbl_id) {
	case NTMP_MAFT_ID:
		return "MAC Address Filter Table";
	case NTMP_RSST_ID:
		return "RSS Table";
	default:
		return "Unknown Table";
	};
}

static int ntmp_delete_entry_by_id(struct ntmp_user *user, int tbl_id,
				   u8 tbl_ver, u32 entry_id, u32 req_len,
				   u32 resp_len)
{
	struct netc_swcbd swcbd = {
		.size = max(req_len, resp_len),
	};
	struct ntmp_req_by_eid *req;
	struct netc_cbdr *cbdr;
	union netc_cbd cbd;
	int err;

	err = ntmp_alloc_data_mem(user->dev, &swcbd, (void **)&req);
	if (err)
		return err;

	ntmp_fill_crd_eid(req, tbl_ver, 0, 0, entry_id);
	ntmp_fill_request_hdr(&cbd, swcbd.dma, NTMP_LEN(req_len, resp_len),
			      tbl_id, NTMP_CMD_DELETE, NTMP_AM_ENTRY_ID);

	ntmp_select_and_lock_cbdr(user, &cbdr);
	err = netc_xmit_ntmp_cmd(cbdr, &cbd, &swcbd);
	if (err)
		dev_err(user->dev,
			"Failed to delete entry 0x%x of %s, err: %pe",
			entry_id, ntmp_table_name(tbl_id), ERR_PTR(err));
	ntmp_unlock_cbdr(cbdr);

	return err;
}

static int ntmp_query_entry_by_id(struct netc_cbdr *cbdr, int tbl_id,
				  struct ntmp_req_by_eid *req,
				  struct netc_swcbd *swcbd,
				  bool compare_eid)
{
	u32 len = NTMP_LEN(sizeof(*req), swcbd->size);
	struct ntmp_cmn_resp_query *resp;
	int cmd = NTMP_CMD_QUERY;
	union netc_cbd cbd;
	u32 entry_id;
	int err;

	entry_id = le32_to_cpu(req->entry_id);
	if (le16_to_cpu(req->crd.update_act))
		cmd = NTMP_CMD_QU;

	/* Request header */
	ntmp_fill_request_hdr(&cbd, swcbd->dma, len, tbl_id, cmd,
			      NTMP_AM_ENTRY_ID);
	err = netc_xmit_ntmp_cmd(cbdr, &cbd, swcbd);
	if (err) {
		dev_err(cbdr->dev,
			"Failed to query entry 0x%x of %s, err: %pe\n",
			entry_id, ntmp_table_name(tbl_id), ERR_PTR(err));
		return err;
	}

	/* For a few tables, the first field of their response data is not
	 * entry_id, so directly return success.
	 */
	if (!compare_eid)
		return 0;

	resp = (struct ntmp_cmn_resp_query *)req;
	if (unlikely(le32_to_cpu(resp->entry_id) != entry_id)) {
		dev_err(cbdr->dev,
			"%s: query EID 0x%x doesn't match response EID 0x%x\n",
			ntmp_table_name(tbl_id), entry_id, le32_to_cpu(resp->entry_id));
		return -EIO;
	}

	return 0;
}

int ntmp_maft_add_entry(struct ntmp_user *user, u32 entry_id,
			struct maft_entry_data *maft)
{
	struct netc_swcbd swcbd = {
		.size = sizeof(struct maft_req_add),
	};
	struct maft_req_add *req;
	struct netc_cbdr *cbdr;
	union netc_cbd cbd;
	int err;

	err = ntmp_alloc_data_mem(user->dev, &swcbd, (void **)&req);
	if (err)
		return err;

	/* Set mac address filter table request data buffer */
	ntmp_fill_crd_eid(&req->rbe, user->tbl.maft_ver, 0, 0, entry_id);
	req->keye = maft->keye;
	req->cfge = maft->cfge;

	ntmp_fill_request_hdr(&cbd, swcbd.dma, NTMP_LEN(swcbd.size, 0),
			      NTMP_MAFT_ID, NTMP_CMD_ADD, NTMP_AM_ENTRY_ID);

	ntmp_select_and_lock_cbdr(user, &cbdr);
	err = netc_xmit_ntmp_cmd(cbdr, &cbd, &swcbd);
	if (err)
		dev_err(user->dev, "Failed to add MAFT entry 0x%x, err: %pe\n",
			entry_id, ERR_PTR(err));
	ntmp_unlock_cbdr(cbdr);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_maft_add_entry);

int ntmp_maft_query_entry(struct ntmp_user *user, u32 entry_id,
			  struct maft_entry_data *maft)
{
	struct netc_swcbd swcbd = {
		.size = sizeof(struct maft_resp_query),
	};
	struct maft_resp_query *resp;
	struct ntmp_req_by_eid *req;
	struct netc_cbdr *cbdr;
	int err;

	err = ntmp_alloc_data_mem(user->dev, &swcbd, (void **)&req);
	if (err)
		return err;

	ntmp_fill_crd_eid(req, user->tbl.maft_ver, 0, 0, entry_id);

	ntmp_select_and_lock_cbdr(user, &cbdr);
	err = ntmp_query_entry_by_id(cbdr, NTMP_MAFT_ID, req, &swcbd, true);
	if (err)
		goto unlock_cbdr;

	resp = (struct maft_resp_query *)req;
	maft->keye = resp->keye;
	maft->cfge = resp->cfge;

unlock_cbdr:
	ntmp_unlock_cbdr(cbdr);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_maft_query_entry);

int ntmp_maft_delete_entry(struct ntmp_user *user, u32 entry_id)
{
	return ntmp_delete_entry_by_id(user, NTMP_MAFT_ID, user->tbl.maft_ver,
				       entry_id, NTMP_EID_REQ_LEN, 0);
}
EXPORT_SYMBOL_GPL(ntmp_maft_delete_entry);

int ntmp_rsst_update_entry(struct ntmp_user *user, const u32 *table,
			   int count)
{
	struct rsst_req_update *req;
	struct netc_swcbd swcbd;
	struct netc_cbdr *cbdr;
	union netc_cbd cbd;
	int err, i;

	if (count != RSST_ENTRY_NUM)
		/* HW only takes in a full 64 entry table */
		return -EINVAL;

	swcbd.size = struct_size(req, groups, count);
	err = ntmp_alloc_data_mem(user->dev, &swcbd, (void **)&req);
	if (err)
		return err;

	/* Set the request data buffer */
	ntmp_fill_crd_eid(&req->rbe, user->tbl.rsst_ver, 0,
			  NTMP_GEN_UA_CFGEU | NTMP_GEN_UA_STSEU, 0);
	for (i = 0; i < count; i++)
		req->groups[i] = (u8)(table[i]);

	ntmp_fill_request_hdr(&cbd, swcbd.dma, NTMP_LEN(swcbd.size, 0),
			      NTMP_RSST_ID, NTMP_CMD_UPDATE, NTMP_AM_ENTRY_ID);

	ntmp_select_and_lock_cbdr(user, &cbdr);
	err = netc_xmit_ntmp_cmd(cbdr, &cbd, &swcbd);
	if (err)
		dev_err(user->dev, "Failed to update RSST entry, err: %pe\n",
			ERR_PTR(err));
	ntmp_unlock_cbdr(cbdr);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_rsst_update_entry);

int ntmp_rsst_query_entry(struct ntmp_user *user, u32 *table, int count)
{
	struct ntmp_req_by_eid *req;
	struct netc_swcbd swcbd;
	struct netc_cbdr *cbdr;
	union netc_cbd cbd;
	int err, i;
	u8 *group;

	if (count != RSST_ENTRY_NUM)
		/* HW only takes in a full 64 entry table */
		return -EINVAL;

	swcbd.size = NTMP_ENTRY_ID_SIZE + RSST_STSE_DATA_SIZE(count) +
		     RSST_CFGE_DATA_SIZE(count);
	err = ntmp_alloc_data_mem(user->dev, &swcbd, (void **)&req);
	if (err)
		return err;

	/* Set the request data buffer */
	ntmp_fill_crd_eid(req, user->tbl.rsst_ver, 0, 0, 0);
	ntmp_fill_request_hdr(&cbd, swcbd.dma, NTMP_LEN(sizeof(*req), swcbd.size),
			      NTMP_RSST_ID, NTMP_CMD_QUERY, NTMP_AM_ENTRY_ID);

	ntmp_select_and_lock_cbdr(user, &cbdr);
	err = netc_xmit_ntmp_cmd(cbdr, &cbd, &swcbd);
	if (err) {
		dev_err(user->dev, "Failed to query RSST entry, err: %pe\n",
			ERR_PTR(err));
		goto unlock_cbdr;
	}

	group = (u8 *)req;
	group += NTMP_ENTRY_ID_SIZE + RSST_STSE_DATA_SIZE(count);
	for (i = 0; i < count; i++)
		table[i] = group[i];

unlock_cbdr:
	ntmp_unlock_cbdr(cbdr);

	return err;
}
EXPORT_SYMBOL_GPL(ntmp_rsst_query_entry);

MODULE_DESCRIPTION("NXP NETC Library");
MODULE_LICENSE("Dual BSD/GPL");
