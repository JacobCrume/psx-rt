#include "psx.h"

void hle_pad_update(u16 buttons);

/*
 * HLE bootstrap kernel.
 *
 * Instead of running the real BIOS (whose compressed-kernel loader is a
 * project of its own), we provide the tiny subset of kernel functionality
 * that PSn00bSDK executables rely on:
 *  - calls through the RAM dispatch tables at 0xA0/0xB0/0xC0 ($t1 = fn id)
 *  - EnterCriticalSection / ExitCriticalSection (syscall, $a0 = 1/2)
 *  - the HookEntryInt / ReturnFromException context-switch pair used by
 *    the SDK's interrupt dispatcher
 */

/* context saved when an interrupt switches to the ISR */
struct SavedCtx {
	u32 r[32];
	u32 hi, lo;
	u32 sr, epc, cause;
	int bd;
};

static struct SavedCtx saved;
static u32 hook_jmpbuf = 0; /* physical addr of SDK JumpBuffer, 0 = none */
u32 pad_buf[2] = {0, 0};    /* phys addrs of pad buffers */
int pad_active = 0;

/* buttons: bit0 select, bit3 start, bit4-7 dpad, bit12-15 face buttons,
 * active low (PS1 convention) */
void hle_pad_update(u16 buttons) {
	if (!pad_active)
		return;
	for (int i = 0; i < 2; i++) {
		if (!pad_buf[i])
			continue;
		mem_write8(pad_buf[i] + 0, 0xFF);
		mem_write8(pad_buf[i] + 1, 0x41); /* digital pad ID */
		mem_write8(pad_buf[i] + 2, (u8)(buttons & 0xFF)); /* active low */
		mem_write8(pad_buf[i] + 3, (u8)((buttons >> 8) & 0xFF));
		mem_write8(pad_buf[i] + 4, 0x00);
		mem_write8(pad_buf[i] + 5, 0x00);
	}
}


extern CPU cpu;
extern u32 cur_pc;
extern int cur_in_delay;

static void hle_table_call(int table);

/* called from cpu_step when pc reaches a dispatch table address */
static void hle_table_call(int table) {
	u32 fn = cpu.r[9]; /* $t1 */

	switch (table) {
	case 0xA0:
		switch (fn) {
		case 0x39: /* InitHeap */
		case 0x44: /* FlushCache */
		case 0x70: /* _bu_init */
		case 0x71: /* _96_init */
		case 0x72: /* _96_remove */
		case 0x9C: /* SetConf */
		case 0x9D: /* GetConf */
		case 0x9F: /* SetMem */
		case 0xB0: /* GetSystemInfo */
			cpu.r[2] = 0;
			break;
		default:
			psx_log("HLE: unimplemented A(%02X)\n", fn);
			cpu.r[2] = 0;
			break;
		}
		break;
	case 0xB0:
		switch (fn) {
		case 0x17: /* ReturnFromException */
			hle_return_from_exception();
			return;
		case 0x18: /* ResetEntryInt */
			hook_jmpbuf = 0;
			break;
		case 0x19: /* HookEntryInt */
			hook_jmpbuf = cpu.r[4] & 0x1FFFFFFF;
			break;
		case 0x5B: /* ChangeClearPAD */
			break;
		case 0x08: /* OpenEvent */
			cpu.r[2] = 1; /* fake event handle */
			break;
		case 0x0C: /* EnableEvent */
		case 0x0D: /* DisableEvent */
		case 0x09: /* CloseEvent */
			cpu.r[2] = 0;
			break;
		case 0x02: /* SetRCnt */
		case 0x04: /* StartRCnt */
		case 0x05: /* StopRCnt */
		case 0x06: /* ResetRCnt */
			break;
		case 0x03: /* GetRCnt */
			cpu.r[2] = 0;
			break;
		case 0x12: /* InitPAD(buf1,len1,buf2,len2) */
			if (g_trace)
				psx_log("HLE InitPAD buf1=%08X\n", cpu.r[4]);
			pad_buf[0] = cpu.r[4] & 0x1FFFFFFF;
			pad_buf[1] = cpu.r[6] & 0x1FFFFFFF;
			pad_active = 0;
			/* pre-fill: no buttons pressed, so the game does not
			 * read a phantom "start held" before the first tick */
			for (int i = 0; i < 2; i++) {
				if (!pad_buf[i])
					continue;
				for (int j = 0; j < 8; j++)
					mem_write8(pad_buf[i] + j, 0xFF);
			}
			break;
		case 0x13: /* StartPAD */
			if (g_trace)
				psx_log("HLE StartPAD\n");
			pad_active = 1;
			break;
		case 0x14: /* StopPAD */
			pad_active = 0;
			break;
		default:
			psx_log("HLE: unimplemented B(%02X)\n", fn);
			cpu.r[2] = 0;
			break;
		}
		break;
	case 0xC0:
		switch (fn) {
		case 0x0A: /* ChangeClearRCnt */
			cpu.r[2] = 0;
			break;
		default:
			psx_log("HLE: unimplemented C(%02X)\n", fn);
			cpu.r[2] = 0;
			break;
		}
		break;
	}

	/* return to $ra */
	cpu.pc = cpu.r[31];
	cpu.next_pc = cpu.r[31] + 4;
	cpu.in_branch_delay = 0;
}

