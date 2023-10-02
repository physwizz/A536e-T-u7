/*
 * Samsung Exynos SoC series NPU driver
 *
 * Copyright (c) 2017 Samsung Electronics Co., Ltd.
 *	http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>

#include "mailbox_ipc.h"
#include "../../npu-util-regs.h"

#define LINE_TO_SGMT(sgmt_len, ptr)	((ptr) & ((sgmt_len) - 1))

void dbg_print_msg(struct message *msg, struct command *cmd);
void dbg_print_ctrl(volatile struct mailbox_ctrl *mCtrl);
void dbg_print_hdr(volatile struct mailbox_hdr *mHdr);

static int wait_for_mem_value(struct npu_system *system, volatile u32 *addr, const u32 expected, int ms_timeout);
static u32 get_logging_time_ms(void);

//TODO : interface should assign the ioremaped address with (NPU_IOMEM_SRAM_END, NPU_MAILBOX_SIZE)
int mailbox_init(volatile struct mailbox_hdr *header, struct npu_system *system)
{
	//Memory Alloc, and Wait Sig.
	int ret = 0;
	int i, j;
	u32 cur_ofs = 0;
	volatile struct mailbox_ctrl ctrl[MAX_MAILBOX];

	npu_info("cold(%d) mailbox initialize: start, header base at %pK\n",
			system->fw_cold_boot, header);

	if (!header) {
		npu_err("invalid mailbox header pointer\n");
		ret = -EFAULT;
		goto err_exit;
	}

	header->debug_time = get_logging_time_ms();

#ifndef CONFIG_NPU_USE_HW_DEVICE
	cur_ofs = NPU_MAILBOX_SECTION_CONFIG[0];  /* Start position of data_ofs */
#endif
	for (i = 0; i < MAX_MAILBOX - 1; i++) {
		if (unlikely(NPU_MAILBOX_SECTION_CONFIG[i] == 0)) {
			npu_err("invalid mailbox size on %d th mailbox\n", i);
			BUG_ON(1);
		}
#ifndef CONFIG_NPU_USE_HW_DEVICE
		cur_ofs += NPU_MAILBOX_SECTION_CONFIG[i + 1];
#else
		cur_ofs += NPU_MAILBOX_SECTION_CONFIG[i];
#endif
		ctrl[i].sgmt_ofs = cur_ofs;
		ctrl[i].sgmt_len = NPU_MAILBOX_SECTION_CONFIG[i + 1];
		ctrl[i].wptr = ctrl[i].rptr = 0;
	}

	j = 0;
	for (i = 0; i < (MAX_MAILBOX - 1) / 2; i++, j++) {
		header->h2fctrl[i] = ctrl[j];
	}
	for (i = 0; i < (MAX_MAILBOX - 1) / 2; i++, j++) {
		header->f2hctrl[i] = ctrl[j];
	}
	BUG_ON(j > (MAX_MAILBOX - 1));

	mb();

	if (!system->fw_cold_boot) {
		header->signature2 = MAILBOX_SIGNATURE2;
	} else {
#if (CONFIG_NPU_MAILBOX_VERSION == 9)
		header->boottype = 0;
#endif

		header->max_slot = 0;//TODO : TBD in firmware policy.
		/* half low 16 bits is mailbox ipc version, half high 16 bits is command version */
		header->version = ((CONFIG_NPU_COMMAND_VERSION << 16) |
				CONFIG_NPU_MAILBOX_VERSION);
		npu_info("header version \t: %08X\n", header->version);
		header->log_level = 192;

		/* Write Signature */
		header->signature2 = MAILBOX_SIGNATURE2;
	}

	npu_info("mailbox initialize: wait for firmware boot signature.\n");
	ret = wait_for_mem_value(system, &(header->signature1), MAILBOX_SIGNATURE1, 3000);
	if (ret) {
		npu_err("init. error(%d) in firmware waiting\n", ret);
		// dbg_print_hdr(header);
		goto err_exit;
	}
	npu_info("header signature \t: %X\n", header->signature1);

	npu_info("init. success in NPU mailbox\n");

err_exit:
	return ret;
}


