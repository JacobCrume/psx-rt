#include "psx.h"

/* 3 root counters */
typedef struct {
	u16 count, target;
	u32 mode;
	int prescale_div;  /* cycles per tick */
	int irq_bit;
} Timer;

static Timer timers[3];

void timers_reset(void) {
	memset(timers, 0, sizeof(timers));
}

/* mode bits (nocash):
 * 0     sync enable
 * 1-2   sync mode
 * 3     reset counter to 0 at target
 * 4     irq at target
 * 5     irq at 0xFFFF
 * 6-7   irq repeat/pulse
 * 8     clock source: ch0: dotclock, ch1/2: hblank/sysclk div8
 */
static void timer_recalc(Timer *t, int idx) {
	if (idx == 0)
		t->prescale_div = (t->mode & 0x100) ? 1 : 5; /* sysclk vs dotclk(21477k/33868k~) approx */
	else if (idx == 1)
		t->prescale_div = (t->mode & 0x100) ? 39 : 1; /* ~sysclk/1566? hblank handled specially */
	else
		t->prescale_div = (t->mode & 0x100) ? 8 : 1;
	t->irq_bit = 4 + idx;
}

static void timer_tick_one(Timer *t, int idx, int cycles) {
	if (!(t->mode & 1))
		return; /* sync disabled = free run for our purposes */
	int acc = cycles / t->prescale_div;
	while (acc-- > 0) {
		t->count++;
		if (t->count == t->target && (t->mode & 0x10)) {
			irq_assert(t->irq_bit);
			if (t->mode & 0x08)
				t->count = 0;
		}
		if (t->count == 0xFFFF) {
			if (t->mode & 0x20)
				irq_assert(t->irq_bit);
			if (t->mode & 0x08)
				t->count = 0;
			else
				t->count = 0; /* wraps */
		}
	}
}

void timers_tick(int cycles) {
	for (int i = 0; i < 3; i++)
		timer_tick_one(&timers[i], i, cycles);
}

/* called once per video scanline by main loop */
void timers_hblank(void) {
	Timer *t = &timers[1];
	if ((t->mode & 0x101) == 0x101 || !(t->mode & 1)) {
		/* in hblank sync mode count hblanks directly */
		if (!(t->mode & 1))
			return;
		t->count++;
		if (t->count == t->target && (t->mode & 0x10)) {
			irq_assert(5);
			if (t->mode & 0x08)
				t->count = 0;
		}
		if (t->count == 0xFFFF) {
			if (t->mode & 0x20)
				irq_assert(5);
			t->count = 0;
		}
	}
}

u32 timers_read(u32 addr) {
	int ch = (addr >> 4) & 3;
	switch (addr & 0xF) {
	case 0x0: return timers[ch].count;
	case 0x4: return timers[ch].mode | 0x400 /* target reached latch */;
	case 0x8: return timers[ch].target;
	default:  return 0;
	}
}

void timers_write(u32 addr, u32 v) {
	int ch = (addr >> 4) & 3;
	switch (addr & 0xF) {
	case 0x0: timers[ch].count = (u16)v; break;
	case 0x4:
		timers[ch].mode = v & 0x13FF;
		timer_recalc(&timers[ch], ch);
		timers[ch].count = 0;
		break;
	case 0x8: timers[ch].target = (u16)v; break;
	default: break;
	}
}
