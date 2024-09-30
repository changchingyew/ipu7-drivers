// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2020 - 2024 Intel Corporation

#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/pm_runtime.h>
#include <linux/kthread.h>
#include <linux/init_task.h>
#include <uapi/linux/sched/types.h>
#include <linux/module.h>
#include <linux/fs.h>

#include "ipu7.h"
#include "ipu7-bus.h"
#include "ipu7-buttress-regs.h"
#include "ipu7-psys.h"
#include "ipu7-platform-regs.h"
#include "ipu7-fw-psys.h"

static bool early_pg_transfer;
module_param(early_pg_transfer, bool, 0664);
MODULE_PARM_DESC(early_pg_transfer,
		 "Copy PGs back to user after resource allocation");

bool enable_power_gating = true;
module_param(enable_power_gating, bool, 0664);
MODULE_PARM_DESC(enable_power_gating, "enable power gating");

#define DOMAIN_POWE_TIMEOUT_US		(200 * USEC_PER_MSEC)

static const struct ipu7_msg_to_str ps_fw_msg[] = {
	{IPU_MSG_TYPE_RESERVED, "IPU_MSG_TYPE_RESERVED"},
	{IPU_MSG_TYPE_INDIRECT, "IPU_MSG_TYPE_INDIRECT"},
	{IPU_MSG_TYPE_DEV_LOG, "IPU_MSG_TYPE_DEV_LOG"},
	{IPU_MSG_TYPE_GENERAL_ERR, "IPU_MSG_TYPE_GENERAL_ERR"},
	{IPU_MSG_TYPE_DEV_OPEN, "IPU_MSG_TYPE_DEV_OPEN"},
	{IPU_MSG_TYPE_DEV_OPEN_ACK, "IPU_MSG_TYPE_DEV_OPEN_ACK"},
	{IPU_MSG_TYPE_GRAPH_OPEN, "IPU_MSG_TYPE_GRAPH_OPEN"},
	{IPU_MSG_TYPE_GRAPH_OPEN_ACK, "IPU_MSG_TYPE_GRAPH_OPEN_ACK"},
	{IPU_MSG_TYPE_TASK_REQ, "IPU_MSG_TYPE_TASK_REQ"},
	{IPU_MSG_TYPE_TASK_DONE, "IPU_MSG_TYPE_TASK_DONE"},
	{IPU_MSG_TYPE_GRAPH_CLOSE, "IPU_MSG_TYPE_GRAPH_CLOSE"},
	{IPU_MSG_TYPE_GRAPH_CLOSE_ACK, "IPU_MSG_TYPE_GRAPH_CLOSE_ACK"},
	{IPU_MSG_TYPE_DEV_CLOSE, "IPU_MSG_TYPE_DEV_CLOSE"},
	{IPU_MSG_TYPE_DEV_CLOSE_ACK, "IPU_MSG_TYPE_DEV_CLOSE_ACK"},
	{IPU_MSG_TYPE_N, "IPU_MSG_TYPE_N"},
};

void ipu7_psys_subdomains_power(struct ipu7_psys *psys, bool on)
{
	struct device *dev = &psys->adev->auxdev.dev;
	u32 val, req;
	int ret;
	void __iomem *base = psys->adev->isp->base;

	/* power domain req */
	req = on ? IPU_PSYS_DOMAIN_POWER_MASK : 0;
	val = readl(base + BUTTRESS_REG_PS_DOMAINS_STATUS);

	dev_dbg(dev, "power %s psys sub-domains. status: 0x%x\n",
		on ? "UP" : "DOWN", val);
	if ((val & IPU_PSYS_DOMAIN_POWER_MASK) == req) {
		dev_warn(dev,
			 "psys sub-domains power already in request state.\n");
		return;
	}
	writel(req, base + BUTTRESS_REG_PS_WORKPOINT_DOMAIN_REQ);
	ret = readl_poll_timeout(base + BUTTRESS_REG_PS_DOMAINS_STATUS,
				 val,
				 !(val & IPU_PSYS_DOMAIN_POWER_IN_PROGRESS) &&
				 ((val & IPU_PSYS_DOMAIN_POWER_MASK) == req),
				 100, DOMAIN_POWE_TIMEOUT_US);
	if (ret)
		dev_err(dev,
			"Psys sub-domains power %s timeout! status: 0x%x\n",
			on ? "UP" : "DOWN", val);
}

