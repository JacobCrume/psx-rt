#include "psx.h"

GPU gpu;
static int gp0_state;
static u32 fill_color, fill_pos;

static u32 gp1_hrange, gp1_vrange;
static int field_toggle;

void gpu_reset(void) {
	memset(&gpu, 0, sizeof(gpu));
	gp0_state = 0;
	gpu.disp_w = 320;
	gpu.disp_h = 240;
}

/* ---- GP0 ---- */

/* GP0 parameter state machine: 0=idle, 1=A0 got hdr, 2=A0 got xy,
 * 3=C0 got hdr, 4=C0 got xy, 5=fill got color, 6=fill got xy */
static u32 fill_color, fill_pos;

static void gpu_begin_transfer(u32 w1, u32 w2) {
	int x = w1 & 0x3FF;
	int y = (w1 >> 16) & 0x1FF;
	int w = (w2 & 0xFFFF);
	int h = ((w2 >> 16) & 0xFFFF);
	gpu.xfer_x = x;
	gpu.xfer_y = y;
	gpu.xfer_w = w;
	gpu.xfer_h = h;
	gpu.xfer_cx = x;
	gpu.xfer_cy = y;
	gpu.xfer_left = w * h;
	gpu.xfer_active = 1;
}

static void gpu_put_pixel(u16 px) {
	u32 off = (u32)(gpu.xfer_cy & 511) * 1024 + (gpu.xfer_cx & 1023);
	psx_vram[off] = px;
	if (++gpu.xfer_cx >= gpu.xfer_x + gpu.xfer_w) {
		gpu.xfer_cx = gpu.xfer_x;
		gpu.xfer_cy++;
	}
	if (--gpu.xfer_left <= 0)
		gpu.xfer_active = 0;
}

static void gpu_fill_rect(u32 c, u32 pos, u32 size) {
	int x = pos & 0x3FF;
	int y = (pos >> 16) & 0x1FF;
	int w = (size & 0xFFFF);
	int h = (size >> 16) & 0xFFFF;
	u16 col = (u16)(((c & 0xF8) >> 3) | ((c & 0xF800) >> 6) | ((c & 0xF80000) >> 9));
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			u32 off = (u32)((y + j) & 511) * 1024 + ((x + i) & 1023);
			psx_vram[off] = col;
		}
}

void gpu_gp0(u32 cmd) {
	/* mid-transfer: word = two pixels */
	if (gpu.xfer_active && !gpu.xfer_dir_to_cpu) {
		gpu_put_pixel((u16)(cmd & 0xFFFF));
		if (gpu.xfer_active)
			gpu_put_pixel((u16)(cmd >> 16));
		return;
	}

	switch (gp0_state) {
	case 1:
		fill_pos = cmd;
		gp0_state = 2;
		return;
	case 2:
		gpu_begin_transfer(fill_pos, cmd);
		gpu.xfer_dir_to_cpu = 0;
		gp0_state = 0;
		return;
	case 3:
		fill_pos = cmd;
		gp0_state = 4;
		return;
	case 4:
		gpu_begin_transfer(fill_pos, cmd);
		gpu.xfer_dir_to_cpu = 1;
		gp0_state = 0;
		return;
	case 5:
		fill_color = cmd;
		gp0_state = 6;
		return;
	case 6:
		gpu_fill_rect(fill_color, fill_pos, cmd);
		gp0_state = 0;
		return;
	}

	switch (cmd >> 29) {
	case 0:
		if (((cmd >> 24) & 0x1F) == 0x02)
			gp0_state = 5; /* fill rect: expect color, xy, wh */
		else
			gp0_state = 0; /* nop / flush cache */
		break;
	case 5:
		gp0_state = 1; /* CPU->VRAM */
		break;
	case 6:
		gp0_state = 3; /* VRAM->CPU */
		break;
	default:
		gp0_state = 0; /* polygons etc: not implemented yet */
		break;
	}
}

void gpu_dma_feed(u32 w) {
	if (!gpu.xfer_active) {
		gpu_gp0(w);
		return;
	}
	if (gpu.xfer_dir_to_cpu)
		return; /* payload of readback via DMA - ignore */
	gpu_put_pixel((u16)(w & 0xFFFF));
	if (gpu.xfer_active)
		gpu_put_pixel((u16)(w >> 16));
}

u32 gpu_pio_read(void) {
	/* GPUREAD: return two pixels packed */
	if (gpu.xfer_active && gpu.xfer_dir_to_cpu) {
		u16 a = psx_vram[(u32)(gpu.xfer_cy & 511) * 1024 + (gpu.xfer_cx & 1023)];
		gpu_put_pixel(a);
		u16 b = gpu.xfer_active
			? psx_vram[(u32)(gpu.xfer_cy & 511) * 1024 + (gpu.xfer_cx & 1023)]
			: 0;
		if (gpu.xfer_active)
			gpu_put_pixel(b);
		return (u32)a | ((u32)b << 16);
	}
	return 0;
}

/* ---- GP1 ---- */

void gpu_gp1(u32 cmd) {
	switch ((cmd >> 24) & 0x3F) {
	case 0x00: /* reset */
		gpu_reset();
		break;
	case 0x01: /* reset fifo */
		gp0_state = 0;
		gpu.xfer_active = 0;
		break;
	case 0x02: /* ack irq */
		break;
	case 0x03: /* display enable */
		break;
	case 0x04: /* dma direction */
		break;
	case 0x05: { /* display area start */
		gpu.disp_x = cmd & 0x3FF;
		gpu.disp_y = (cmd >> 10) & 0x1FF;
		break;
	}
	case 0x06:
		gp1_hrange = cmd;
		break;
	case 0x07:
		gp1_vrange = cmd;
		break;
	case 0x08: { /* display mode */
		static const int widths[4] = { 256, 320, 512, 640 };
		int m = cmd & 0xFF;
		int hr = (m & 3) < 4 ? (m & 3) : 0;
		gpu.disp_w = widths[hr];
		gpu.disp_h = (m & 4) ? 480 : 240;
		gpu.disp_depth24 = (m & 0x10) != 0;
		gpu.video_ntsc = !(m & 0x08); /* PAL flag bit3 */
		break;
	}
	default:
		break;
	}
}

u32 gpu_gp1_read(void) {
	u32 st = 0;
	/* ready to receive commands / DMA data (bits 26,27,28) */
	st |= (1 << 26) | (1 << 27) | (1 << 28);
	st |= field_toggle << 31;
	return st;
}

void gpu_render_frame(void) {
	field_toggle ^= 1;
}
