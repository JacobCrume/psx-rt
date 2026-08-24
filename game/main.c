/*
 * REFLECTOR - a complete ray-traced 3D game for the Sony PlayStation
 *
 * Everything you see is ray traced per pixel on the R3000A CPU at
 * 33.8688 MHz, no FPU, all fixed point Q16.16:
 *   - primary rays per pixel (3 obstacle spheres + player sphere + plane)
 *   - shadow rays toward the sun for every hit
 *   - one reflective bounce on the player's mirror sphere
 *   - distance fog, checkered ground
 *
 * You are the mirror sphere. Steer with LEFT/RIGHT, dodge the incoming
 * spheres. START to begin / restart.
 */

#include <stdint.h>
#include <stddef.h>
#include <psxgpu.h>
#include <psxapi.h>

/* ------------------------------------------------------------------ */
/* Screen / RT config                                                  */
/* ------------------------------------------------------------------ */

#define SCREEN_XRES 320
#define SCREEN_YRES 240

#define RT_W 112
#define RT_H 84

#define RT_X ((SCREEN_XRES - RT_W) / 2)
#define RT_Y ((SCREEN_YRES - RT_H) / 2)

#define TMAX (70 << 16)

/* ------------------------------------------------------------------ */
/* Fixed point Q16.16                                                  */
/* ------------------------------------------------------------------ */

typedef int32_t fx;
#define FX_ONE 0x00010000

static inline fx fmul(fx a, fx b) {
	int32_t lo, hi;
	__asm__ volatile(
		"mult %2, %3\n\t"
		"mflo %0\n\t"
		"mfhi %1"
		: "=r"(lo), "=r"(hi)
		: "r"(a), "r"(b));
	return (int32_t)(((uint32_t)lo >> 16) | ((uint32_t)hi << 16));
}

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

static fx sin_tab[1024];

