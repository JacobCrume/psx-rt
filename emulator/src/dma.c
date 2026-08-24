#include "psx.h"
extern CPU cpu;
extern u32 cur_pc;

/* DMA registers */
static u32 dma_madr[7], dma_bcr[7], dma_chcr[7];
static u32 dpcr, dicr;

/* IRQ controller */
static u16 i_stat, i_mask;

void irq_assert(int bit) { i_stat |= (u16)(1 << bit); }
u32 irq_read_stat(void)  { return i_stat; }
void irq_write_stat(u32 v) {
	/* I_STAT acknowledge is inverted: writing 0 clears the flag */
	i_stat &= (u16)v;
}
u32 irq_read_mask(void)  { return i_mask; }
void irq_write_mask(u32 v) { i_mask = (u16)v; }
int irq_pending(void)    { return (i_stat & i_mask) != 0; }

extern void gpu_gp0(u32 cmd);

static void dma_irq(int ch) {
	dicr |= (1 << (24 + ch));
	if (dicr & (1 << (16 + ch)))
		irq_assert(3); /* DMA irq */
}

int dma_update(void) {
	int busy = 0;
	for (int ch = 0; ch < 7; ch++) {
		if (!(dma_chcr[ch] & 1))
			continue;
		busy = 1;
		u32 madr = dma_madr[ch] & 0x1FFFFC;
		u32 bcr  = dma_bcr[ch];
		u32 sync = (dma_chcr[ch] >> 9) & 3;
		int to_ram = (dma_chcr[ch] >> 12) & 1;

		switch (ch) {
		case DMA_GPU: {
			if (g_trace)
				psx_log("DMA GPU run: chcr=%08X bcr=%08X madr=%08X\n",
					dma_chcr[ch], bcr, madr);
			if (!to_ram) {
				/* CPU->GPU: sync mode 2 = linked list, else block of words */
				if (sync == 2) {
					u32 addr = madr;
					for (;;) {
						u32 hdr = mem_read32(addr);
						int n = hdr >> 24;
						addr = (addr + 4) & 0x1FFFFF;
						for (int i = 0; i < n; i++) {
							gpu_dma_feed(mem_read32(addr));
							addr = (addr + 4) & 0x1FFFFF;
						}
						if (hdr & 0x800000)
							break;
						addr = hdr & 0x1FFFFF;
					}
				} else {
					u32 count = ((bcr >> 16) ? (bcr >> 16) : 0x100000) * (bcr & 0xFFFF);
					for (u32 i = 0; i < count; i++)
						gpu_dma_feed(mem_read32((madr + i * 4) & 0x1FFFFF));
				}
			}
			break;
		}
		case DMA_OTC: {
			/* clear ordering table: build backwards list */
			u32 count = bcr & 0xFFFF;
			u32 addr = madr;
			for (u32 i = 0; i < count; i++) {
				u32 next = (i == count - 1) ? 0xFFFFFF : ((addr - 4) & 0x1FFFFF);
				mem_write32(addr, next);
				addr = (addr - 4) & 0x1FFFFF;
			}
			break;
		}
		case DMA_MDEC_IN:
		case DMA_MDEC_OUT:
			break; /* stub: drop data */
		case DMA_CDROM:
		case DMA_SPU:
		case DMA_PIO:
			if (to_ram) {
				/* fill with zeros (no real source implemented) */
				u32 words = (bcr >> 16) * (bcr & 0xFFFF);
				for (u32 i = 0; i < words; i++)
					mem_write32((madr + i * 4) & 0x1FFFFF, 0);
			}
			break;
		}

		dma_chcr[ch] &= ~0x01000001u; /* transfer done: clear start + TR */
		dma_irq(ch);
	}
	return busy;
}

static int valid_reg(u32 addr) {
	return 1;
}

u32 dma_read32(u32 addr) {
	u32 off = addr & 0xFF;
	if (off == 0xF0) return dpcr;
	if (off == 0xF4) return dicr;
	if (off == 0xF8) return 0x07654321;
	int ch = (off >> 4) & 7;
	switch (off & 0xF) {
	case 0x0: return dma_madr[ch];
	case 0x4: return dma_bcr[ch];
	case 0x8: return dma_chcr[ch];
	default:  return 0;
	}
	valid_reg(addr);
}

void dma_write32(u32 addr, u32 v) {
	u32 off = addr & 0xFF;
	if (off == 0xF0) { dpcr = v; return; }
	if (off == 0xF4) {
		/* bits 0-23+31: enable bits (direct write). bits 24-30: flags,
		 * write 1 to CLEAR. */
		u32 flags = dicr & ~(v & 0x7F000000);
		dicr = (v & 0x80FFFFFF) | flags;
		return;
	}
	if (off == 0xF8) return;
	int ch = (off >> 4) & 7;
	switch (off & 0xF) {
	case 0x0:
		dma_madr[ch] = v;
		break;
	case 0x4:
		dma_bcr[ch] = v;
		break;
	case 0x8:
		dma_chcr[ch] = v;
		if (v & 1)
			dma_update();
		break;
	default: break;
	}
}
