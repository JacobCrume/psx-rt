#include "psx.h"

CPU cpu;

/* COP0 registers */
enum { COP0_INX = 0, COP0_RAND, COP0_ENTRYLO0, COP0_BPC, COP0_ENTRYHI,
       COP0_SR = 12, COP0_CAUSE = 13, COP0_EPC = 14, COP0_PRID = 15 };

static const char *exc_name[] = {
	"Int", "Mod", "TLBL", "TLBS", "AdEL", "AdES", "IBErr", "DBErr",
	"Syscall", "Break", "RI", "CpU", "Ovf" };

void cpu_reset(u32 entry_pc) {
	memset(&cpu, 0, sizeof(cpu));
	cpu.pc = entry_pc;
	cpu.next_pc = entry_pc + 4;
	cpu.sr = 0x10600000; /* BEV=1, kernel mode */
}

u32 cur_pc;
int cur_in_delay;

static void take_exception(int code) {
	u32 vector;
	int in_delay = cur_in_delay;
	u32 epc = in_delay ? (cur_pc - 4) : cur_pc;

	cpu.cause = (cpu.cause & ~0x7C) | ((u32)code << 2);
	if (in_delay)
		cpu.cause |= 1 << 31;
	else
		cpu.cause &= ~(1u << 31);
	cpu.epc = epc;

	/* shift interrupt/mode stack */
	u32 sr = cpu.sr;
	sr = (sr & ~0x3F) | ((sr & 0x0F) << 2); /* IEc/KUc -> old */
	sr |= 2;                                /* EXL */
	cpu.sr = sr;

	vector = (sr & (1 << 22)) ? 0xBFC00180 : 0x80000080;
	if (code == 0 || code == 8 || code == 9)
		vector += 0x00; /* common vector */
	cpu.in_branch_delay = 0;
	cpu.pc = vector;
	cpu.next_pc = vector + 4;
	if (g_trace)
		psx_log("EXC %s EPC=%08X\n", exc_name[code], epc);
}

void cpu_exception(int code) {
	take_exception(code);
}

/* sign helpers */
static inline s32 sx16(u16 v) { return (s16)v; }
static inline s32 sx8(u8 v)   { return (s8)v; }

#define R(cpu_, i) (cpu_.r[i])

static void do_branch(u32 target) {
	cpu.next_pc = target;
	cpu.in_branch_delay = 1;
}