void mailbox_deinit(volatile struct mailbox_hdr *header, struct npu_system *system)
{
#if (CONFIG_NPU_MAILBOX_VERSION == 9)
#define BOOT_TYPE_WARM_BOOT         (0xb0080c0d)

	system->fw_cold_boot = (header->boottype == BOOT_TYPE_WARM_BOOT) ? (false) : (true);
	npu_info("cold(%d) requested by fw, boottype(%#x)\n",
			system->fw_cold_boot, header->boottype);

	header->signature1 = 0;
	header->signature2 = 0;
#else
	(void)header;
	system->fw_cold_boot = true;
#endif
}

/*
 * Busy-wait for the contents of address is set to expected value,
 * up to ms_timeout ms
 * return value:
 *  0 : OK
 *  -ETIMEDOUT : Timeout
 */
static int wait_for_mem_value(struct npu_system *system, volatile u32 *addr, const u32 expected, int ms_timeout)
{
	int ret = 0;
	int i;
	u32 v = 0;

	for (i = 0; i < ms_timeout; i++) {
		v = readl(addr);
		if (v == expected) {
			ret = 0;
			goto ok_exit;
		}
		mdelay(1);
		if (!(i % 100)) {
			npu_info("waiting Frimware signature\n");
#ifdef CONFIG_NPU_USE_HW_DEVICE
			ret = npu_cmd_map(system, "cpupc");
			if (ret) {
				npu_err("fail(%d) in npu_cmd_map for cpupc\n", ret);
				ret = -EFAULT;
				goto ok_exit;
			}
#endif
		}
	}
	/* Timed-out */
	npu_err("timeout after waiting %d(ms). Current value: [0x%08x]@%pK\n",
		ms_timeout, v, addr);
	ret = -ETIMEDOUT;
ok_exit:
	return ret;
}

/*
 * Get the current time for kmsg in miliseconds unit
 */
static u32 get_logging_time_ms(void)
{
	unsigned long long kmsg_time_ns = sched_clock();

	/* Convert to ms */
	return (u32)(kmsg_time_ns / 1000 / 1000);
}

static inline u32 __get_readable_size(u32 sgmt_len, u32 wptr, u32 rptr)
{
	return wptr - rptr;
}

static inline u32 __get_writable_size(u32 sgmt_len, u32 wptr, u32 rptr)
{
	return sgmt_len - __get_readable_size(sgmt_len, wptr, rptr);
}

static inline u32 __copy_message_from_line(char *base, u32 sgmt_len, u32 rptr, struct message *msg)
{
	msg->magic	= *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, rptr + 0x00));
	msg->mid	= *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, rptr + 0x04));
	msg->command	= *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, rptr + 0x08));
	msg->length	= *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, rptr + 0x0C));
	/*
	 * actually, self is not requried to copy
	 * msg->self	= *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, rptr + 0x10));
	 */
	msg->data	= *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, rptr + 0x14));

	return rptr + sizeof(struct message);
}

static inline u32 __copy_message_to_line(char *base, u32 sgmt_len, u32 wptr, const struct message *msg)
{
	*(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, wptr + 0x00)) = msg->magic;
	*(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, wptr + 0x04)) = msg->mid;
	*(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, wptr + 0x08)) = msg->command;
	*(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, wptr + 0x0C)) = msg->length;
	/*
	 * actually, self is not requried to copy
	 * *(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, wptr + 0x10)) = msg->self;
	 */
	*(volatile u32 *)(base + LINE_TO_SGMT(sgmt_len, wptr + 0x14)) = msg->data;

	return wptr + sizeof(struct message);
}

static inline u32 __copy_command_from_line(char *base, u32 sgmt_len, u32 rptr, void *cmd, u32 cmd_size)
{
	/* need to reimplement accroding to user environment */
	memcpy(cmd, base + LINE_TO_SGMT(sgmt_len, rptr), cmd_size);
	return rptr + cmd_size;
}

static inline u32 __copy_command_to_line(char *base, u32 sgmt_len, u32 wptr, const void *cmd, u32 cmd_size)
{
	/* need to reimplement accroding to user environment */
	//npu_info("base : %pK\t Move : %08X\t dst : %pK\n",
	//	base, LINE_TO_SGMT(sgmt_len, wptr), (base + LINE_TO_SGMT(sgmt_len, wptr)));
	//npu_info("sgmt_len : %d\t wptr : %d\t size of Cmd : %d\n", sgmt_len, wptr, cmd_size);
	memcpy(base + LINE_TO_SGMT(sgmt_len, wptr), cmd, cmd_size);

	return wptr + cmd_size;
}

