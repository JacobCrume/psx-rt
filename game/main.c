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

/*
 * Incremental analytic renderer.
 *
 * Rays are rd = fwd + u*right + v*up with an orthonormal camera basis, so
 * |rd|^2 = 1 + u^2 + v^2, stepped across the screen with pure adds (second
 * differences). Sphere intersections use e = normalize(center - cam),
 * b = e*rd (linear in u,v - one add per pixel per sphere) and the quadratic
 * b^2 - A*cc. Per-sphere screen bounding boxes reject most spheres with two
 * compares. The camera never rolls, so right.y == 0: the ground-plane hit t
 * is constant per row and ground points step linearly per pixel, so the
 * checkerboard costs only shifts. Sun shadows on the ground are solved as
 * one quadratic per obstacle per row (a pixel interval), replacing
 * per-pixel shadow rays.
 */

#define FOCAL 27525
#define ROW_DU 491                       /* (2*FOCAL)/RT_W */
#define ROW_DV 655                       /* (2*FOCAL)/RT_H */
#define ROW_U0 (-(ROW_DU * (RT_W - 1)) / 2)
#define ROW_V0 ((ROW_DV * (RT_H - 1)) / 2)  /* top of screen looks up */

static inline fx fdiv(fx a, fx b) {
	return (fx)(((int64_t)a << 16) / b);
}

static inline uint16_t pack555(fx r, fx g, fx b) {
	if (r < 0) r = 0; else if (r > 65535) r = 65535;
	if (g < 0) g = 0; else if (g > 65535) g = 65535;
	if (b < 0) b = 0; else if (b > 65535) b = 65535;
	return (uint16_t)((r >> 11) | ((g >> 6) & 0x03E0) | ((b >> 1) & 0x7C00));
}

static vec3 sph_oc[NUM_SPHERES];  /* center - cam */
static fx   sph_cc[NUM_SPHERES];  /* dist^2 - r^2 (fx^2) */
static fx   sph_b0[NUM_SPHERES];  /* e*fwd */
static fx   sph_bu[NUM_SPHERES];  /* e*right (per-pixel step) */
static fx   sph_bv[NUM_SPHERES];  /* e*up */
static fx   sph_umin[NUM_SPHERES], sph_umax[NUM_SPHERES];
static int  sph_py0[NUM_SPHERES], sph_py1[NUM_SPHERES];

static fx Lhx, Lhz, Ly2;          /* sun horizontal unit dir, Ly^2 */
static fx lit_plane;              /* precomputed plane lambert */
static fx chk_r, chk_g, chk_b;    /* light checker * lit */
static fx chk_dr, chk_dg, chk_db; /* dark checker * lit */
static fx shd_r, shd_g, shd_b;    /* blocked checker * AMBIENT */
static fx shd_dr, shd_dg, shd_db;

static void rt_init(void) {
	fx lh = fsqrt(fmul(light_dir.x, light_dir.x) +
		      fmul(light_dir.z, light_dir.z));
	Lhx = fdiv(light_dir.x, lh);
	Lhz = fdiv(light_dir.z, lh);
	Ly2 = fmul(light_dir.y, light_dir.y);
	lit_plane = AMBIENT + fmul(light_dir.y, FX_ONE - AMBIENT);

	chk_r = fmul(45000, lit_plane); chk_g = fmul(45000, lit_plane);
	chk_b = fmul(45000, lit_plane);
	chk_dr = fmul(11500, lit_plane); chk_dg = fmul(13500, lit_plane);
	chk_db = fmul(15000, lit_plane);
	shd_r = fmul(45000, AMBIENT); shd_g = fmul(45000, AMBIENT);
	shd_b = fmul(45000, AMBIENT);
	shd_dr = fmul(11500, AMBIENT); shd_dg = fmul(13500, AMBIENT);
	shd_db = fmul(15000, AMBIENT);
}

