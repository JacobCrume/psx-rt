#include <stdint.h>

#define OUT ((volatile u32 *)0x80013000)
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int8_t s8;
typedef uint8_t u8;

static u32 results[128];
static int idx;

static void chk(u32 label, u32 got, u32 exp) {
	results[idx++] = label;
	results[idx++] = got;
	results[idx++] = exp;
}

/* function called via jalr to test jr $ra path */
static u32 (*volatile fp)(u32) = 0;

static u32 add10(u32 x) { return x + 10; }

int main(void) {
	fp = &add10;

	/* --- arithmetic edge cases --- */
	volatile s32 imin = (s32)0x80000000;
	chk(20, (u32)(imin / -1), (u32)0x80000000);          /* INT_MIN/-1 */
	chk(21, (u32)(imin % 7), (u32)(-2147483648 % 7));
	chk(22, (u32)((u32)imin / 3u), 0x2AAAAAAAu);
	volatile s32 a = -7, b = 3;
	chk(23, (u32)(a % b), (u32)(-7 % 3));                /* -1 */
	chk(24, (u32)(a / b), (u32)(-7 / 3));                /* -2 */

	/* --- variable shifts incl. by 0 and 31 --- */
	volatile u32 sv = 31;
	u32 x = 0x80000001u;
	chk(25, x >> sv, 1);
	chk(26, x << sv, 0x80000000u);
	volatile s32 sn = 0x80000000;
	chk(27, (u32)(sn >> sv), 0xFFFFFFFFu);
	volatile u32 s0 = 0;
	chk(28, x >> s0, x);
	chk(29, (u32)(sn >> s0), (u32)sn);

	/* --- slti/sltiu/xori/andi/nori via C --- */
	chk(30, (u32)((s32)0x80000000 < -5), 1);
	chk(31, (u32)(0xFFFF0000u > 5u), 1);
	chk(32, (0xF0F0 ^ 0x0FF0), 0xFF00u);
	chk(33, (0xF0F0 | 0x0F0F), 0xFFFFu);
	chk(34, (0xF0F0 & 0x0FF0), 0x00F0u);

	/* --- lui/ori pairs --- */
	chk(35, (0x12340000u | 0x5678u), 0x12345678u);

	/* --- lh/lb sign extension --- */
	static volatile unsigned char bytes[8] = {0x80, 0x7F, 0xFF, 0x01, 0x00, 0x81, 0xFE, 0x7E};
	volatile s16 *ph = (s16 *)(bytes + 2); /* 0xFF,0x01 -> 0x01FF = 511 */
	chk(36, (u32)(s32)*ph, 511);
	volatile s8 *pb = (s8 *)(bytes + 5); /* 0x81 = -127 */
	chk(37, (u32)(s32)*pb, (u32)(s32)-127);
	volatile u8 *pub = (u8 *)(bytes + 2);
	chk(38, (u32)*pub, 255);

	/* --- lwl/lwr all offsets vs aligned word --- */
	static volatile unsigned char src[8] = {0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE};
	for (u32 off = 0; off < 4; off++) {
		u32 v;
		__asm__ volatile("lwr %0, 0(%1)\n\t"
				 "lwl %0, 3(%1)"
				 : "=&r"(v) : "r"(src + off) : "memory");
		/* unaligned word starting at src+off (LE) */
		u32 exp = 0;
		for (u32 i = 0; i < 4; i++)
			exp |= (u32)src[off + i] << (8 * i);
		chk(40 + off, v, exp);
	}

	/* --- swl/swr all offsets --- */
	for (u32 off = 0; off < 4; off++) {
		u32 dst[2] = {0x11111111u, 0x22222222u};
		u32 val = 0xDCCBAABBu; /* bytes LE: BB AA BB CC? -> bytes: BB AA BB DC */
		__asm__ volatile("swr %1, 0(%2)\n\t"
				 "swl %1, 3(%2)"
				 : "+r"(val) : "r"(val), "r"((char *)dst + off) : "memory");
		/* store 32-bit LE word at dst+off */
		u32 e0 = 0x11111111u, e1 = 0x22222222u;
		u8 *e = (u8 *)&e0;
		u8 *vb = (u8 *)&val;
		for (u32 i = 0; i < 4; i++) {
			u32 addr = off + i;
			if (addr < 4)
				e[addr] = vb[i];
			else
				((u8 *)&e1)[addr - 4] = vb[i];
		}
		chk(50 + off * 2, e0, dst[0]);
		chk(51 + off * 2, e1, dst[1]);
	}

	/* --- function pointer call (jalr) --- */
	chk(60, fp(37), 47);

	/* --- bltzal/bgezal via C (function prologue uses them rarely) --- */
	volatile s32 neg = -5, pos = 5;
	u32 cnt = 0;
	if (neg < 0) cnt++;
	if (pos >= 0) cnt++;
	chk(61, cnt, 2);

	/* --- 64/32 division patterns used by fixed point --- */
	volatile s64 big = 0x123456789LL;
	chk(62, (u32)(big / 1000), (u32)(0x123456789LL / 1000));
	chk(63, (u32)(big % 1000), (u32)(0x123456789LL % 1000));

	/* --- nor via ~(|) --- */
	chk(64, ~(0xF0F0u | 0x0F0Fu), 0xFFFF0000u);

	results[idx++] = 0xDEADBEEFu;

	u32 *o = (u32 *)0x80013000;
	for (int i = 0; i < idx; i++)
		o[i] = results[i];
	o[idx] = 0xDEADBEEFu;

	for (;;)
		;
	return 0;
}