/* be careful to use this function, ptr should be virtual address not physical */
void *mbx_ipc_translate(char *underlay, volatile struct mailbox_ctrl *ctrl, u32 ptr)
{
	char *base;

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	return base + LINE_TO_SGMT(ctrl->sgmt_len, ptr);
}

static void __mbx_ipc_print(char *underlay, volatile struct mailbox_ctrl *ctrl, u32 log_level)
{
	u32 wptr, rptr, sgmt_len;
	struct message msg;
	struct command cmd;
	char *base;

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;

	npu_dbg("   MAGIC        MID      CMD       LEN       DATA       PAYLOAD    LEN\n");
	npu_dbg("-----------------------------------------------------------------------------------\n");

	while (rptr < wptr) {
		__copy_message_from_line(base, sgmt_len, rptr, &msg);
		if (msg.length != sizeof(struct command)) {
			npu_err("Please check the msg->length\n");
			return;
		}
		/* copy_command should copy only command part else payload */
		__copy_command_from_line(base, sgmt_len, msg.data, &cmd, sizeof(struct command));

		if (msg.magic == MESSAGE_MARK) {
			rptr = msg.data + msg.length;
			continue;
		}

		switch (log_level) {
		case (NPU_LOG_DBG):
			npu_dbg(
				"0x%08X %8d %8d    0x%08X 0x%08X 0x%08X 0x%08X\n",
				msg.magic, msg.mid, msg.command, msg.length,
				msg.data, cmd.payload, cmd.length);
			break;
		case (NPU_LOG_INFO):
			npu_info(
				"0x%08X %8d %8d    0x%08X 0x%08X 0x%08X 0x%08X\n",
				msg.magic, msg.mid, msg.command, msg.length,
				msg.data, cmd.payload, cmd.length);
			break;
		case (NPU_LOG_ERR):
			dbg_print_msg(&msg, &cmd);
			break;
		default:
			break;
		}

		rptr = msg.data + msg.length;
	}
	npu_dbg("-----------------------------------------------------------------------------------\n");
}

void mbx_ipc_print(char *underlay, volatile struct mailbox_ctrl *ctrl, u32 log_level)
{
	__mbx_ipc_print(underlay, ctrl, log_level);
}
void mbx_ipc_print_dbg(char *underlay, volatile struct mailbox_ctrl *ctrl)
{
	__mbx_ipc_print(underlay, ctrl, NPU_LOG_DBG);
}

int mbx_ipc_put(char *underlay, volatile struct mailbox_ctrl *ctrl, struct message *msg, struct command *cmd)
{
	int ret = 0;
	u32 writable_size, readable_size;
	u32 wptr, rptr, sgmt_len;
	u32 cmd_str_wptr, cmd_end_wptr;
	u32 ofs_to_next_sgmt;
	char *base;

	if ((msg == NULL) || (msg->length == 0) || (cmd == NULL)) {
		ret = -EPARAM;
		goto p_err;
	}

	if (msg->length != sizeof(struct command)) {
		ret = -EALIGN;
		dbg_print_msg(msg, cmd);
		goto p_err;
	}

	if ((!underlay) || (!ctrl)) {
		ret = -EINVAL;
		goto p_err;
	}

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;
	//npu_info("rptr : %d, \t wptr : %d\n", rptr, wptr);

	writable_size = __get_writable_size(sgmt_len, wptr, rptr);
	if (writable_size < sizeof(struct message)) {
		ret = -ERESOURCE;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, cmd);
		goto p_err;
	}

	cmd_str_wptr = wptr + sizeof(struct message);
	cmd_end_wptr = cmd_str_wptr + msg->length;

	ofs_to_next_sgmt = sgmt_len - LINE_TO_SGMT(sgmt_len, cmd_str_wptr);
	if (ofs_to_next_sgmt < msg->length) {
		cmd_str_wptr += ofs_to_next_sgmt;
		cmd_end_wptr += ofs_to_next_sgmt;
	}
	//npu_info("cmd_str_wptr : %d\t cmd_end_wptr : %d.\n", cmd_str_wptr, cmd_end_wptr);
	readable_size = __get_readable_size(sgmt_len, cmd_end_wptr, rptr);
	if (readable_size > sgmt_len) {
		ret = -ERESOURCE;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, cmd);
		goto p_err;
	}

	msg->magic = MESSAGE_MAGIC;
	msg->data = cmd_str_wptr;

	__copy_message_to_line(base, sgmt_len, wptr, msg);
	//npu_info("sgmt_len : %d\t wptr : %d\t cmd_str_wptr : %d\n", sgmt_len, wptr, cmd_str_wptr);
	/* if payload is a continous thing behind of command, payload should be updated */
	if (msg->length > sizeof(struct command)) {
		cmd->payload = (u32)(cmd_str_wptr + sizeof(struct command));
		npu_info("update cmd->payload: %d\n", cmd->payload);
	}
	__copy_command_to_line(base, sgmt_len, cmd_str_wptr, cmd, sizeof(struct command));
	// npu_memory_sync_for_device();
	/* We should guarantee msg and cmd are written before update wptr */
	dmb(st);
	ctrl->wptr = cmd_end_wptr;
	dsb(st);