void ipu7_psys_setup_hw(struct ipu7_psys *psys)
{
	void __iomem *base = psys->pdata->base;
	u32 val = IRQ_FROM_LOCAL_FW;

	/* Enable TO_SW IRQ from FW */
	writel(val, base + IPU_REG_PSYS_TO_SW_IRQ_CNTL_CLEAR);
	writel(val, base + IPU_REG_PSYS_TO_SW_IRQ_CNTL_MASK);
	writel(val, base + IPU_REG_PSYS_TO_SW_IRQ_CNTL_ENABLE);

	/* correct the initial printf configuration */
	writel(0x2, base + PS_UC_CTRL_BASE + PRINTF_AXI_CNTL);
}

static struct ipu7_psys_stream*
ipu7_psys_get_ip_from_fh(struct ipu7_psys *psys, u8 graph_id)
{
	struct ipu7_psys_fh *fh;

	list_for_each_entry(fh, &psys->fhs, list) {
		if (fh->ip->graph_id == graph_id)
			return fh->ip;
	}

	return NULL;
}

static void ipu7_psys_handle_graph_open_ack(struct ipu7_psys *psys,
					    const void *buffer)
{
	const struct ipu7_msg_graph_open_ack *ack_msg =
		(const struct ipu7_msg_graph_open_ack *)buffer;
	const struct ia_gofo_msg_header_ack  *ack_header = &ack_msg->header;
	struct ipu7_psys_stream *ip;
	struct device *dev = &psys->dev;
	const struct ia_gofo_tlv_header *msg_tlv_item;
	u16 num_items;
	u16 head_offset;
	u32 i;

	dev_dbg(dev, "[ACK]%s: graph_id: %d\n", __func__, ack_msg->graph_id);

	if (ack_msg->graph_id > (u8)IPU_PSYS_MAX_GRAPH_NUMS) {
		dev_err(dev, "%s: graph id %d\n", __func__, ack_msg->graph_id);
		return;
	}

	ip = ipu7_psys_get_ip_from_fh(psys, ack_msg->graph_id);
	if (!ip) {
		dev_err(dev, "%s: graph id %d\n", __func__, ack_msg->graph_id);
		return;
	}

	if (ip->graph_state != IPU_MSG_GRAPH_STATE_OPEN_WAIT) {
		dev_err(dev, "%s state %d\n", __func__, ip->graph_state);
		goto open_graph_exit;
	}

	num_items = ack_header->header.msg_options.num_elems;
	if (!num_items) {
		dev_err(dev, "%s, num_items is 0\n", __func__);
		goto open_graph_exit;
	}

	head_offset = ack_header->header.msg_options.head_offset;
	msg_tlv_item = (const struct ia_gofo_tlv_header *)
		((u8 *)&ack_header->header.msg_options + head_offset);

	if (!msg_tlv_item) {
		dev_err(dev, "%s: failed to get tlv item\n", __func__);
		goto open_graph_exit;
	}

	for (i = 0U; i < num_items; i++) {
		u32 option_type = msg_tlv_item->tlv_type;
		u32 option_bytes = msg_tlv_item->tlv_len32 *
			TLV_ITEM_ALIGNMENT;
		struct ipu7_msg_graph_open_ack_task_q_info *msg_option =
			(struct ipu7_msg_graph_open_ack_task_q_info *)msg_tlv_item;

		switch (option_type) {
		case IPU_MSG_GRAPH_ACK_TASK_Q_INFO:
			/*
			 * Should do check that:
			 * - Each managed node has a queue ID
			 * - Queue ID's are sane
			 */
			dev_dbg(dev, "[ACK]set node_ctx_id %d q_id %d\n",
				msg_option->node_ctx_id,  msg_option->q_id);
			ip->q_id[msg_option->node_ctx_id] = msg_option->q_id;
			break;
		default:
			/*
			 * Only one option supported
			 */
			dev_err(dev, "not supported %u\n", option_type);
			break;
		}

		msg_tlv_item = (struct ia_gofo_tlv_header *)
			(((u8 *)msg_tlv_item) + option_bytes);
	}

	ip->graph_state = IPU_MSG_GRAPH_STATE_OPEN;

open_graph_exit:
	complete(&ip->graph_open);
}

