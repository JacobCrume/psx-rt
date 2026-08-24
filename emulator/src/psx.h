#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uint64_t u64;
typedef int64_t  s64;

#define RAM_SIZE  (2 * 1024 * 1024)
#define BIOS_SIZE (512 * 1024)
#define VRAM_SIZE (1024 * 512) /* in halfwords */

extern u8 *psx_ram;
extern u8 *psx_scratch;
extern u8 *psx_bios;
extern u16 psx_vram[VRAM_SIZE];

/* logging */
void psx_log(const char *fmt, ...);
extern int g_trace;

struct PSX;

/* memory.c */
u32 mem_read32(u32 addr);
u16 mem_read16(u32 addr);
u8  mem_read8(u32 addr);
void mem_write32(u32 addr, u32 v);
void mem_write16(u32 addr, u16 v);
void mem_write8(u32 addr, u8 v);

/* gpu.c */
typedef struct {
	/* display */
	u16 disp_x, disp_y, disp_w, disp_h;
	int disp_depth24;
	int video_ntsc;
	/* transfer state machine (GP0 A0/C0) */
	int xfer_active;
	int xfer_dir_to_cpu;
	int xfer_x, xfer_y, xfer_w, xfer_h;
	int xfer_cx, xfer_cy, xfer_left;
	u32 fifo_word[16];
	int fifo_len;
} GPU;

extern GPU gpu;

void gpu_reset(void);
void gpu_gp0(u32 cmd);
u32  gpu_gp1_read(void);
void gpu_gp1(u32 cmd);
u32  gpu_pio_read(void);      /* GPUREAD via port */
void gpu_dma_feed(u32 w);     /* word from DMA into GP0 */
void gpu_render_frame(void);  /* end of frame housekeeping */

/* dma.c */
enum { DMA_MDEC_IN = 0, DMA_MDEC_OUT, DMA_GPU, DMA_CDROM, DMA_SPU, DMA_PIO, DMA_OTC };

void dma_write32(u32 addr, u32 v);
u32  dma_read32(u32 addr);
int  dma_update(void); /* run pending transfers; returns cycles consumed */

/* timers.c */
void timers_reset(void);
void timers_tick(int cycles);
void timers_hblank(void);
u32  timers_read(u32 addr);
void timers_write(u32 addr, u32 v);

/* irq */
void irq_assert(int bit);
u32  irq_read_stat(void);
void irq_write_stat(u32 v);
u32  irq_read_mask(void);
void irq_write_mask(u32 v);
int  irq_pending(void);

/* cdrom.c */
void cdrom_write8(u32 addr, u8 v);
u8   cdrom_read8(u32 addr);
void cdrom_tick(int cycles);

/* cpu.c */
typedef struct {
	u32 r[32];
	u32 pc, next_pc, current_pc_in_delay;
	u32 hi, lo;
	u32 sr, cause, epc;
	int in_branch_delay;
	u64 pending_cycles;
} CPU;

extern CPU cpu;
void cpu_reset(u32 entry_pc);
int  cpu_step(void);          /* execute one instruction; returns cycles */
void cpu_exception(int code); /* raise exception */

/* hle.c */
int  hle_check_dispatch(u32 pc_v);
int  hle_syscall(u32 a0);
int  hle_deliver_irq(void);
void hle_return_from_exception(void);
void hle_pad_update(u16 buttons);

/* main helpers */
void load_exe(const char *path);
void load_bios(const char *path);
void try_inject_exe(void);