int hle_check_dispatch(u32 pc_v) {
	switch (pc_v & 0x1FFFFFFF) {
	case 0xA0:
	case 0xB0:
	case 0xC0:
		hle_table_call(pc_v & 0x1FFFFFFF);
		return 1;
	}
	return 0;
}

/* syscall with $a0 = 1/2 */
int hle_syscall(u32 a0) {
	switch (a0) {
	case 1: /* EnterCriticalSection */
		cpu.sr &= ~1u;
		cpu.r[2] = 1;
		return 1;
	case 2: /* ExitCriticalSection */
		cpu.sr |= 1;
		cpu.r[2] = 0;
		return 1;
	}
	return 0;
}

/* deliver an interrupt by switching to the SDK's ISR via its JumpBuffer */
int hle_deliver_irq(void) {
	if (!hook_jmpbuf)
		return 0;

	/* save full context */
	for (int i = 0; i < 32; i++)
		saved.r[i] = cpu.r[i];
	saved.hi = cpu.hi;
	saved.lo = cpu.lo;
	saved.sr = cpu.sr;
	saved.epc = cur_in_delay ? (cur_pc - 4) : cur_pc;
	saved.bd = cur_in_delay;
	saved.cause = (cur_in_delay ? (1u << 31) : 0);

	/* load ISR context from the SDK's JumpBuffer */
	u32 buf = hook_jmpbuf;
	cpu.r[31] = mem_read32(buf + 0x00); /* ra -> _global_isr */
	cpu.r[29] = mem_read32(buf + 0x04); /* sp  -> ISR stack */
	cpu.r[30] = mem_read32(buf + 0x08); /* s8/fp */
	cpu.r[16] = mem_read32(buf + 0x0C); /* s0 */
	cpu.r[17] = mem_read32(buf + 0x10);
	cpu.r[18] = mem_read32(buf + 0x14);
	cpu.r[19] = mem_read32(buf + 0x18);
	cpu.r[20] = mem_read32(buf + 0x1C);
	cpu.r[21] = mem_read32(buf + 0x20);
	cpu.r[22] = mem_read32(buf + 0x24);
	cpu.r[23] = mem_read32(buf + 0x28);
	cpu.r[28] = mem_read32(buf + 0x2C); /* gp */
	cpu.r[1] = 0;                       /* at */
	cpu.r[2] = cpu.r[3] = 0;            /* v0,v1 */
	cpu.r[4] = cpu.r[5] = cpu.r[6] = cpu.r[7] = 0;
	cpu.r[8] = cpu.r[9] = cpu.r[10] = cpu.r[11] = 0;
	cpu.r[12] = cpu.r[13] = cpu.r[14] = cpu.r[15] = 0;
	cpu.r[24] = cpu.r[25] = 0;
	cpu.r[26] = cpu.r[27] = 0;          /* k0,k1 */

	cpu.pc = cpu.r[31];
	cpu.next_pc = cpu.pc + 4;
	cpu.in_branch_delay = 0;
	/* run the ISR with interrupts masked, like a real exception would */
	cpu.sr = (saved.sr & ~1u) | 2u;
	return 1;
}

void hle_return_from_exception(void) {
	for (int i = 0; i < 32; i++)
		cpu.r[i] = saved.r[i];
	cpu.hi = saved.hi;
	cpu.lo = saved.lo;
	cpu.sr = saved.sr;
	cpu.cause = saved.cause;
	cpu.epc = saved.epc;

	cpu.pc = saved.epc; /* re-executes branch if BD (correct) */
	cpu.next_pc = cpu.pc + 4;
	cpu.in_branch_delay = 0;
}