static void rt_setup(void) {
	for (int i = 0; i < NUM_SPHERES; i++) {
		vec3 oc = vsub(spheres[i].center, cam_pos);
		fx dist2 = vdot(oc, oc);
		sph_oc[i] = oc;
		sph_b0[i] = vdot(oc, cam_fwd);
		sph_bu[i] = fmul(vdot(oc, cam_right), ROW_DU);
		sph_bv[i] = vdot(oc, cam_up);
		sph_cc[i] = dist2 - fmul(spheres[i].radius, spheres[i].radius);

		if (sph_b0[i] <= 0 || sph_cc[i] < 0) {
			sph_py0[i] = 1; sph_py1[i] = 0; /* empty */
			sph_umin[i] = 0; sph_umax[i] = -1;
			continue;
		}
		fx uc = fdiv(sph_bu[i], sph_b0[i]);
		fx vc = fdiv(sph_bv[i], sph_b0[i]);
		/* tan(ang radius) = r/sqrt(cc); screen u = tan*focal.
		 * isqrt(cc<<16) = sqrt(cc_true)*256, hence the >>8. */
		fx ur = fdiv(fmul(spheres[i].radius, FOCAL),
			     isqrt32((uint32_t)sph_cc[i]) << 8) << 8 >> 8;
		ur = fmul(ur, 83558); /* 1.275 margin */
		sph_umin[i] = uc - ur;
		sph_umax[i] = uc + ur;
		sph_py0[i] = (ROW_V0 - vc - ur) / ROW_DV;
		sph_py1[i] = (ROW_V0 - vc + ur) / ROW_DV;
		if (sph_py0[i] < 0) sph_py0[i] = 0;
		if (sph_py1[i] >= RT_H) sph_py1[i] = RT_H - 1;
	}
}

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
				       32768, -(1600000 + i * 900000 + frand(0, 600000)));
		spheres[i].radius = 65536 * 50 / 100;
	}
}

