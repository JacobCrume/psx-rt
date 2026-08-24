#include "psx.h"

/* Minimal CDROM controller: responds to BIOS probes with error/no-disc
 * responses so the boot process continues to the shell. */

static u8 cmd_fifo[16];
static int cmd_len;
static u8 resp_fifo[16];
static int resp_len, resp_pos;
static int pending_int; /* 3 or 5 */
static int int_delay;
static u8 cd_stat;

extern void irq_assert(int bit);

static void push_resp(u8 b) {
	if (resp_len < 16)
		resp_fifo[resp_len++] = b;
}

static void queue_int(int n) {
	pending_int = n;
	int_delay = 20000;
}

static void exec_cmd(u8 cmd) {
	resp_len = resp_pos = 0;
	switch (cmd) {
	case 0x01: /* GetStat */
		cd_stat = 0x02; /* head pos? door closed */
		push_resp(cd_stat);
		queue_int(3);
		break;
	case 0x02: /* ReadN */
	case 0x06: /* ReadTOC */
	case 0x1A: /* GetID: no disc */
		push_resp(0x80); /* error */
		push_resp(0x00);
		push_resp(0x00);
		push_resp(0x00);
		queue_int(5);
		break;
	case 0x0E: /* SetMode */
		push_resp(cd_stat);
		queue_int(3);
		break;
	case 0x15: /* SeekL */
	case 0x16:
	case 0x0A:
	case 0x0C:
	case 0x09:
	case 0x08:
	case 0x07:
	case 0x1B:
		push_resp(cd_stat);
		queue_int(3);
		push_resp(cd_stat);
		queue_int(2);
		break;
	default:
		push_resp(0x40 | cd_stat); /* bad command */
		push_resp(0x10);
		push_resp(0x00);
		push_resp(0x00);
		queue_int(5);
		break;
	}
}

void cdrom_write8(u32 addr, u8 v) {
	switch (addr & 3) {
	case 0:
		if (cmd_len < 16)
			cmd_fifo[cmd_len++] = v;
		break;
	case 1:
		/* issue command */
		if (cmd_len) {
			exec_cmd(cmd_fifo[0]);
			cmd_len = 0;
		}
		break;
	case 2:
		/* int mask */
		break;
	case 3:
		if (v & 0xE0) { /* ack int */
			pending_int = 0;
			resp_len = resp_pos = 0;
		}
		break;
	}
}

u8 cdrom_read8(u32 addr) {
	switch (addr & 3) {
	case 0:
		return 0xE0; /* status: fifo empty-ish, ready */
	case 1: {
		if (resp_pos < resp_len)
			return resp_fifo[resp_pos++];
		return 0;
	}
	case 2:
		return 0xE0;
	case 3: {
		u8 v = pending_int ? pending_int : 0;
		if (pending_int) {
			irq_assert(2);
			pending_int = -pending_int; /* keep asserted until acked */
			pending_int = 0;
			/* keep flag readable once */
		}
		return v;
	}
	}
	return 0;
}

void cdrom_tick(int cycles) {
	if (int_delay > 0) {
		int_delay -= cycles;
		if (int_delay <= 0 && pending_int > 0)
			irq_assert(2);
	}
}