p_err:
	return ret;
}

/*
 * return value
 * plus value : readed size
 * minus value : error code
 * zero : nothing to read
*/
int mbx_ipc_peek_msg(char *underlay, volatile struct mailbox_ctrl *ctrl, struct message *msg)
{
	int ret = 0;
	u32 readable_size, updated_rptr;
	u32 wptr, rptr, sgmt_len;
	char *base;

	if (msg == NULL || underlay == NULL) {
		ret = -EINVAL;
		goto p_err;
	}

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;
	readable_size = __get_readable_size(sgmt_len, wptr, rptr);
	if (readable_size == 0) {
		/* 0 read size means there is no any message but it's ok */
		ret = 0;
		goto p_err;
	} else if (readable_size < sizeof(struct message)) {
		ret = -ERESOURCE;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, NULL);
		goto p_err;
	}
	updated_rptr = __copy_message_from_line(base, sgmt_len, rptr, msg);
	if (msg->magic != MESSAGE_MAGIC) {
		ret = -EINVAL;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, NULL);
		goto p_err;
	}
	if (msg->length != sizeof(struct command)) {
		ret = -ERESOURCE;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, NULL);
		goto p_err;
	}
	msg->self = rptr;
	//ctrl->rptr = updated_rptr;
	ret = sizeof(struct message);

p_err:
	return ret;
}

/*
 * return value
 * plus value : readed size
 * minus value : error code
 * zero : nothing to read
*/
int mbx_ipc_get_msg(char *underlay, volatile struct mailbox_ctrl *ctrl, struct message *msg)
{
	int ret = 0;
	u32 readable_size, updated_rptr;
	u32 wptr, rptr, sgmt_len;
	char *base;

	if (msg == NULL) {
		ret = -EINVAL;
		goto p_err;
	}

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;

	readable_size = __get_readable_size(sgmt_len, wptr, rptr);
	if (readable_size == 0) {
		/* 0 read size means there is no any message but it's ok */
		ret = 0;
		goto p_err;
	} else if (readable_size < sizeof(struct message)) {
		ret = -ERESOURCE;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, NULL);
		goto p_err;
	}
	updated_rptr = __copy_message_from_line(base, sgmt_len, rptr, msg);
	if (msg->magic != MESSAGE_MAGIC) {
		ret = -EINVAL;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, NULL);
		goto p_err;
	}

	if (msg->length != sizeof(struct command)) {
		ret = -ERESOURCE;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, NULL);
		goto p_err;
	}

	msg->self = rptr;
	ctrl->rptr = updated_rptr;
	ret = sizeof(struct message);

p_err:
	return ret;
}


int mbx_ipc_get_cmd(char *underlay, volatile struct mailbox_ctrl *ctrl, struct message *msg, struct command *cmd)
{
	int ret = 0;
	u32 readable_size, updated_rptr;
	u32 wptr, rptr, sgmt_len;
	char *base;

	if ((msg == NULL) || (cmd == NULL)) {
		ret = -EINVAL;
		goto p_err;
	}

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;

	readable_size = __get_readable_size(sgmt_len, wptr, rptr);
	if (readable_size < sizeof(struct command)) {
		ret = -EINVAL;
		dbg_print_ctrl(ctrl);
		dbg_print_msg(msg, cmd);
		goto p_err;
	}

	updated_rptr = __copy_command_from_line(base, sgmt_len, msg->data, cmd, sizeof(struct command));

	ctrl->rptr = updated_rptr;

p_err:
	return ret;
}


