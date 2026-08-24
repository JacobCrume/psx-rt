#include "psx.h"
#include <SDL2/SDL.h>

u8 *psx_ram;
u8 *psx_scratch;
u8 *psx_bios;

static const int CYCLES_PER_FRAME = 564480; /* ~33.87MHz / 60Hz */
static const int LINES_PER_FRAME = 263;

/* ---- PS-EXE loading ---- */

static char exe_path[1024];
static int exe_pending = 0;
static u64 idle_counter = 0;

void load_bios(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		psx_log("cannot open BIOS %s\n", path);
		exit(1);
	}
	fread(psx_bios, 1, BIOS_SIZE, f);
	fclose(f);
}

void load_exe(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		psx_log("cannot open exe %s\n", path);
		return;
	}
	u8 hdr[2048];
	fread(hdr, 1, 2048, f);
	if (memcmp(hdr, "PS-X EXE", 8) != 0) {
		psx_log("not a PS-X EXE\n");
		fclose(f);
		return;
	}
	u32 pc0    = *(u32 *)(hdr + 0x10);
	u32 gp0    = *(u32 *)(hdr + 0x14);
	u32 t_addr = *(u32 *)(hdr + 0x18);
	u32 t_size = *(u32 *)(hdr + 0x1C);

	u32 pa = t_addr & 0x1FFFFFFF;
	if (pa + t_size > RAM_SIZE) {
		psx_log("exe too big for RAM\n");
		fclose(f);
		return;
	}
	fseek(f, 0x800, SEEK_SET);
	fread(psx_ram + pa, 1, t_size, f);
	fclose(f);

	memset(&cpu.r, 0, sizeof(cpu.r));
	cpu.r[28] = gp0;
	cpu.r[29] = 0x801FFF00;
	cpu.r[30] = 0x801FFF00;
	cpu.pc = pc0 < 0x80000000 ? (pc0 | 0x80000000) : pc0;
	cpu.next_pc = cpu.pc + 4;
	cpu.sr = 0x10001; /* kernel mode, interrupts enabled */
	cpu.in_branch_delay = 0;

	exe_pending = 0;
	psx_log("loaded exe: pc=%08X t_addr=%08X t_size=%X\n", cpu.pc, t_addr, t_size);
}

void try_inject_exe(void) {
	if (exe_pending && exe_path[0])
		load_exe(exe_path);
}

/* ---- main ---- */

