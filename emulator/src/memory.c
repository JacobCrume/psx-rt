#include "psx.h"

extern CPU cpu;


u16 psx_vram[VRAM_SIZE];

int g_trace = 0;

void psx_log(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static inline u32 phys(u32 addr) {
	return addr & 0x1FFFFFFF;
}

/* ---- RAM / ROM access ---- */

static inline u32 rd32_phys(u32 p) {
	if (p < RAM_SIZE)
		return *(u32 *)(psx_ram + (p & ~3));
	if (p >= 0x1FC00000 && p < 0x1FC00000 + BIOS_SIZE)
		return *(u32 *)(psx_bios + (p - 0x1FC00000));
	if (p >= 0x1F800000 && p < 0x1F800400)
		return *(u32 *)(psx_scratch + (p - 0x1F800000));
	return 0;
}

static inline void wr32_phys(u32 p, u32 v) {
	if (p < RAM_SIZE) {
		*(u32 *)(psx_ram + (p & ~3)) = v;
		return;
	}
	if (p >= 0x1F800000 && p < 0x1F800400) {
		*(u32 *)(psx_scratch + (p - 0x1F800000)) = v;
		return;
	}
}

/* ---- IO dispatch ---- */

static u32 io_read32(u32 p);
static u16 io_read16(u32 p);
static u8  io_read8(u32 p);
static void io_write32(u32 p, u32 v);
static void io_write16(u32 p, u16 v);
static void io_write8(u32 p, u8 v);

static int is_io(u32 p) {
	return p >= 0x1F801000 && p < 0x1F802000;
}

u32 mem_read32(u32 addr) {
	u32 p = phys(addr);
	if (is_io(p))
		return io_read32(p);
	return rd32_phys(p & ~3);
}

u16 mem_read16(u32 addr) {
	u32 p = phys(addr);
	if (is_io(p))
		return (u16)io_read32(p & ~1);
	if (p < RAM_SIZE || ((p >= 0x1FC00000) && (p < 0x1FC00000 + BIOS_SIZE))) {
		u32 w = rd32_phys(p & ~3);
		return (u16)((w >> ((p & 3) * 8)) & 0xFFFF);
	}
	return 0;
}

u8 mem_read8(u32 addr) {
	u32 p = phys(addr);
	if (is_io(p))
		return io_read8(p);
	if (p < RAM_SIZE)
		return psx_ram[p];
	if (p >= 0x1FC00000 && p < 0x1FC00000 + BIOS_SIZE)
		return psx_bios[p - 0x1FC00000];
	return 0;
}

void mem_write32(u32 addr, u32 v) {
	u32 p = phys(addr);
	if (is_io(p)) {
		io_write32(p & ~3, v);
		return;
	}
	wr32_phys(p & ~3, v);
}

void mem_write16(u32 addr, u16 v) {
	u32 p = phys(addr);
	if (is_io(p)) {
		io_write16(p & ~1, v);
		return;
	}
	if (p < RAM_SIZE) {
		u32 a = p & ~3;
		u32 sh = (p & 3) * 8;
		u32 w = *(u32 *)(psx_ram + a);
		w = (w & ~(0xFFFF << sh)) | ((u32)v << sh);
		*(u32 *)(psx_ram + a) = w;
	}
}

void mem_write8(u32 addr, u8 v) {
	u32 p = phys(addr);
	if (is_io(p)) {
		io_write8(p, v);
		return;
	}
	if (p < RAM_SIZE)
		psx_ram[p] = v;
}

/* ---- IO implementation ---- */

static u32 io_read32(u32 p) {
	switch (p) {
	case 0x1F801070: return irq_read_stat();
	case 0x1F801074: return irq_read_mask();
	case 0x1F801810: return gpu_pio_read();
	case 0x1F801814: return gpu_gp1_read();
	default: break;
	}
	if (p >= 0x1F801080 && p <= 0x1F8010FC)
		return dma_read32(p);
	if (p >= 0x1F801100 && p <= 0x1F80112C)
		return timers_read(p);
	if (p >= 0x1F801C00 && p <= 0x1F801E00)
		return 0x00040300; /* SPU status-ish: ready */
	if (p == 0x1F801044)
		return 0x00000005; /* JOY stat: ready */
	if (p == 0x1F801040)
		return 0xFFFF5FFF; /* no controller data */
	return 0;
}

static u16 io_read16(u32 p) {
	return (u16)io_read32(p);
}

static u8 io_read8(u32 p) {
	switch (p) {
	case 0x1F801800:
	case 0x1F801801:
	case 0x1F801802:
	case 0x1F801803:
		return cdrom_read8(p);
	case 0x1F801040:
		return 0xFF;
	case 0x1F801044:
		return 0x05;
	}
	if (p >= 0x1F801C00 && p <= 0x1F801E00)
		return 0x04;
	return (u8)io_read32(p & ~3);
}

static void io_write32(u32 p, u32 v) {
	switch (p) {
	case 0x1F801070: irq_write_stat(v); return;
	case 0x1F801074: irq_write_mask(v); return;
	case 0x1F801810: gpu_gp0(v); return;
	case 0x1F801814: gpu_gp1(v); return;
	case 0x1F80100C: /* COM_DELAY */ return;
	case 0x1F801060: /* RAM size */ return;
	default: break;
	}
	if (p >= 0x1F801080 && p <= 0x1F8010FC) {
		dma_write32(p, v);
		return;
	}
	if (p >= 0x1F801100 && p <= 0x1F80112C) {
		timers_write(p, v);
		return;
	}
	/* SPU and others: ignore */
}

static void io_write16(u32 p, u16 v) {
	io_write32(p, v);
}

static void io_write8(u32 p, u8 v) {
	switch (p) {
	case 0x1F801800:
	case 0x1F801801:
	case 0x1F801802:
	case 0x1F801803:
		cdrom_write8(p, v);
		return;
	}
}