static int ipu7_psys_handle_task_done(struct ipu7_psys *psys,
				      void *buffer, u32 error)
{
	const struct ipu7_msg_task_done *ack_msg =
		(const struct ipu7_msg_task_done *)buffer;
	const struct ia_gofo_msg_header_ack *ack_header = &ack_msg->header;
	const struct ia_gofo_msg_header *msg_header = &ack_header->header;
	struct ipu_psys_task_queue *task_queue;
	struct device *dev = &psys->dev;
	struct ipu7_psys_stream *ip;
	u32 i, index_id = 0;

	if (ack_msg->graph_id > (u8)IPU_PSYS_MAX_GRAPH_NUMS) {
		dev_err(dev, "%s: graph id %d\n", __func__, ack_msg->graph_id);
		return -EINVAL;
	}

	ip = ipu7_psys_get_ip_from_fh(psys, ack_msg->graph_id);
	if (!ip) {
		dev_err(dev, "%s: graph id %d\n", __func__, ack_msg->graph_id);
		return -EINVAL;
	}

	mutex_lock(&ip->event_mutex);
	task_queue = (struct ipu_psys_task_queue *)msg_header->user_token;
	if (!task_queue) {
		dev_err(dev, "%s: task_token is NULL\n", __func__);
		mutex_unlock(&ip->event_mutex);
		return -EINVAL;
	}

	if (task_queue->task_state != IPU_MSG_TASK_STATE_WAIT_DONE) {
		dev_err(dev, "%s: task_queue %d graph %d node %d error %d\n",
			__func__, task_queue->index, ack_msg->graph_id,
			ack_msg->node_ctx_id, task_queue->task_state);

		mutex_unlock(&ip->event_mutex);
		return -ENOENT;
	}

	index_id = task_queue->index;
	task_queue->available = 1;
	task_queue->task_state = IPU_MSG_TASK_STATE_DONE;
	dev_dbg(dev, "%s: task_token(%p, %d)\n", __func__, task_queue,
		index_id);

	if (ip->event_queue[ip->event_write_index].available != 0U) {
		i = ip->event_write_index;
		ip->event_queue[i].graph_id = ack_msg->graph_id;
		ip->event_queue[i].node_ctx_id = ack_msg->node_ctx_id;
		ip->event_queue[i].frame_id = ack_msg->frame_id;
		ip->event_queue[i].err_code = error;
		ip->event_queue[i].available = 0U;
		ip->event_write_index = (i + 1U) % MAX_TASK_EVENT_QUEUE_SIZE;
	} else {
		dev_dbg(dev, "%s: event queue is full(%d)\n", __func__,
			MAX_TASK_EVENT_QUEUE_SIZE);
	}
	mutex_unlock(&ip->event_mutex);

	wake_up_interruptible(&ip->fh->wait);

	return 0;
}

static void ipu7_psys_handle_graph_close_ack(struct ipu7_psys *psys,
					     void *buffer)
{
	struct ipu7_msg_graph_close_ack *ack_msg =
		(struct ipu7_msg_graph_close_ack *)buffer;
	struct device *dev = &psys->dev;
	struct ipu7_psys_stream *ip;

	dev_dbg(dev, "[ACK]%s:graph_id: %d\n", __func__, ack_msg->graph_id);

	if (ack_msg->graph_id > (u8)IPU_PSYS_MAX_GRAPH_NUMS) {
		dev_err(dev, "%s: graph id %d\n", __func__, ack_msg->graph_id);
		return;
	}

	ip = ipu7_psys_get_ip_from_fh(psys, ack_msg->graph_id);
	if (!ip) {
		dev_err(dev, "%s: graph id %d\n", __func__, ack_msg->graph_id);
		return;
	}

	if (ip->graph_state != IPU_MSG_GRAPH_STATE_CLOSE_WAIT) {
		dev_err(dev, "%s state %d\n", __func__, ip->graph_state);
		goto graph_close_exit;
	}

	ip->graph_state = IPU_MSG_GRAPH_STATE_CLOSED;

graph_close_exit:
	complete(&ip->graph_close);
}