int main(int argc, char **argv) {
	const char *bios_path = NULL;
	const char *dump_prefix = NULL;
	int headless = 0, max_frames = 0, verbose_trace = 0;
	long inject_after_frames = -1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--bios") && i + 1 < argc)
			bios_path = argv[++i];
		else if (!strcmp(argv[i], "--exe") && i + 1 < argc) {
			strncpy(exe_path, argv[++i], sizeof(exe_path) - 1);
			exe_pending = 1;
		} else if (!strcmp(argv[i], "--inject-after") && i + 1 < argc)
			inject_after_frames = atol(argv[++i]);
		else if (!strcmp(argv[i], "--headless"))
			headless = 1;
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			max_frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
			dump_prefix = argv[++i];
		else if (!strcmp(argv[i], "--trace"))
			verbose_trace = 1;
		else if (!strcmp(argv[i], "--input") && i + 1 < argc)
			i++; /* consumed by the script parser below */
		else {
			psx_log("usage: %s --bios FILE [--exe FILE] [--inject-after N]\n"
				"          [--headless --frames N --dump PREFIX] [--trace]\n",
				argv[0]);
			return 1;
		}
	}
	g_trace = verbose_trace;

	psx_ram = malloc(RAM_SIZE);
	psx_scratch = malloc(0x400);
	psx_bios = malloc(BIOS_SIZE);
	memset(psx_ram, 0, RAM_SIZE);
	memset(psx_scratch, 0, 0x400);
	memset(psx_bios, 0xFF, BIOS_SIZE);
	memset(psx_vram, 0, sizeof(psx_vram));

	gpu_reset();
	timers_reset();

	if (bios_path) {
		load_bios(bios_path);
		cpu_reset(0xBFC00000);
	} else if (exe_pending) {
		/* HLE boot: no BIOS, dispatch tables handled by hle.c */
		cpu_reset(0x80010000);
		try_inject_exe();
	} else {
		psx_log("need --bios and/or --exe\n");
		return 1;
	}

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
	SDL_Window *win = NULL;
	SDL_Renderer *ren = NULL;
	SDL_Texture *tex = NULL;
	if (!headless) {
		win = SDL_CreateWindow("oxpsx", SDL_WINDOWPOS_CENTERED,
				       SDL_WINDOWPOS_CENTERED, 960, 720, 0);
		ren = SDL_CreateRenderer(win, -1,
					 SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
					SDL_TEXTUREACCESS_STREAMING, 1024, 512);
	}

	u32 fb[VRAM_SIZE];
	int frame = 0;
	int running = 1;
	SDL_Event ev;
	u16 pad_buttons = 0xFFFF; /* active low */
	/* scripted input for headless testing: "frame:hexbuttons,frame:hex..." */
	struct { int frame; u16 btn; } script[64];
	int script_n = 0;
	{
		const char *script_arg = NULL;
		for (int i = 1; i < argc; i++)
			if (!strcmp(argv[i], "--input") && i + 1 < argc)
				script_arg = argv[++i];
		if (script_arg) {
			char buf[512];
			strncpy(buf, script_arg, sizeof(buf) - 1);
			char *tok = strtok(buf, ",");
			while (tok && script_n < 64) {
				int f;
				unsigned b;
				if (sscanf(tok, "%d:%x", &f, &b) == 2) {
					script[script_n].frame = f;
					script[script_n].btn = (u16)b;
					script_n++;
				}
				tok = strtok(NULL, ",");
			}
		}
	}
	const u8 *keys = headless ? NULL : SDL_GetKeyboardState(NULL);

	while (running) {
		/* run one frame worth of CPU */
		int cycles_left = CYCLES_PER_FRAME;
		int line_cycles = CYCLES_PER_FRAME / LINES_PER_FRAME;
		int lines = LINES_PER_FRAME;
		static u64 total_steps = 0;
		while (cycles_left > 0 && lines > 0) {
			int chunk = line_cycles;
			while (chunk > 0) {
				cpu_step();
				chunk -= 1;
				if (g_trace && ++total_steps % 2000000 == 0)
					psx_log("tick: pc=%08X\n", cpu.pc);
			}
			cycles_left -= line_cycles;
			lines--;
			timers_tick(line_cycles);
			timers_hblank();
			cdrom_tick(line_cycles);
		}
		dma_update();

		/* end of frame: vblank */
		gpu_render_frame();
		irq_assert(0); /* VBlank */

		/* controller input */
		{
			u16 btn = 0xFFFF;
			if (!headless && keys) {
				if (keys[SDL_SCANCODE_UP])    btn &= ~0x0010;
				if (keys[SDL_SCANCODE_RIGHT]) btn &= ~0x0020;
				if (keys[SDL_SCANCODE_DOWN])  btn &= ~0x0040;
				if (keys[SDL_SCANCODE_LEFT])  btn &= ~0x0080;
				if (keys[SDL_SCANCODE_RETURN]) btn &= ~0x0008; /* start */
				if (keys[SDL_SCANCODE_Z]) btn &= ~0x4000; /* cross */
				if (keys[SDL_SCANCODE_X]) btn &= ~0x2000; /* circle */
			}
			/* script entries HOLD from their frame until the next */
			for (int i = script_n - 1; i >= 0; i--) {
				if (frame >= script[i].frame) {
					btn = script[i].btn;
					break;
				}
			}
			pad_buttons = btn;
			hle_pad_update(pad_buttons);
		}

		/* exe injection heuristics (real BIOS path only) */
		if (bios_path && inject_after_frames >= 0 && frame == inject_after_frames)
			try_inject_exe();
		/* also auto-inject when the BIOS idles in the shell main loop:
		 * heuristic - PC stuck near shell region for a long time */
		if (exe_pending && bios_path && inject_after_frames < 0) {
			if (cpu.pc >= 0x80040000 && cpu.pc < 0x801F0000)
				idle_counter++;
			else
				idle_counter = 0;
			if (idle_counter > 200000000)
				try_inject_exe();
		}

		/* render */
		if (!headless) {
			for (int y = 0; y < 512; y++)
				for (int x = 0; x < 1024; x++) {
					u16 c = psx_vram[y * 1024 + x];
					u8 r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
					fb[y * 1024 + x] =
					    0xFF000000u | (b5 << 3) << 16 |
					    ((g5 << 3) << 8) | (r5 << 3);
				}
			SDL_UpdateTexture(tex, NULL, fb, 1024 * 4);
			SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
			SDL_RenderClear(ren);
			SDL_Rect src = { gpu.disp_x, gpu.disp_y,
					 gpu.disp_w, gpu.disp_h };
			SDL_RenderCopy(ren, tex, &src, NULL);
			SDL_RenderPresent(ren);

			while (SDL_PollEvent(&ev)) {
				if (ev.type == SDL_QUIT)
					running = 0;
				if (ev.type == SDL_KEYDOWN &&
				    ev.key.keysym.sym == SDLK_ESCAPE)
					running = 0;
				if (ev.type == SDL_KEYDOWN &&
				    ev.key.keysym.sym == SDLK_l)
					try_inject_exe();
			}
		}

		if (dump_prefix && frame >= 30) {
			{
				static int ram_dumped = 0;
				if (!ram_dumped) {
					u32 *o = (u32 *)(psx_ram + 0x13000);
					if (o[0] != 0) {
						ram_dumped = 1;
						FILE *f = fopen("/home/jacob/tmp/dumps/cpuresults.bin", "wb");
						fwrite(o, 1, 0x400, f);
						fclose(f);
						psx_log("cpu results dumped\n");
					}
				}
			}
			char path[512];
			snprintf(path, sizeof(path), "%s_%04d.ppm", dump_prefix, frame);
			FILE *f = fopen(path, "wb");
			if (f) {
				fprintf(f, "P6\n%d %d\n255\n", gpu.disp_w, gpu.disp_h);
				for (int y = 0; y < gpu.disp_h; y++)
					for (int x = 0; x < gpu.disp_w; x++) {
						u16 c = psx_vram[(gpu.disp_y + y) * 1024 +
								 gpu.disp_x + x];
						u8 r5 = c & 31, g5 = (c >> 5) & 31,
						   b5 = (c >> 10) & 31;
						u8 rgb[3] = { (u8)(r5 << 3),
							      (u8)(g5 << 3),
							      (u8)(b5 << 3) };
						fwrite(rgb, 1, 3, f);
					}
				fclose(f);
			}
			/* also dump raw VRAM once for debugging */
			snprintf(path, sizeof(path), "%s_vram.ppm", dump_prefix);
			if (frame == max_frames - 1) {
				f = fopen(path, "wb");
				if (f) {
					fprintf(f, "P6\n1024 512\n255\n");
					for (int y = 0; y < 512; y++)
						for (int x = 0; x < 1024; x++) {
							u16 c = psx_vram[y * 1024 + x];
							u8 rgb[3] = { (u8)((c & 31) << 3),
								      (u8)(((c >> 5) & 31) << 3),
								      (u8)(((c >> 10) & 31) << 3) };
							fwrite(rgb, 1, 3, f);
						}
					fclose(f);
				}
			}
		}

		frame++;
		if (frame % 60 == 0)
			psx_log("frame %d pc=%08X sr=%08X istat=%04X imask=%04X\n",
				frame, cpu.pc, cpu.sr,
				irq_read_stat(), irq_read_mask());
		if (max_frames && frame >= max_frames)
			running = 0;
	}

	SDL_Quit();
	return 0;
}
