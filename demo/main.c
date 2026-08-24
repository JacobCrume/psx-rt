/*
 * psxrt - realtime ray tracer for the Sony PlayStation
 *
 * True Whitted-style ray tracing (primary rays + reflective bounce +
 * shadows) computed entirely on the PS1's R3000A CPU at 33.8688 MHz,
 * with no FPU - all math is 32-bit fixed point. The finished image is
 * DMA'd straight into VRAM, exactly what real hardware allows.
 *
 * Scene: mirror sphere + two diffuse spheres over an infinite
 * checkered plane, camera slowly orbiting the scene.
 */

#include <stdint.h>
#include <stddef.h>
#ifdef HOST_TEST
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { int16_t x, y, w, h; } RECT;
static void SetDefDispEnv(void *e, int a, int b, int c, int d) { (void)e;(void)a;(void)b;(void)c;(void)d; }
static void PutDispEnv(void *e) { (void)e; }
static int cur_fb_dummy;
static void ResetGraph(int m) { (void)m; }
static void SetDispMask(int m) { (void)m; }
static void DrawSync(int m) { (void)m; }
static void VSync(int m) { (void)m; }
static void LoadImage(RECT *r, uint32_t *d) { (void)r; (void)d; }
#else
#include <psxgpu.h>
#endif

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

#define SCREEN_XRES 320
#define SCREEN_YRES 240

/* Ray traced resolution (centered on screen). Every pixel costs real
 * CPU cycles here - this is the bottleneck on real hardware. */
#define RT_W 128
#define RT_H 96

#define RT_X ((SCREEN_XRES - RT_W) / 2)
#define RT_Y ((SCREEN_YRES - RT_H) / 2)

#define TMAX (60 << 16)   /* max ray distance: 60 world units */

/* ------------------------------------------------------------------ */
/* Fixed point math (Q16.16)                                           */
/* ------------------------------------------------------------------ */

typedef int32_t fx;
#define FX_ONE 0x00010000

#ifdef HOST_TEST
static inline fx fmul(fx a, fx b) { return (fx)(((int64_t)a * (int64_t)b) >> 16); }
#else
static inline fx fmul(fx a, fx b) { return (fx)(((int64_t)a * (int64_t)b) >> 16); }
#endif

static uint32_t isqrt32(uint32_t n) {
	uint32_t res = 0;
	uint32_t bit = 1u << 30;
	while (bit > n && bit)
		bit >>= 2;
	while (bit) {
		if (n >= res + bit) {
			n -= res + bit;
			res = (res >> 1) + bit;
		} else {
			res >>= 1;
		}
		bit >>= 2;
	}
	return res;
}

static inline fx fsqrt(fx v) {
	if (v <= 0)
		return 0;
	return (fx)(isqrt32((uint32_t)v) << 8);
}

/* sin table: angle unit = 1/1024 turn */
static fx sin_tab[1024];

static void sin_init(void) {
	for (int j = 0; j <= 256; j++) {
		fx x = (fx)(((int32_t)j * FX_ONE) / 1024); /* fraction of turn */
		fx r  = fmul(x, 411775);                   /* radians (2pi Q16.16) */
		fx r2 = fmul(r, r);
		fx s  = r;
		s -= fmul(r, fmul(r2, 10923));             /* r^3/6 */
		s += fmul(r, fmul(r2, fmul(r2, 546)));     /* r^5/120 */
		s -= fmul(r, fmul(r2, fmul(r2, fmul(r2, 13)))); /* r^7/5040 */
		sin_tab[j] = s;
	}
	sin_tab[256] = FX_ONE;
	for (int j = 1; j < 256; j++)
		sin_tab[256 + j] = sin_tab[256 - j];
	for (int j = 0; j < 512; j++)
		sin_tab[512 + j] = -sin_tab[j];
}

static inline fx fsin(int a) { return sin_tab[a & 1023]; }
static inline fx fcos(int a) { return sin_tab[(a + 256) & 1023]; }