void ipu7_psys_handle_events(struct ipu7_psys *psys)
{
	const struct ia_gofo_msg_header_ack  *ack_header;
	u8 buffer[FWPS_MSG_FW2HOST_MAX_SIZE];
	struct device *dev = &psys->dev;
	u32 error = 0;
	int ret = 0;

	do {
#ifdef ENABLE_FW_OFFLINE_LOGGER
		ipu7_fw_psys_get_log(psys);
#endif
		ret = ipu7_fw_psys_event_handle(psys, buffer);
		if (ret)
			break;

		ack_header = (const struct ia_gofo_msg_header_ack *)buffer;

		dev_dbg(dev, "[ACK]%s: ack msg %s received\n", __func__,
			ps_fw_msg[ack_header->header.tlv_header.tlv_type].msg);

		if (!IA_GOFO_MSG_ERR_IS_OK(ack_header->err)) {
			dev_err(dev, "group %d, code %d, detail: %d, %d\n",
				ack_header->err.err_group,
				ack_header->err.err_code,
				ack_header->err.err_detail[0],
				ack_header->err.err_detail[1]);
			error = (ack_header->err.err_group ==
				 IPU_MSG_ERR_GROUP_TASK) ?
				IPU_PSYS_EVT_ERROR_FRAME :
				IPU_PSYS_EVT_ERROR_INTERNAL;
		}

		switch (ack_header->header.tlv_header.tlv_type) {
		case IPU_MSG_TYPE_GRAPH_OPEN_ACK:
			ipu7_psys_handle_graph_open_ack(psys, buffer);
			break;
		case IPU_MSG_TYPE_TASK_DONE:
			ipu7_psys_handle_task_done(psys, buffer, error);
			break;
		case IPU_MSG_TYPE_GRAPH_CLOSE_ACK:
			ipu7_psys_handle_graph_close_ack(psys, buffer);
			break;
		case IPU_MSG_TYPE_GENERAL_ERR:
			/* already printed the log above for general error */
			break;
		default:
			dev_err(&psys->dev, "Unknown type %d\n",
				ack_header->header.tlv_header.tlv_type);
		}
	} while (1);
}

static int ipu7_psys_complete_event(struct ipu7_psys_stream *ip,
				    struct ipu_psys_event *event)
{
	u32 i;

	mutex_lock(&ip->event_mutex);
	/* Check if there is already an event in the queue */
	i = ip->event_read_index;
	if (ip->event_queue[i].available == 1U) {
		mutex_unlock(&ip->event_mutex);
		return -EAGAIN;
	}
	ip->event_read_index = (i + 1U) % MAX_TASK_EVENT_QUEUE_SIZE;
	event->graph_id = ip->event_queue[i].graph_id;
	event->node_ctx_id = ip->event_queue[i].node_ctx_id;
	event->frame_id = ip->event_queue[i].frame_id;
	event->error = ip->event_queue[i].err_code;
	ip->event_queue[i].available = 1;
	mutex_unlock(&ip->event_mutex);

	dev_dbg(&ip->fh->psys->dev, "event %d graph %d cb %d frame %d dequeued",
		i, event->graph_id, event->node_ctx_id, event->frame_id);

	return 0;
}

long ipu7_ioctl_dqevent(struct ipu_psys_event *event,
			struct ipu7_psys_fh *fh, unsigned int f_flags)
{
	struct ipu7_psys *psys = fh->psys;
	int rval = 0;

	dev_dbg(&psys->adev->auxdev.dev, "IOC_DQEVENT\n");

	if (!(f_flags & O_NONBLOCK)) {
		rval = wait_event_interruptible(fh->wait,
						!ipu7_psys_complete_event(fh->ip, event));
		if (rval == -ERESTARTSYS)
			return rval;
	} else {
		rval = ipu7_psys_complete_event(fh->ip, event);
	}

	return rval;
}