static void update_game(u16 buttons) {
	fx accel = FX_ONE / 10;

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
	if (player_vx > 65536 / 5)
		player_vx = 65536 / 5;
	if (player_vx < -65536 / 5)
		player_vx = -65536 / 5;

	player_x += player_vx;
	if (player_x > 163840)
		player_x = 163840;
	if (player_x < -163840)
		player_x = -163840;

	score_frames++;

	/* difficulty ramps */
	world_speed = 35000 + score_frames * 15;
	if (world_speed > 80000)
		world_speed = 80000;

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
	rt_setup();

	fx v = ROW_V0;
	fx v2 = v * v;                     /* raw fx^4/65536 */
	fx vstep = -2 * v * ROW_DV + ROW_DV * ROW_DV;  /* raw fx^2 units */
	const fx vstep2 = 2 * ROW_DV * ROW_DV;

	const fx u0 = ROW_U0;
	const fx u0_2 = u0 * u0;           /* raw */
	fx ustep = 2 * u0 * ROW_DU + ROW_DU * ROW_DU;   /* raw fx^2 units */
	const fx ustep2 = 2 * ROW_DU * ROW_DU;

	for (int py = 0; py < RT_H; py++) {
		fx A_row = FX_ONE + (v2 >> 16);

		/* per-sphere row setup */
		fx B[NUM_SPHERES];
		int rowact[NUM_SPHERES];
		for (int i = 0; i < NUM_SPHERES; i++) {
			rowact[i] = (py >= sph_py0[i] && py <= sph_py1[i]);
			B[i] = rowact[i] ? (sph_b0[i] + fmul(v, sph_bv[i])) : 0;
		}

		/* ground row: rdy = fwd.y (right.y == 0, roll-free camera) */
		fx rdy_row = cam_fwd.y + fmul(v, cam_up.y);
		int ground = rdy_row < 0;
		fx t_plane = 0, gx = 0, gz = 0, gxs = 0, gzs = 0;
		fx fog = 0, keep = FX_ONE;
		fx fr_r = 0, fr_g = 0, fr_b = 0;      /* fogged checker light */
		fx fd_r = 0, fd_g = 0, fd_b = 0;      /* fogged checker dark */
		fx sr_r = 0, sr_g = 0, sr_b = 0;      /* fogged shadow light */
		fx sd_r = 0, sd_g = 0, sd_b = 0;      /* fogged shadow dark */
		if (ground) {
			t_plane = fmul(cam_pos.y, finv(-rdy_row));
			gx = cam_pos.x + fmul(t_plane, cam_fwd.x);
			gz = cam_pos.z + fmul(t_plane, cam_fwd.z);
			gxs = fmul(t_plane, cam_right.x);
			gzs = fmul(t_plane, cam_right.z);
			fog = fmul(t_plane, FOG_INV_RANGE);
			if (fog > FX_ONE)
				fog = FX_ONE;
			keep = FX_ONE - fog;
			fr_r = fmul(chk_r, keep) + fmul(SKY_R, fog);
			fr_g = fmul(chk_g, keep) + fmul(SKY_G, fog);
			fr_b = fmul(chk_b, keep) + fmul(SKY_B, fog);
			fd_r = fmul(chk_dr, keep) + fmul(SKY_R, fog);
			fd_g = fmul(chk_dg, keep) + fmul(SKY_G, fog);
			fd_b = fmul(chk_db, keep) + fmul(SKY_B, fog);
			sr_r = fmul(shd_r, keep) + fmul(SKY_R, fog);
			sr_g = fmul(shd_g, keep) + fmul(SKY_G, fog);
			sr_b = fmul(shd_b, keep) + fmul(SKY_B, fog);
			sd_r = fmul(shd_dr, keep) + fmul(SKY_R, fog);
			sd_g = fmul(shd_dg, keep) + fmul(SKY_G, fog);
			sd_b = fmul(shd_db, keep) + fmul(SKY_B, fog);
		}

		/* sky gradient for this row */
		fx hrow = fmul(rdy_row, finv(fsqrt(A_row)));
		if (hrow < 0)
			hrow = 0;
		fx sq_ = hrow >> 1;
		fx sky_r = SKY_R - sq_, sky_g = SKY_G - (hrow >> 2), sky_b = SKY_B;
		if (sky_r < 0) sky_r = 0;
		if (sky_g < 0) sky_g = 0;
		uint16_t sky_col = pack555(sky_r, sky_g, sky_b);

		/* shadow intervals on this ground row (per obstacle) */
		int sh_on[NUM_OBST];
		int sh_lo[NUM_OBST], sh_hi[NUM_OBST];
		if (ground) {
			for (int i = 1; i <= NUM_OBST; i++) {
				fx dx = gx - (spheres[i].center.x -
					      fmul(spheres[i].center.y, Lhx));
				fx dz = gz - (spheres[i].center.z -
					      fmul(spheres[i].center.y, Lhz));
				fx B1 = fmul(dx, Lhx) + fmul(dz, Lhz);
				fx B2 = fmul(dx, Lhz) - fmul(dz, Lhx);
				fx A1 = fmul(gxs, Lhx) + fmul(gzs, Lhz);
				fx A2 = fmul(gxs, Lhz) - fmul(gzs, Lhx);
				fx alpha = fmul(Ly2, fmul(A1, A1)) + fmul(A2, A2);
				fx beta = fmul(Ly2, fmul(B1, A1)) + fmul(B2, A2);
				fx gam = fmul(Ly2, fmul(B1, B1)) + fmul(B2, B2)
					 - fmul(spheres[i].radius, spheres[i].radius);
				if (alpha == 0) { sh_on[i-1] = 0; continue; }
				fx q = fdiv(beta, alpha);
				fx c_raw = fdiv(gam, alpha);
				fx disc_s = fmul(q, q) - 4 * c_raw;
				if (disc_s < 0) { sh_on[i-1] = 0; continue; }
				fx sqs = (fx)isqrt32((uint32_t)disc_s) << 8;
				fx lo = (q - sqs) >> 1;  /* fx pixel units */
				fx hi = (q + sqs) >> 1;
				sh_lo[i-1] = lo >> 16;   /* pixel columns */
				sh_hi[i-1] = hi >> 16;
				sh_on[i-1] = 1;
				if (sh_hi[i-1] < 0 || sh_lo[i-1] >= RT_W)
					sh_on[i-1] = 0;
				if (sh_lo[i-1] < 0) sh_lo[i-1] = 0;
				if (sh_hi[i-1] >= RT_W) sh_hi[i-1] = RT_W - 1;
			}
		} else {
			for (int i = 0; i < NUM_OBST; i++)
				sh_on[i] = 0;
		}

		fx u = ROW_U0;
		fx u2 = u0_2;
		const fx ustep0 = 2 * u0 * ROW_DU + ROW_DU * ROW_DU;
		fx ustep = ustep0;

		for (int px = 0; px < RT_W; px++) {
			fx A = A_row + (u2 >> 16);
			u2 += ustep;
			ustep += ustep2;

			fx   tbest = TMAX;
			int  hit = -1;

			for (int i = 0; i < NUM_SPHERES; i++) {
				fx b = B[i];
				B[i] = b + sph_bu[i];
				if (!rowact[i])
					continue;
				if (u < sph_umin[i] || u > sph_umax[i])
					continue;
				/* divide the quadratic by A to stay in range:
				 * t^2 - 2*(b/A)*t + cc/A = 0 */
				fx disc = fmul(b, b) - fmul(A, sph_cc[i]);
				if (disc < 0)
					continue;
				fx t = fdiv(b - (fx)(isqrt32((uint32_t)disc) << 8), A);
				if (t < 0 || t >= tbest)
					continue;
				tbest = t;
				hit = i;
			}

			if (ground && t_plane < tbest) {
				tbest = t_plane;
				hit = -2; /* plane */
			}

			uint16_t out;

			if (hit == -2) {
				/* ground: checker + sun shadow intervals + fog */
				int checker = ((gx >> 16) ^ (gz >> 16)) & 1;
				int blocked = 0;
				for (int i = 0; i < NUM_OBST; i++) {
					if (sh_on[i] && px >= sh_lo[i] && px <= sh_hi[i]) {
						blocked = 1;
						break;
					}
				}
				out = blocked ? pack555(sr_r, sr_g, sr_b)
					      : pack555(fr_r, fr_g, fr_b);
			} else if (hit >= 0) {
				/* sphere hit: shade with fog */
				Sphere *sp = &spheres[hit];
				vec3 rd = v3(cam_fwd.x + fmul(cam_right.x, u) + fmul(cam_up.x, v),
					     cam_fwd.y + fmul(cam_right.y, u) + fmul(cam_up.y, v),
					     cam_fwd.z + fmul(cam_right.z, u) + fmul(cam_up.z, v));
				vec3 p = vadd(cam_pos, vscale(rd, tbest));
				vec3 n = vscale(vsub(p, sp->center), finv(sp->radius));

				fx ndl = vdot(n, light_dir);
				if (ndl < 0)
					ndl = 0;

				fx lit;
				if (hit == 0) {
					/* player: mirror, skip shadow ray (tiny
					 * diffuse term) */
					lit = AMBIENT + fmul(ndl, FX_ONE - AMBIENT);
				} else {
					/* shadow ray vs all other spheres */
					vec3 sorg = vadd(p, vscale(n, 4096));
					fx   best2 = 1 << 30;
					int  blocked = 0;
					for (int k = 0; k < NUM_SPHERES; k++) {
						if (k == hit)
							continue;
						vec3 oc = vsub(sorg, spheres[k].center);
						fx b = vdot(oc, light_dir);
						if (b <= 0)
							continue;
						fx c2 = vdot(oc, oc) -
							fmul(spheres[k].radius,
							     spheres[k].radius);
						fx disc = fmul(b, b) - c2;
						if (disc >= 0) {
							blocked = 1;
							break;
						}
					}
					lit = blocked ? AMBIENT
						     : (AMBIENT + fmul(ndl, FX_ONE - AMBIENT));
				}

				fx cr = fmul(sp->ar, lit);
				fx cg = fmul(sp->ag, lit);
				fx cb = fmul(sp->ab, lit);

				if (sp->reflect > 0) {
					/* mirror bounce */
					fx   dn  = vdot(rd, n) * 2;
					vec3 rfl = vsub(rd, vscale(n, dn));
					vec3 org = vadd(p, vscale(rfl, 1024));
					fx   rr = 0, rg = 0, rb = 0;
					fx   t2;
					vec3 n2;
					int  h2 = -1;
					fx   t2best = TMAX;
					for (int k = 0; k < NUM_SPHERES; k++) {
						if (k == hit)
							continue;
						vec3 oc = vsub(org, spheres[k].center);
						fx bb = vdot(oc, rfl);
						fx cc = vdot(oc, oc) -
							fmul(spheres[k].radius,
							     spheres[k].radius);
						fx disc = fmul(bb, bb) - cc;
						if (disc < 0)
							continue;
						fx sq = fsqrt(disc);
						fx t = -bb - sq;
						if (t < 0 || t >= t2best)
							continue;
						t2best = t;
						h2 = k;
					}
					if (h2 >= 0) {
						Sphere *s2 = &spheres[h2];
						vec3 p2 = vadd(org, vscale(rfl, t2best));
						vec3 nn = vscale(vsub(p2, s2->center),
								 finv(s2->radius));
						fx ndl2 = vdot(nn, light_dir);
						if (ndl2 < 0)
							ndl2 = 0;
						fx l2 = AMBIENT + fmul(ndl2, FX_ONE - AMBIENT);
						rr = fmul(s2->ar, l2);
						rg = fmul(s2->ag, l2);
						rb = fmul(s2->ab, l2);
					} else if (rfl.y < 0) {
						fx t3 = fmul(p.y, finv(-rfl.y));
						vec3 g = vadd(org, vscale(rfl, t3));
						int checker = ((g.x >> 16) ^ (g.z >> 16)) & 1;
						rr = fmul(checker ? chk_r : chk_dr, lit_plane);
						rg = fmul(checker ? chk_g : chk_dg, lit_plane);
						rb = fmul(checker ? chk_b : chk_db, lit_plane);
					} else {
						fx hh = rfl.y;
						if (hh < 0)
							hh = 0;
						rr = SKY_R - (hh >> 1);
						rg = SKY_G - (hh >> 2);
						rb = SKY_B;
						if (rr < 0) rr = 0;
						if (rg < 0) rg = 0;
						if (rb > 65535) rb = 65535;
					}

					fx k = sp->reflect;
					cr = fmul(cr, FX_ONE - k) + fmul(rr, k);
					cg = fmul(cg, FX_ONE - k) + fmul(rg, k);
					cb = fmul(cb, FX_ONE - k) + fmul(rb, k);
				}

				/* fog */
				fx fog = fmul(tbest, FOG_INV_RANGE);
				if (fog > FX_ONE)
					fog = FX_ONE;
				fx keep = FX_ONE - fog;
				cr = fmul(cr, keep) + fmul(SKY_R, fog);
				cg = fmul(cg, keep) + fmul(SKY_G, fog);
				cb = fmul(cb, keep) + fmul(SKY_B, fog);

				out = pack555(cr, cg, cb);
			} else {
				out = sky_col;
			}

			fb[py][px] = out;
			u += ROW_DU;
			gx += gxs;
			gz += gzs;
		}

		v -= ROW_DV;
		v2 += vstep;
		vstep += vstep2;
	}
}

static void update_camera(void) {
	fx cx = player_x; /* camera follows the player exactly */
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
	rt_init();

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