static void sin_init(void) {
	for (int j = 0; j <= 256; j++) {
		fx x = (fx)(((int32_t)j * FX_ONE) / 1024);
		fx r  = fmul(x, 411775);
		fx r2 = fmul(r, r);
		fx s  = r;
		s -= fmul(r, fmul(r2, 10923));
		s += fmul(r, fmul(r2, fmul(r2, 546)));
		s -= fmul(r, fmul(r2, fmul(r2, fmul(r2, 13))));
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

static inline fx finv(fx b) {
	if (b <= 0)
		return 0x7FFFFFFF;
	return (fx)(((1u << 30) / (uint32_t)b) << 2);
}

/* ------------------------------------------------------------------ */
/* Vec3                                                                */
/* ------------------------------------------------------------------ */

typedef uint16_t u16;
typedef uint8_t u8;
typedef struct { fx x, y, z; } vec3;

static inline vec3 v3(fx x, fx y, fx z) { vec3 v = { x, y, z }; return v; }
static inline vec3 vadd(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 vsub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 vscale(vec3 a, fx s) { return v3(fmul(a.x, s), fmul(a.y, s), fmul(a.z, s)); }
static inline fx vdot(vec3 a, vec3 b) { return fmul(a.x, b.x) + fmul(a.y, b.y) + fmul(a.z, b.z); }
static inline vec3 vcross(vec3 a, vec3 b) {
	return v3(fmul(a.y, b.z) - fmul(a.z, b.y),
		  fmul(a.z, b.x) - fmul(a.x, b.z),
		  fmul(a.x, b.y) - fmul(a.y, b.x));
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

#define NUM_SPHERES 4   /* 0 = player (mirror), 1..3 obstacles */
#define NUM_OBST    3

typedef struct {
	vec3 center;
	fx   radius;
	fx   ar, ag, ab;
	fx   reflect;
} Sphere;

static Sphere spheres[NUM_SPHERES];

static const fx PLANE_Y       = 0;
static const fx AMBIENT       = 8192;             /* 0.125 */
static const fx SKY_R         = 24576;            /* 0.375 */
static const fx SKY_G         = 49152;            /* 0.75  */
static const fx SKY_B         = 98304;            /* 1.5 (clamps) */
static const fx FOG_INV_RANGE = FX_ONE / 55;

static vec3 cam_pos, cam_fwd, cam_right, cam_up;
static vec3 light_dir;

/* ------------------------------------------------------------------ */
/* Framebuffer + font                                                  */
/* ------------------------------------------------------------------ */

static uint16_t fb[RT_H][RT_W];

/* 5x7 glyphs in 8-byte rows (MSB = leftmost column) */
static const uint8_t font[][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
	{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E,0x00}, /* 0 */
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0x00}, /* 1 */
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,0x00}, /* 2 */
	{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E,0x00}, /* 3 */
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02,0x00}, /* 4 */
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,0x00}, /* 5 */
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E,0x00}, /* 6 */
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0x00}, /* 7 */
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0x00}, /* 8 */
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,0x00}, /* 9 */
	{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00,0x00}, /* : */
	{0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00}, /* - */
	{0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00}, /* ! */
	{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* A */
	{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0x00}, /* B */
	{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0x00}, /* C */
	{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E,0x00}, /* D */
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0x00}, /* E */
	{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0x00}, /* F */
	{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0x00}, /* G */
	{0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* H */
	{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, /* I */
	{0x07,0x02,0x02,0x02,0x02,0x12,0x0C,0x00}, /* J */
	{0x11,0x12,0x14,0x18,0x14,0x12,0x11,0x00}, /* K */
	{0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0x00}, /* L */
	{0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0x00}, /* M */
	{0x11,0x19,0x15,0x13,0x11,0x11,0x11,0x00}, /* N */
	{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* O */
	{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0x00}, /* P */
	{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D,0x00}, /* Q */
	{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0x00}, /* R */
	{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E,0x00}, /* S */
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, /* T */
	{0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* U */
	{0x11,0x11,0x11,0x11,0x11,0x0A,0x04,0x00}, /* V */
	{0x11,0x11,0x11,0x15,0x15,0x1B,0x11,0x00}, /* W */
	{0x11,0x0A,0x04,0x04,0x04,0x0A,0x11,0x00}, /* X */
	{0x11,0x11,0x0A,0x04,0x04,0x04,0x04,0x00}, /* Y */
	{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,0x00}, /* Z */
};

static int font_idx(char c) {
	if (c >= '0' && c <= '9')
		return 1 + (c - '0');
	if (c == ':')
		return 11;
	if (c == '-')
		return 12;
	if (c == '!')
		return 13;
	if (c >= 'A' && c <= 'Z')
		return 14 + (c - 'A');
	return 0;
}

static void draw_text(int x, int y, const char *s, uint16_t col) {
	while (*s) {
		const uint8_t *g = font[font_idx(*s)];
		for (int row = 0; row < 7; row++) {
			uint8_t bits = g[row];
			if (!bits)
				continue;
			if (y + row < 0 || y + row >= RT_H)
				continue;
			uint16_t *line = &fb[y + row][x];
			for (int c = 0; c < 5; c++)
				if (bits & (0x10 >> c)) {
					if (x + c >= 0 && x + c < RT_W)
						line[c] = col;
				}
		}
		x += 6;
		s++;
	}
}

static void draw_int(int x, int y, int32_t val, int digits) {
	char buf[12];
	int neg = val < 0;
	if (neg)
		val = -val;
	buf[digits] = 0;
	for (int i = digits - 1; i >= 0; i--) {
		buf[i] = '0' + (val % 10);
		val /= 10;
	}
	if (neg)
		buf[0] = '-';
	draw_text(x, y, buf, 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* Ray tracing                                                         */
/* ------------------------------------------------------------------ */

static inline int trace_spheres(vec3 ro, vec3 rd, fx tmax, fx *t_out, vec3 *n_out,
				int skip) {
	int hit = -1;
	fx t_best = tmax;
	for (int i = 0; i < NUM_SPHERES; i++) {
		if (i == skip)
			continue;
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

static inline vec3 sky_color(vec3 rd) {
	fx h = rd.y;
	if (h < 0)
		h = 0;
	fx q = h >> 1;
	return v3(SKY_R - q, SKY_G - (h >> 2), SKY_B);
}

/* returns 1 if plane hit */
static inline int shade_plane(vec3 ro, vec3 rd, fx tmax, fx *pr, fx *pg, fx *pb) {
	if (rd.y >= 0)
		return 0;
	fx t = fmul(PLANE_Y - ro.y, finv(-rd.y));
	if (t < 0 || t >= tmax)
		return 0;

	vec3 p = vadd(ro, vscale(rd, t));
	int ix = p.x >> 16;
	int iz = p.z >> 16;
	int checker = (ix ^ iz) & 1;

	/* sun shadow ray */
	vec3 n = v3(0, FX_ONE, 0);
	vec3 org = vadd(p, vscale(n, 4096));
	fx   ts;
	vec3 ns;
	int blocked = trace_spheres(org, light_dir, TMAX, &ts, &ns, -1) >= 0;

	fx base = checker ? 45000 : 11500;
	fx gcol = checker ? 45000 : 13500;
	fx bcol = checker ? 45000 : 15000;

	fx lit = blocked ? AMBIENT : (AMBIENT + fmul(-light_dir.y, FX_ONE - AMBIENT));

	fx fog = fmul(t, FOG_INV_RANGE);
	if (fog > FX_ONE)
		fog = FX_ONE;
	fx keep = FX_ONE - fog;

	*pr = fmul(fmul(base, lit), keep) + fmul(SKY_R, fog);
	*pg = fmul(fmul(gcol, lit), keep) + fmul(SKY_G, fog);
	*pb = fmul(fmul(bcol, lit), keep) + fmul(SKY_B, fog);
	return 1;
}

static uint16_t trace_pixel(vec3 ro, vec3 rd) {
	fx   t;
	vec3 n;
	int  hit = trace_spheres(ro, rd, TMAX, &t, &n, -1);

	fx cr, cg, cb;

	if (hit >= 0) {
		Sphere *s = &spheres[hit];
		vec3 p = vadd(ro, vscale(rd, t));

		fx ndl = vdot(n, light_dir);
		if (ndl < 0)
			ndl = 0;

		vec3 sorg = vadd(p, vscale(n, 4096));
		fx   t_sh;
		vec3 n_sh;
		int blocked = trace_spheres(sorg, light_dir, TMAX, &t_sh, &n_sh, hit) >= 0;

		fx lit;
		if (blocked)
			lit = AMBIENT;
		else
			lit = AMBIENT + fmul(ndl, FX_ONE - AMBIENT);

		cr = fmul(s->ar, lit);
		cg = fmul(s->ag, lit);
		cb = fmul(s->ab, lit);

		if (s->reflect > 0) {
			/* mirror: one reflective bounce */
			fx   dn  = vdot(rd, n) * 2;
			vec3 rfl = vsub(rd, vscale(n, dn));
			vec3 org = vadd(p, vscale(rfl, 1024));

			fx   t2;
			vec3 n2;
			fx rr, rg, rb;
			int h2 = trace_spheres(org, rfl, TMAX, &t2, &n2, hit);
			if (h2 >= 0) {
				Sphere *s2 = &spheres[h2];
				fx ndl2 = vdot(n2, light_dir);
				if (ndl2 < 0)
					ndl2 = 0;
				vec3 o2 = vadd(vadd(org, vscale(rfl, t2)),
					       vscale(n2, 4096));
				fx   t3;
				vec3 n3;
				int blk = trace_spheres(o2, light_dir, TMAX, &t3, &n3, h2) >= 0;
				fx l2 = blk ? AMBIENT
					    : (AMBIENT + fmul(ndl2, FX_ONE - AMBIENT));
				rr = fmul(s2->ar, l2);
				rg = fmul(s2->ag, l2);
				rb = fmul(s2->ab, l2);
			} else if (!shade_plane(org, rfl, TMAX, &rr, &rg, &rb)) {
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

	/* distance fog for sphere hits */
	if (hit >= 0) {
		fx fog = fmul(t, FOG_INV_RANGE);
		if (fog > FX_ONE)
			fog = FX_ONE;
		fx keep = FX_ONE - fog;
		cr = fmul(cr, keep) + fmul(SKY_R, fog);
		cg = fmul(cg, keep) + fmul(SKY_G, fog);
		cb = fmul(cb, keep) + fmul(SKY_B, fog);
	}

	if (cr > 65535) cr = 65535;
	if (cg > 65535) cg = 65535;
	if (cb > 65535) cb = 65535;
	return (uint16_t)((cr >> 11) | ((cg >> 6) & 0x03E0) | ((cb >> 1) & 0x7C00));
}

/* ------------------------------------------------------------------ */
/* Game state                                                          */
/* ------------------------------------------------------------------ */

#define GS_TITLE    0
#define GS_PLAY     1
#define GS_DEAD     2

static int game_state = GS_TITLE;
static int32_t score_frames = 0;
static int32_t best_score = 0;
static int death_timer = 0;

static fx player_x = 0;
static fx player_vx = 0;

/* obstacle i (1..3): x, z in spheres[i].center */
static fx world_speed = 0;        /* units per frame */
static uint32_t rng = 0x1234ABCD;

static fx frand(fx lo, fx hi) {
	rng = rng * 1103515245u + 12345u;
	return lo + (fx)((rng >> 9) % 4096) * fmul(hi - lo, FX_ONE / 4096);
}

static void reset_game(void) {
	player_x = 0;
	player_vx = 0;
	score_frames = 0;
	world_speed = 100000; /* ~1.5 units/loop ≈ 7 units/s at the game's fps */
	spheres[0].center = v3(0, 65536 * 45 / 100, 0);
	spheres[0].radius = 65536 * 45 / 100;
	for (int i = 1; i <= NUM_OBST; i++) {
		/* spawn far ahead so the player has time to react */
		spheres[i].center = v3(frand(-220000, 220000),
				       32768, -(983040 + i * 786432));
		spheres[i].radius = 65536 * 50 / 100;
	}
}

static void update_game(u16 buttons) {
	fx accel = FX_ONE / 14;

	if (game_state == GS_TITLE) {
		if (!(buttons & 0x0008)) { /* start */
			reset_game();
			game_state = GS_PLAY;
		}
		return;
	}

	if (game_state == GS_DEAD) {
		death_timer++;
		if (death_timer > 40 && !(buttons & 0x0008)) {
			game_state = GS_TITLE;
			death_timer = 0;
		}
		return;
	}

	/* playing */
	if (!(buttons & 0x0080))
		player_vx -= accel;
	if (!(buttons & 0x0020))
		player_vx += accel;
	player_vx = fmul(player_vx, 65536 - 2000); /* friction */
	if (player_vx > 65536 / 8)
		player_vx = 65536 / 8;
	if (player_vx < -65536 / 8)
		player_vx = -65536 / 8;

	player_x += player_vx;
	if (player_x > 200000)
		player_x = 200000;
	if (player_x < -200000)
		player_x = -200000;

	score_frames++;

	/* difficulty ramps */
	world_speed = 100000 + score_frames * 40;
	if (world_speed > 260000)
		world_speed = 260000;

	/* obstacles scroll toward +z; recycle when passed */
	for (int i = 1; i <= NUM_OBST; i++) {
		spheres[i].center.z += world_speed;
		if (spheres[i].center.z > 120000) {
			spheres[i].center.z = -3000000 - frand(0, 1500000);
			spheres[i].center.x = frand(-220000, 220000);
		}
	}

	/* player sphere follows input */
	spheres[0].center.x = player_x;

	/* collision (xz distance, spheres overlap in y) */
	for (int i = 1; i <= NUM_OBST; i++) {
		fx dx = spheres[i].center.x - player_x;
		fx dz = spheres[i].center.z - spheres[0].center.z;
		fx rr = spheres[i].radius + spheres[0].radius;
		if (fmul(dx, dx) + fmul(dz, dz) < fmul(rr, rr)) {
			game_state = GS_DEAD;
			death_timer = 0;
			if (score_frames > best_score)
				best_score = score_frames;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void render_frame(void) {
	const fx focal = 27525;
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

static void update_camera(void) {
	fx cx = player_x >> 1; /* camera follows half of player x */
	cam_pos = v3(cx, 144179, 294912);
	vec3 target = v3(cx, 48000, -160000);
	cam_fwd   = vnorm(vsub(target, cam_pos));
	vec3 w_up = v3(0, FX_ONE, 0);
	cam_right = vnorm(vcross(cam_fwd, w_up));
	cam_up    = vnorm(vcross(cam_right, cam_fwd));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static DISPENV disp_envs[2];
static int cur_fb = 0;

static volatile u16 pad_buf1[8];

int main(int argc, const char **argv) {
	(void)argc;
	(void)argv;

	ResetGraph(0);
	sin_init();

	InitPAD((u8 *)pad_buf1, 8, (u8 *)pad_buf1, 8);
	StartPAD();

	SetDefDispEnv(&disp_envs[0], 0, 0, SCREEN_XRES, SCREEN_YRES);
	SetDefDispEnv(&disp_envs[1], 0, SCREEN_YRES, SCREEN_XRES, SCREEN_YRES);
	SetDispMask(1);

	light_dir = vnorm(v3(24576, 40960, 16384));

	spheres[0].ar = 57344; spheres[0].ag = 58982; spheres[0].ab = 61440;
	spheres[0].reflect = 57344; /* strong mirror */
	spheres[1].ar = 57344; spheres[1].ag = 12288; spheres[1].ab = 8192;
	spheres[1].reflect = 8192;
	spheres[2].ar = 8192;  spheres[2].ag = 20480; spheres[2].ab = 57344;
	spheres[2].reflect = 8192;
	spheres[3].ar = 12288; spheres[3].ag = 49152; spheres[3].ab = 12288;
	spheres[3].reflect = 8192;

	reset_game();

	uint32_t frame_counter = 0;

	for (;;) {
		/* buttons: active-low halfword filled by the BIOS pad driver
		 * (byte 2 = LSB, byte 3 = MSB of the pad buffer) */
		u16 buttons = pad_buf1[1];

		update_game(buttons);
		update_camera();
		render_frame();

		/* HUD */
		if (game_state == GS_PLAY) {
			draw_text(3, 3, "SCORE", 0xFFFF);
			draw_int(39, 3, score_frames, 5);
			draw_text(3, 12, "BEST", 0xBDF7);
			draw_int(39, 12, best_score, 5);
		} else if (game_state == GS_TITLE) {
			draw_text(17, 8, "REFLECTOR", 0x03FF);
			draw_text(9, 20, "A RAY TRACED GAME", 0xFFFF);
			if ((frame_counter >> 4) & 1)
				draw_text(14, 64, "PRESS START", 0xFFFF);
			draw_text(3, 74, "DODGE THE SPHERES", 0x001F);
		} else {
			draw_text(17, 16, "GAME OVER", 0x001F);
			draw_text(9, 30, "SCORE", 0xFFFF);
			draw_int(45, 30, score_frames, 5);
			draw_text(9, 40, "BEST", 0xBDF7);
			draw_int(45, 40, best_score, 5);
			if ((frame_counter >> 4) & 1)
				draw_text(8, 58, "PRESS START", 0xFFFF);
		}

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