/* returns cycles consumed */
int cpu_step(void) {
	cur_pc = cpu.pc;
	cur_in_delay = cpu.in_branch_delay;

	/* HLE dispatch tables at 0xA0/0xB0/0xC0 */
	if (hle_check_dispatch(cpu.pc))
		return 4;

	/* pending IRQ? */
	if (((cpu.sr & 1)) && !(cpu.sr & 2) && irq_pending()) {
		if (hle_deliver_irq())
			return 8;
		take_exception(0);
		return 4;
	}

	u32 pc_v = cpu.pc;


	u32 instr = mem_read32(pc_v);
	if (g_trace && pc_v >= 0x80010160 && pc_v <= 0x800101F0)
		psx_log("T %08X: %08X v0=%08X v1=%08X a0=%08X\n", pc_v, instr,
			cpu.r[2], cpu.r[3], cpu.r[4]);

	cpu.pc = cpu.next_pc;
	cpu.next_pc += 4;
	cpu.in_branch_delay = 0;

	if (instr == 0) /* NOP */
		return 1;

	u32 op = instr >> 26;
	u32 rs = (instr >> 21) & 31;
	u32 rt = (instr >> 16) & 31;
	u32 rd = (instr >> 11) & 31;
	u32 sa = (instr >> 6) & 31;
	u32 imm = instr & 0xFFFF;
	u32 simm = (u32)sx16((u16)imm);
	u32 target = ((instr & 0x3FFFFFF) << 2) | ((pc_v + 4) & 0xF0000000);

	switch (op) {
	case 0x00: /* SPECIAL */
		switch (instr & 0x3F) {
		case 0x00: if (rd) R(cpu, rd) = R(cpu, rt) << sa; break;
		case 0x02: if (rd) R(cpu, rd) = R(cpu, rt) >> sa; break;
		case 0x03:
			if (rd) R(cpu, rd) = (u32)((s32)R(cpu, rt) >> sa);
			break;
		case 0x04: if (rd) R(cpu, rd) = R(cpu, rt) << (R(cpu, rs) & 31); break;
		case 0x06: if (rd) R(cpu, rd) = R(cpu, rt) >> (R(cpu, rs) & 31); break;
		case 0x07:
			if (rd) R(cpu, rd) = (u32)((s32)R(cpu, rt) >> (R(cpu, rs) & 31));
			break;
		case 0x08: /* JR */
			do_branch(R(cpu, rs));
			break;
		case 0x09: /* JALR */
			do_branch(R(cpu, rs));
			if (rd)
				R(cpu, rd) = pc_v + 8;
			break;
		case 0x0C: /* SYSCALL */
			if (hle_syscall(R(cpu, 4)))
				return 4;
			take_exception(8);
			return 4;
		case 0x0D: /* BREAK */
			take_exception(9);
			return 4;
		case 0x10: if (rd) R(cpu, rd) = cpu.hi; break; /* MFHI */
		case 0x11: cpu.hi = R(cpu, rs); break;         /* MTHI */
		case 0x12: if (rd) R(cpu, rd) = cpu.lo; break; /* MFLO */
		case 0x13: cpu.lo = R(cpu, rs); break;         /* MTLO */
		case 0x18: { /* MULT */
			s64 r = (s64)(s32)R(cpu, rs) * (s32)R(cpu, rt);
			cpu.lo = (u32)r;
			cpu.hi = (u32)((u64)r >> 32);
			break;
		}
		case 0x19: { /* MULTU */
			u64 r = (u64)R(cpu, rs) * R(cpu, rt);
			cpu.lo = (u32)r;
			cpu.hi = (u32)(r >> 32);
			break;
		}
		case 0x1A: { /* DIV */
			s32 a = (s32)R(cpu, rs), b = (s32)R(cpu, rt);
			if (b == 0) {
				cpu.lo = a < 0 ? 1 : 0xFFFFFFFF;
				cpu.hi = b ? a : a;
				if (!b)
					cpu.hi = a;
			} else {
				cpu.lo = (u32)(a / b);
				cpu.hi = (u32)(a % b);
			}
			break;
		}
		case 0x1B: { /* DIVU */
			u32 a = R(cpu, rs), b = R(cpu, rt);
			if (b != 0) {
				cpu.lo = a / b;
				cpu.hi = a % b;
			} else {
				cpu.lo = 0xFFFFFFFF;
				cpu.hi = a;
			}
			break;
		}
		case 0x20: { /* ADD with overflow trap */
			s32 a = (s32)R(cpu, rs), b = (s32)R(cpu, rt);
			s32 r_ = a + b;
			if (!((a ^ r_) & (b ^ r_) & 0x80000000)) {
				if (rd) R(cpu, rd) = (u32)r_;
			} else
				take_exception(12);
			break;
		}
		case 0x21: if (rd) R(cpu, rd) = R(cpu, rs) + R(cpu, rt); break;
		case 0x22: { /* SUB ovf */
			s32 a = (s32)R(cpu, rs), b = (s32)R(cpu, rt);
			s32 r_ = a - b;
			if (!((a ^ b) & (a ^ r_) & 0x80000000)) {
				if (rd) R(cpu, rd) = (u32)r_;
			} else
				take_exception(12);
			break;
		}
		case 0x23: if (rd) R(cpu, rd) = R(cpu, rs) - R(cpu, rt); break;
		case 0x24: if (rd) R(cpu, rd) = R(cpu, rs) & R(cpu, rt); break;
		case 0x25: if (rd) R(cpu, rd) = R(cpu, rs) | R(cpu, rt); break;
		case 0x26: if (rd) R(cpu, rd) = R(cpu, rs) ^ R(cpu, rt); break;
		case 0x27: if (rd) R(cpu, rd) = ~(R(cpu, rs) | R(cpu, rt)); break;
		case 0x2A: if (rd) R(cpu, rd) = ((s32)R(cpu, rs) < (s32)R(cpu, rt)); break;
		case 0x2B: if (rd) R(cpu, rd) = (R(cpu, rs) < R(cpu, rt)); break;
		default:
			if (g_trace)
				psx_log("unimpl SPECIAL %02x @%08X\n", (instr & 0x3F), pc_v);
			break;
		}
		break;

	case 0x01: /* REGIMM */
		switch (rt) {
		case 0x00: if ((s32)R(cpu, rs) < 0) do_branch(pc_v + 4 + (simm << 2)); break;
		case 0x01: if ((s32)R(cpu, rs) >= 0) do_branch(pc_v + 4 + (simm << 2)); break;
		case 0x10: { int t = (s32)R(cpu, rs) < 0; R(cpu, 31) = pc_v + 8; if (t) do_branch(pc_v + 4 + (simm << 2)); break; }
		case 0x11: { int t = (s32)R(cpu, rs) >= 0; R(cpu, 31) = pc_v + 8; if (t) do_branch(pc_v + 4 + (simm << 2)); break; }
		default: break;
		}
		break;
	case 0x02: /* J */
		do_branch(target);
		break;
	case 0x03: /* JAL */
		R(cpu, 31) = pc_v + 8;
		do_branch(target);
		break;
	case 0x04: if (R(cpu, rs) == R(cpu, rt)) do_branch(pc_v + 4 + (simm << 2)); break;
	case 0x05: if (R(cpu, rs) != R(cpu, rt)) do_branch(pc_v + 4 + (simm << 2)); break;
	case 0x06: if ((s32)R(cpu, rs) <= 0) do_branch(pc_v + 4 + (simm << 2)); break;
	case 0x07: if ((s32)R(cpu, rs) > 0) do_branch(pc_v + 4 + (simm << 2)); break;
	case 0x08: { /* ADDI ovf */
		s32 a = (s32)R(cpu, rs), b = (s32)simm;
		s32 r_ = a + b;
		if (!((a ^ r_) & (b ^ r_) & 0x80000000))
			R(cpu, rt) = (u32)r_;
		else
			take_exception(12);
		break;
	}
	case 0x09: R(cpu, rt) = R(cpu, rs) + simm; break;
	case 0x0A: R(cpu, rt) = ((s32)R(cpu, rs) < (s32)simm); break;
	case 0x0B: R(cpu, rt) = (R(cpu, rs) < simm); break;
	case 0x0C: R(cpu, rt) = R(cpu, rs) & imm; break;
	case 0x0D: R(cpu, rt) = R(cpu, rs) | imm; break;
	case 0x0E: R(cpu, rt) = R(cpu, rs) ^ imm; break;
	case 0x0F: R(cpu, rt) = imm << 16; break;

	case 0x10: /* COP0 */
		switch (rs) {
		case 0x00: /* MFC0 */
			switch (rd) {
			case COP0_SR:    R(cpu, rt) = cpu.sr; break;
			case COP0_CAUSE: R(cpu, rt) = cpu.cause; break;
			case COP0_EPC:   R(cpu, rt) = cpu.epc; break;
			case COP0_PRID:  R(cpu, rt) = 2; break;
			default:         R(cpu, rt) = 0; break;
			}
			break;
		case 0x04: /* MTC0 */
			switch (rd) {
			case COP0_SR:    cpu.sr = R(cpu, rt); break;
			case COP0_CAUSE: cpu.cause &= ~0x300; break;
			default: break;
			}
			break;
		case 0x10: /* RFE */
			cpu.sr = (cpu.sr & ~0x0F) |
				 ((cpu.sr >> 2) & 0x0F);
			break;
		default:
			break;
		}
		break;
	case 0x12: case 0x13: case 0x32: case 0x3A: /* GTE/LWC2/SWC2 stub */
		break;

	case 0x20: R(cpu, rt) = (u32)sx8(mem_read8(R(cpu, rs) + simm)); break;
	case 0x21: { u32 a = R(cpu, rs) + simm;
		     if (a & 1) { take_exception(4); break; }
		     R(cpu, rt) = (u32)(s16)mem_read16(a); break; }
	case 0x23: { u32 a = R(cpu, rs) + simm;
		     if (a & 3) { take_exception(4); break; }
		     R(cpu, rt) = mem_read32(a); break; }
	case 0x24: R(cpu, rt) = mem_read8(R(cpu, rs) + simm); break;
	case 0x25: { u32 a = R(cpu, rs) + simm;
		     if (a & 1) { take_exception(4); break; }
		     R(cpu, rt) = mem_read16(a); break; }

	case 0x28: mem_write8(R(cpu, rs) + simm, (u8)R(cpu, rt)); break;
	case 0x29: { u32 a = R(cpu, rs) + simm;
		     if (a & 1) {
			     if (g_trace)
				     psx_log("AdES addr=%08X pc=%08X ra=%08X\n",
					     a, pc_v, R(cpu, 31));
			     take_exception(5);
			     break;
		     }
		     mem_write16(a, (u16)R(cpu, rt)); break; }
	case 0x2B: { u32 a = R(cpu, rs) + simm;
		     if (a & 3) { take_exception(5); break; }
		     mem_write32(a, R(cpu, rt)); break; }

	case 0x22: { /* LWL (little-endian) */
		u32 addr = R(cpu, rs) + simm;
		u32 w = mem_read32(addr & ~3);
		u32 b = addr & 3;
		u32 sh = 8 * (3 - b);
		u32 keep = (b == 3) ? 0 : (0xFFFFFFFFu >> (8 * (b + 1)));
		R(cpu, rt) = (R(cpu, rt) & keep) | ((w << sh) & ~keep);
		break;
	}
	case 0x26: { /* LWR (little-endian) */
		u32 addr = R(cpu, rs) + simm;
		u32 w = mem_read32(addr & ~3);
		u32 b = addr & 3;
		u32 sh = 8 * b;
		u32 mask = 0xFFFFFFFFu >> (8 * b);
		R(cpu, rt) = (R(cpu, rt) & ~mask) | ((w >> sh) & mask);
		break;
	}
	case 0x2A: { /* SWL (little-endian) */
		u32 addr = R(cpu, rs) + simm;
		u32 w = mem_read32(addr & ~3);
		u32 b = addr & 3;
		u32 sh = 8 * (3 - b);
		u32 keep = (b == 3) ? 0 : (0xFFFFFFFFu << (8 * (b + 1)));
		w = (w & keep) | ((R(cpu, rt) >> sh) & ~keep);
		mem_write32(addr & ~3, w);
		break;
	}
	case 0x2E: { /* SWR (little-endian) */
		u32 addr = R(cpu, rs) + simm;
		u32 w = mem_read32(addr & ~3);
		u32 b = addr & 3;
		u32 sh = 8 * b;
		u32 keep = (b == 0) ? 0 : (0xFFFFFFFFu >> (8 * (4 - b)));
		w = (w & keep) | ((R(cpu, rt) << sh) & ~keep);
		mem_write32(addr & ~3, w);
		break;
	}

	default:
		if (g_trace)
			psx_log("unimpl op %02X @%08X\n", op, pc_v);
		break;
	}

	return 1;
}