/* ------------------------------------------------------------------ */
/* Vec3                                                                */
/* ------------------------------------------------------------------ */

typedef struct { fx x, y, z; } vec3;

static inline vec3 v3(fx x, fx y, fx z) { vec3 v = { x, y, z }; return v; }
static inline vec3 vadd(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 vscale(vec3 a, fx s) { return v3(fmul(a.x, s), fmul(a.y, s), fmul(a.z, s)); }
static inline fx vdot(vec3 a, vec3 b) { return fmul(a.x, b.x) + fmul(a.y, b.y) + fmul(a.z, b.z); }

static inline vec3 vcross(vec3 a, vec3 b) {
	/* fmul: fx components of a x b without overflowing */
	return v3(fmul(a.y, b.z) - fmul(a.z, b.y),
		  fmul(a.z, b.x) - fmul(a.x, b.z),
		  fmul(a.x, b.y) - fmul(a.y, b.x));
}

/* reciprocal via the R3000A hardware divider (32/32 -> quotient in LO).
 * Returns FX_ONE/b for b>0, accurate to ~4 LSB. */
static inline fx finv(fx b) {
	if (b <= 0)
		return 0x7FFFFFFF;
	return (fx)(((1u << 30) / (uint32_t)b) << 2);
}

static inline vec3 vnorm(vec3 a) {
	fx len = fsqrt(vdot(a, a));
	if (len == 0)
		return v3(0, 0, 0);
	fx inv = finv(len);
	return v3(fmul(a.x, inv), fmul(a.y, inv), fmul(a.z, inv));
}

/* ------------------------------------------------------------------ */
/* Scene                                                               */
/* ------------------------------------------------------------------ */

#define NUM_SPHERES 3

typedef struct {
	vec3 center;
	fx   radius;
	fx   ar, ag, ab;      /* albedo, 0..FX_ONE */
	fx   reflect;         /* reflectivity 0..FX_ONE */
} Sphere;

static Sphere spheres[NUM_SPHERES];

static const fx PLANE_Y       = 0;
static const fx AMBIENT       = 98304 / 4;          /* 0.25 */
static const fx SKY_R         = 98304 / 4;          /* 0.25 */
static const fx SKY_G         = 98304 / 2;          /* 0.50 */
static const fx SKY_B         = 98304;              /* 1.50 -> clamps */
static const fx FOG_INV_RANGE = FX_ONE / 45;        /* full fog at 45 units */

static vec3 cam_pos, cam_fwd, cam_right, cam_up;
static vec3 light_dir; /* direction toward the light source */

/* ------------------------------------------------------------------ */
/* Framebuffer + tiny font HUD                                         */
/* ------------------------------------------------------------------ */

static uint16_t fb[RT_H][RT_W];

static const uint8_t font_digits[10][8] = {
	{0x38,0x44,0x4C,0x54,0x64,0x44,0x38,0x00},
	{0x10,0x30,0x10,0x10,0x10,0x10,0x38,0x00},
	{0x38,0x44,0x04,0x08,0x10,0x20,0x7C,0x00},
	{0x7C,0x08,0x10,0x08,0x04,0x44,0x38,0x00},
	{0x08,0x18,0x28,0x48,0x7C,0x08,0x08,0x00},
	{0x7C,0x40,0x78,0x04,0x04,0x44,0x38,0x00},
	{0x38,0x40,0x78,0x44,0x44,0x44,0x38,0x00},
	{0x7C,0x04,0x08,0x10,0x20,0x20,0x20,0x00},
	{0x38,0x44,0x44,0x38,0x44,0x44,0x38,0x00},
	{0x38,0x44,0x44,0x3C,0x04,0x44,0x38,0x00},
};

static void draw_int(int x, int y, uint32_t val, int digits) {
	for (int i = digits - 1; i >= 0; i--) {
		const uint8_t *g = font_digits[val % 10];
		val /= 10;
		for (int row = 0; row < 8; row++) {
			uint8_t bits = g[row];
			if (!bits)
				continue;
			uint16_t *line = &fb[y + row][x + i * 8];
			for (int c = 0; c < 8; c++)
				if (bits & (0x80 >> c))
					line[c] = 0xFFFF;
		}
	}
}

static inline uint16_t pack555(fx r, fx g, fx b) {
	if (r > 65535) r = 65535;
	if (g > 65535) g = 65535;
	if (b > 65535) b = 65535;
	/* PS1 VRAM is BGR555: R in bits 0-4, G in 5-9, B in 10-14 */
	return (uint16_t)((r >> 11) | ((g >> 6) & 0x03E0) | ((b >> 1) & 0x7C00));
}

/* ------------------------------------------------------------------ */
/* Ray tracing                                                         */
/* ------------------------------------------------------------------ */

static inline int trace_spheres(vec3 ro, vec3 rd, fx tmax, fx *t_out, vec3 *n_out) {
	int hit = -1;
	fx t_best = tmax;
	for (int i = 0; i < NUM_SPHERES; i++) {
		vec3 oc = vsub(ro, spheres[i].center);
		fx b    = vdot(oc, rd);
		fx c    = vdot(oc, oc) - fmul(spheres[i].radius, spheres[i].radius);
		fx disc = fmul(b, b) - c;
		if (disc < 0)
			continue;
		fx sq = fsqrt(disc);
		fx t  = -b - sq;
		if (t < 0)
			t = -b + sq;
		if (t < 0 || t >= t_best)
			continue;
		t_best = t;
		hit    = i;
	}
	if (hit >= 0) {
		vec3 p = vadd(ro, vscale(rd, t_best));
		*n_out = vnorm(vsub(p, spheres[hit].center));
		*t_out = t_best;
	}
	return hit;
}

/* Returns 1 and sets rgb if plane hit, else 0 (sky/sphere beyond tmax). */
static inline int shade_plane(vec3 ro, vec3 rd, fx tmax, fx *pr, fx *pg, fx *pb) {
	if (rd.y >= 0)
		return 0;
	/* t = (PLANE_Y - ro.y) / rd.y, all fixed point */
	fx t = fmul(PLANE_Y - ro.y, finv(-rd.y));
	if (t < 0 || t >= tmax)
		return 0;

	vec3 p = vadd(ro, vscale(rd, t));
	int ix = p.x >> 16;
	int iz = p.z >> 16;
	int checker = (ix ^ iz) & 1;

	fx base = checker ? 49152 : 12288;   /* light/dark gray */
	fx gcol = checker ? 49152 : 14336;
	fx bcol = checker ? 49152 : 16384;

	/* distance fog toward sky color at horizon */
	fx fog = fmul(t, FOG_INV_RANGE);
	if (fog > FX_ONE)
		fog = FX_ONE;
	fx keep = FX_ONE - fog;

	*pr = fmul(base, keep) + fmul(SKY_R, fog);
	*pg = fmul(gcol, keep) + fmul(SKY_G, fog);
	*pb = fmul(bcol, keep) + fmul(SKY_B, fog);
	return 1;
}

static inline vec3 sky_color(vec3 rd) {
	fx h = rd.y;
	if (h < 0)
		h = 0;
	fx q = h >> 1;
	return v3(SKY_R - q, SKY_G - (h >> 2), SKY_B);
}

static uint16_t trace_pixel(vec3 ro, vec3 rd) {
	fx   t;
	vec3 n;
	int  hit = trace_spheres(ro, rd, TMAX, &t, &n);

	fx cr, cg, cb;

	if (hit >= 0) {
		Sphere *s = &spheres[hit];
		vec3 p = vadd(ro, vscale(rd, t));

		/* shadow ray toward light */
		fx ndl = vdot(n, light_dir);
		if (ndl < 0)
			ndl = 0;

		fx t_sh;
		vec3 n_sh;
		int blocked = ndl > 0 &&
			trace_spheres(vadd(p, vscale(n, 4096)), light_dir, TMAX, &t_sh, &n_sh) >= 0;

		fx lit;
		if (blocked)
			lit = AMBIENT;
		else
			lit = AMBIENT + fmul(ndl, FX_ONE - AMBIENT);

		cr = fmul(s->ar, lit);
		cg = fmul(s->ag, lit);
		cb = fmul(s->ab, lit);

		if (s->reflect > 0) {
			/* one reflective bounce */
			fx   dn  = vdot(rd, n) * 2;
			vec3 rfl = vsub(rd, vscale(n, dn));
			vec3 org = vadd(p, vscale(rfl, 1024)); /* epsilon offset */

			fx   t2;
			vec3 n2;
			int  h2 = trace_spheres(org, rfl, TMAX, &t2, &n2);
			p = org;
			fx rr, rg, rb;
			if (h2 >= 0) {
				Sphere *s2 = &spheres[h2];
				fx l2 = AMBIENT + fmul(vdot(n2, light_dir) < 0 ? 0 : vdot(n2, light_dir),
						       FX_ONE - AMBIENT);
				rr = fmul(s2->ar, l2);
				rg = fmul(s2->ag, l2);
				rb = fmul(s2->ab, l2);
			} else if (!shade_plane(p, rfl, TMAX, &rr, &rg, &rb)) {
				vec3 sk = sky_color(rfl);
				rr = sk.x;
				rg = sk.y;
				rb = sk.z;
			}

			cr = fmul(cr, FX_ONE - s->reflect) + fmul(rr, s->reflect);
			cg = fmul(cg, FX_ONE - s->reflect) + fmul(rg, s->reflect);
			cb = fmul(cb, FX_ONE - s->reflect) + fmul(rb, s->reflect);
		}
	} else if (!shade_plane(ro, rd, TMAX, &cr, &cg, &cb)) {
		vec3 sk = sky_color(rd);
		cr = sk.x;
		cg = sk.y;
		cb = sk.z;
	}

	return pack555(cr, cg, cb);
}

static void render_frame(void) {
	/* focal: tan(half vertical fov) ~ 0.42 */
	const fx focal = 27525;

	/* incremental screen coordinates */
	const fx du = (fx)(((int64_t)focal * 2) / RT_W);
	const fx dv = (fx)(((int64_t)focal * 2) / RT_H);
	fx v = -(fx)(((int64_t)dv * (RT_H - 1)) >> 1);

	for (int py = 0; py < RT_H; py++, v += dv) {
		fx u = -(fx)(((int64_t)du * (RT_W - 1)) >> 1);
		for (int px = 0; px < RT_W; px++, u += du) {
			vec3 rd = vnorm(v3(
				cam_fwd.x + fmul(cam_right.x, u) + fmul(cam_up.x, v),
				cam_fwd.y + fmul(cam_right.y, u) + fmul(cam_up.y, v),
				cam_fwd.z + fmul(cam_right.z, u) + fmul(cam_up.z, v)));
			fb[py][px] = trace_pixel(cam_pos, rd);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Setup + main loop                                                   */
/* ------------------------------------------------------------------ */

#ifdef HOST_TEST
static int cur_fb = 0;
#else
static DISPENV disp_envs[2];
static int cur_fb = 0;
#endif

#ifdef HOST_TEST
int main(void) {
	sin_init();
		light_dir = vnorm(v3(24576, 40960, 16384)); /* up and to the front */
	spheres[0].ar = 57344; spheres[0].ag = 58982; spheres[0].ab = 61440;
	spheres[0].reflect = 49152;
	spheres[1].ar = 57344; spheres[1].ag = 12288; spheres[1].ab = 8192;
	spheres[1].reflect = 0;
	spheres[2].ar = 8192;  spheres[2].ag = 20480; spheres[2].ab = 57344;
	spheres[2].reflect = 0;
	int ang = 0;
	cam_pos = v3(fmul(fsin(ang), 294912), 144179, fmul(fcos(ang), 294912));
	vec3 target = v3(0, 65536, 0);
	cam_fwd   = vnorm(vsub(target, cam_pos));
	vec3 w_up = v3(0, FX_ONE, 0);
	cam_right = vnorm(vcross(cam_fwd, w_up));
	cam_up    = vnorm(vcross(cam_right, cam_fwd));
	spheres[0].center = v3(0, 65536, 0);
	spheres[0].radius = 65536;
	spheres[1].center = v3(0, 42000, 170000);
	spheres[1].radius = 33000;
	spheres[2].center = v3(-210000, 33000, 0);
	spheres[2].radius = 23000;
	render_frame();
	FILE *f = fopen("/home/jacob/tmp/dumps/host_frame.ppm", "wb");
	fprintf(f, "P6\n%d %d\n255\n", RT_W, RT_H);
	for (int y = 0; y < RT_H; y++)
		for (int x = 0; x < RT_W; x++) {
			uint16_t c = fb[y][x];
			uint8_t rgb[3] = { (uint8_t)((c & 31) << 3),
					   (uint8_t)(((c >> 5) & 31) << 3),
					   (uint8_t)(((c >> 10) & 31) << 3) };
			fwrite(rgb, 1, 3, f);
		}
	fclose(f);
	f = fopen("/home/jacob/tmp/dumps/host_dbg.bin", "wb");
	fwrite((const void *)dbg, 1, sizeof(dbg), f);
	fclose(f);
	f = fopen("/home/jacob/tmp/dumps/host_fb.bin", "wb");
	fwrite(fb, 1, sizeof(fb), f);
	fclose(f);
	printf("done\n");
	return 0;
}
#else
int main(int argc, const char **argv) {
	(void)argc;
	(void)argv;

	ResetGraph(0);
	sin_init();

	SetDefDispEnv(&disp_envs[0], 0, 0, SCREEN_XRES, SCREEN_YRES);
	SetDefDispEnv(&disp_envs[1], 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
	SetDispMask(1);

		light_dir = vnorm(v3(24576, 40960, 16384)); /* up and to the front */ /* toward light */

	/* scene setup */
	spheres[0].ar = 57344; spheres[0].ag = 58982; spheres[0].ab = 61440;
	spheres[0].reflect = 49152;                    /* strong mirror */
	spheres[1].ar = 57344; spheres[1].ag = 12288; spheres[1].ab = 8192;
	spheres[1].reflect = 0;
	spheres[2].ar = 8192;  spheres[2].ag = 20480; spheres[2].ab = 57344;
	spheres[2].reflect = 0;

	uint32_t frame_counter = 0;

	for (;;) {
		int ang = frame_counter * 2;

		/* orbiting camera, radius 4.5, height 2.2, looking near origin */
		cam_pos = v3(fmul(fsin(ang), 294912), 144179, fmul(fcos(ang), 294912));
		vec3 target = v3(0, 65536, 0);
		cam_fwd   = vnorm(vsub(target, cam_pos));
		vec3 w_up = v3(0, FX_ONE, 0);
		cam_right = vnorm(vcross(cam_fwd, w_up));
		cam_up    = vnorm(vcross(cam_right, cam_fwd));

		/* animate spheres (world units in fx) */
		spheres[0].center = v3(0, 65536 + fmul(fsin(frame_counter * 3), 8192), 0);
		spheres[0].radius = 65536;
		spheres[1].center = v3(fmul(fsin(ang), 170000), 42000, fmul(fcos(ang), 170000));
		spheres[1].radius = 33000;
		spheres[2].center = v3(fmul(fsin(ang + 512), 210000), 33000, fmul(fcos(ang + 512), 210000));
		spheres[2].radius = 23000;

		render_frame();
		draw_int(2, 2, frame_counter, 6);

		DrawSync(0);
		VSync(0);

		RECT rect;
		setRECT(&rect, RT_X, disp_envs[cur_fb].disp.y + RT_Y, RT_W, RT_H);
		LoadImage(&rect, (uint32_t *)fb);
		DrawSync(0);
		PutDispEnv(&disp_envs[cur_fb]);
		cur_fb ^= 1;

		frame_counter++;
	}

	return 0;
}
#endif /* HOST_TEST */