int mbx_ipc_ref_msg(char *underlay, volatile struct mailbox_ctrl *ctrl, struct message *prev, struct message *next)
{
	int ret = 0;
	u32 readable_size, msg_str_rptr;
	u32 wptr, rptr, sgmt_len;
	char *base;

	if (next == NULL) {
		ret = -EINVAL;
		goto p_err;
	}

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;

	readable_size = __get_readable_size(sgmt_len, wptr, rptr);
	if (readable_size < sizeof(struct message)) {
		ret = -ERESOURCE;
		goto p_err;
	}

	msg_str_rptr = ((prev == NULL) ? rptr : prev->data + prev->length);
	__copy_message_from_line(base, sgmt_len, msg_str_rptr, next);
	if (next->magic != MESSAGE_MAGIC) {
		ret = -EINVAL;
		goto p_err;
	}

	if (next->length != sizeof(struct command)) {
		ret = -ERESOURCE;
		goto p_err;
	}

	next->self = msg_str_rptr;

p_err:
	return ret;
}

int mbx_ipc_clr_msg(char *underlay, volatile struct mailbox_ctrl *ctrl, struct message *msg)
{
	int ret = 0;
	u32 wptr, rptr, sgmt_len;
	struct message temp;
	char *base;

	if (msg == NULL) {
		ret = -EINVAL;
		goto p_err;
	}

	base = NPU_MAILBOX_GET_CTRL(underlay, ctrl->sgmt_ofs);
	sgmt_len = ctrl->sgmt_len;
	rptr = ctrl->rptr;
	wptr = ctrl->wptr;

	msg->magic = MESSAGE_MARK;
	__copy_message_to_line(base, sgmt_len, msg->self, msg);

	while (rptr < wptr) {
		__copy_message_from_line(base, sgmt_len, rptr, &temp);
		if (temp.magic != MESSAGE_MARK)
			break;

		if (temp.length != sizeof(struct command)) {
		ret = -ERESOURCE;
		goto p_err;
	}

		rptr = temp.data + temp.length;
	}

	ctrl->rptr = rptr;

p_err:
	return ret;
}

void dbg_print_msg(struct message *msg, struct command *cmd)
{
	if (!msg || !cmd)
		return;

	if (msg) {
		npu_info(
				"0x%08X %8d %8d    0x%08X 0x%08X 0x%08X 0x%08X\n",
				msg->magic, msg->mid, msg->command, msg->length,
				msg->data, cmd->payload, cmd->length);
	}

	if (cmd) {
		switch (msg->command) {
		case (COMMAND_LOAD):
			npu_info("cmd_load->oid: (%d), cmd_load->tid: (%d)\n",
					cmd->c.load.oid, cmd->c.load.tid);
			break;
		case (COMMAND_PROCESS):
			npu_info("cmd_process->oid: (%d), cmd_process->fid: (%d)\n",
				 cmd->c.process.oid, cmd->c.process.fid);
			break;
		case (COMMAND_DONE):
			npu_info("cmd_done->fid: (0x%x)\n", cmd->c.done.fid);
			break;
		case (COMMAND_NDONE):
			npu_info("cmd_ndone->fid: (0x%x), cmd_ndone->error: (0x%x)\n",
				 cmd->c.ndone.fid, cmd->c.ndone.error);
			break;
		default:
			break;
		}
	}
}

void dbg_print_ctrl(volatile struct mailbox_ctrl *mCtrl)
{
	if (mCtrl) {
		npu_info("sgmt_ofs: (%d)\n", mCtrl->sgmt_ofs);
		npu_info("sgmt_len: (%d)\n", mCtrl->sgmt_len);
		npu_info("wptr: (%d)\n", mCtrl->wptr);
		npu_info("rptr: (%d)\n", mCtrl->rptr);
	}
}

void dbg_print_hdr(volatile struct mailbox_hdr *mHdr)
{
	if (mHdr) {
		npu_info("max_slot: (%d)\n", mHdr->max_slot);
		npu_info("debug_time: (%d)\n", mHdr->debug_time);
		npu_info("debug_code: (%d)\n", mHdr->debug_code);
		npu_info("totsize: (%d)\n", mHdr->totsize);
		npu_info("version: (0x%08X)\n", mHdr->version);
		npu_info("signature2: (0x%x)\n", mHdr->signature2);
		npu_info("signature1: (0x%x)\n", mHdr->signature1);
	}
}
