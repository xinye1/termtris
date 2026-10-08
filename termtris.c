/*
 * termtris: a small, faithful Tetris for the terminal.
 *
 * Rules follow the 1989 NES game: Nintendo rotation (no wall kicks), the NES
 * gravity table, lock on contact, 40/100/300/1200 x (level+1) line scores and
 * a new level every 10 lines. The board is drawn like the 1984 original, and
 * the Game Boy's Korobeiniki theme plays from a tiny built-in synth. It runs
 * on Linux terminals and on the Windows console (PowerShell, Windows Terminal).
 *
 * Smoothness comes from three things: a fixed 60 Hz simulation clock, frames
 * that only send changed cells inside synchronized-output markers, and the
 * kitty keyboard protocol. That protocol reports key releases, so auto-shift
 * timing is ours instead of the OS key-repeat's. Terminals without it fall
 * back to plain keys, where holding a key relies on the OS repeat. The Windows
 * console reports key releases natively.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <direct.h>
#else
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define MAYBE_UNUSED __attribute__((unused))
#else
#define MAYBE_UNUSED
#endif

/* Bumped for each release; the release workflow checks it matches the tag. */
#define TERMTRIS_VERSION "0.1.0-beta.2"

enum { W = 10, H = 20, FPS = 60 };

/* Timings, in 60 Hz frames. */
enum {
	DAS = 10,          /* hold a direction this long before auto-shift starts */
	ARR = 2,           /* then shift once every ARR frames */
	SOFT = 2,          /* soft drop: one row every SOFT frames (NES: 2) */
	CLEAR_FRAMES = 20, /* line-clear animation (NES: about 20) */
};
#define FRAME_NS (1000000000LL / FPS)

/* NES (NTSC) frames per row, levels 0-28. Level 29 and up is 1. */
static const unsigned char gravity[29] = {
	48, 43, 38, 33, 28, 23, 18, 13, 8, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};
static const int line_score[5] = { 0, 40, 100, 300, 1200 };

static int frames_per_row(int level) { return level < 29 ? gravity[level] : 1; }

/* ---- pieces ------------------------------------------------------------ */

enum { PI, PO, PT, PS, PZ, PJ, PL, NPIECE };
typedef struct { int x, y; } Pt;

/* Spawn orientations around the pivot, y down, as on the NES. */
static const Pt spawn_shape[NPIECE][4] = {
	[PI] = { { -2, 0 }, { -1, 0 }, { 0, 0 }, { 1, 0 } },
	[PO] = { { -1, 0 }, { 0, 0 }, { -1, 1 }, { 0, 1 } },
	[PT] = { { -1, 0 }, { 0, 0 }, { 1, 0 }, { 0, 1 } },
	[PS] = { { 0, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 } },
	[PZ] = { { -1, 0 }, { 0, 0 }, { 0, 1 }, { 1, 1 } },
	[PJ] = { { -1, 0 }, { 0, 0 }, { 1, 0 }, { 1, 1 } },
	[PL] = { { -1, 0 }, { 0, 0 }, { 1, 0 }, { -1, 1 } },
};
static const int nrot[NPIECE] = { 2, 1, 4, 2, 2, 4, 4 };
static Pt shape[NPIECE][4][4]; /* [piece][rotation][cell] */

static void init_shapes(void)
{
	/* I, S and Z flip between two fixed states instead of pivoting. */
	static const Pt vertical[NPIECE][4] = {
		[PI] = { { 0, -2 }, { 0, -1 }, { 0, 0 }, { 0, 1 } },
		[PS] = { { 0, -1 }, { 0, 0 }, { 1, 0 }, { 1, 1 } },
		[PZ] = { { 1, -1 }, { 0, 0 }, { 1, 0 }, { 0, 1 } },
	};
	for (int p = 0; p < NPIECE; p++)
		for (int r = 0; r < nrot[p]; r++)
			for (int c = 0; c < 4; c++) {
				Pt v = spawn_shape[p][c];
				if (r && nrot[p] == 2)
					v = vertical[p][c];
				else
					for (int k = 0; k < r; k++)
						v = (Pt){ -v.y, v.x }; /* clockwise */
				shape[p][r][c] = v;
			}
}

/* ---- game state -------------------------------------------------------- */

enum { ST_TITLE, ST_PLAY, ST_CLEAR, ST_PAUSE, ST_OVER };

static struct {
	unsigned char board[H][W]; /* 0 empty, else piece + 1 */
	int state, resume_state;
	int piece, rot, px, py, next, last_roll;
	int score, lines, level, start_level, hiscore;
	bool new_high;
	int gravity_timer, soft_timer, soft_rows;
	int clear_rows[4], nclear, clear_timer;
	/* held keys; only tracked when the terminal reports releases */
	bool precise, held_l, held_r, soft_held, soft_armed;
	int held_dir, das;
	bool ghost, preview, classic, music, quit;
	int games; /* bumped per game so the music can start over */
	uint32_t rng;
} g;

static uint32_t rnd(void)
{
	g.rng ^= g.rng << 13;
	g.rng ^= g.rng >> 17;
	g.rng ^= g.rng << 5;
	return g.rng;
}

/* NES randomizer: roll 8 sides; on a repeat or the dummy side, reroll once. */
static int roll(void)
{
	int r = rnd() % 8;
	if (r == 7 || r == g.last_roll)
		r = rnd() % 7;
	return g.last_roll = r;
}

static bool fits(int piece, int rot, int x, int y)
{
	for (int c = 0; c < 4; c++) {
		int cx = x + shape[piece][rot][c].x, cy = y + shape[piece][rot][c].y;
		if (cx < 0 || cx >= W || cy >= H)
			return false;
		if (cy >= 0 && g.board[cy][cx])
			return false;
	}
	return true;
}

static void save_hiscore(void);

static void game_over(void)
{
	g.state = ST_OVER;
	g.new_high = g.score > g.hiscore;
	if (g.new_high) {
		g.hiscore = g.score;
		save_hiscore();
	}
}

static void spawn(void)
{
	g.piece = g.next;
	g.next = roll();
	g.rot = 0;
	g.px = 5;
	g.py = 0;
	g.gravity_timer = g.soft_timer = g.soft_rows = 0;
	g.soft_armed = false; /* NES: holding down never carries into a new piece */
	g.state = ST_PLAY;
	if (!fits(g.piece, g.rot, g.px, g.py))
		game_over();
}

static void lock(void)
{
	bool topout = false;
	for (int c = 0; c < 4; c++) {
		int cx = g.px + shape[g.piece][g.rot][c].x;
		int cy = g.py + shape[g.piece][g.rot][c].y;
		if (cy < 0)
			topout = true;
		else
			g.board[cy][cx] = g.piece + 1;
	}
	g.score += g.soft_rows;
	if (topout) {
		game_over();
		return;
	}
	g.nclear = 0;
	for (int y = 0; y < H; y++) {
		int x = 0;
		while (x < W && g.board[y][x])
			x++;
		if (x == W)
			g.clear_rows[g.nclear++] = y;
	}
	if (g.nclear) {
		g.state = ST_CLEAR;
		g.clear_timer = 0;
	} else {
		spawn();
	}
}

static void finish_clear(void)
{
	int dst = H - 1;
	for (int y = H - 1; y >= 0; y--) {
		bool full = false;
		for (int i = 0; i < g.nclear; i++)
			full |= g.clear_rows[i] == y;
		if (!full)
			memmove(g.board[dst--], g.board[y], W);
	}
	while (dst >= 0)
		memset(g.board[dst--], 0, W);
	g.score += line_score[g.nclear] * (g.level + 1);
	g.lines += g.nclear;
	/* With a start level of 0-9 this matches the NES transition rule. */
	if (g.lines / 10 > g.level)
		g.level = g.lines / 10;
	g.nclear = 0;
	spawn();
}

static void new_game(void)
{
	memset(g.board, 0, sizeof g.board);
	g.score = g.lines = 0;
	g.level = g.start_level;
	g.new_high = false;
	g.games++;
	g.last_roll = -1;
	g.next = roll();
	spawn();
}

static void shift(int dx)
{
	if (fits(g.piece, g.rot, g.px + dx, g.py))
		g.px += dx;
}

static void rotate(int dir)
{
	int r = (g.rot + dir + nrot[g.piece]) % nrot[g.piece];
	if (fits(g.piece, r, g.px, g.py))
		g.rot = r;
}

/* Move down one row, or lock if something is below. */
static void fall(bool soft)
{
	if (fits(g.piece, g.rot, g.px, g.py + 1)) {
		g.py++;
		g.soft_rows += soft;
	} else {
		lock();
	}
}

static void hard_drop(void)
{
	while (fits(g.piece, g.rot, g.px, g.py + 1)) {
		g.py++;
		g.score += 2;
	}
	lock();
}


/* Advance one 60 Hz frame. Returns true if anything visible changed. */
static bool tick(void)
{
	if (g.state == ST_CLEAR) {
		/* Auto-shift keeps charging so the next piece can slide at once. */
		if (g.held_dir && g.das < DAS - 1)
			g.das++;
		if (++g.clear_timer >= CLEAR_FRAMES)
			finish_clear();
		return true;
	}
	if (g.state != ST_PLAY)
		return false;

	bool changed = false;
	if (g.held_dir && ++g.das >= DAS) {
		shift(g.held_dir);
		g.das = DAS - ARR;
		changed = true;
	}
	int fpr = frames_per_row(g.level);
	if (g.soft_held && g.soft_armed && SOFT <= fpr) { /* scores even at equal speed */
		if (++g.soft_timer >= SOFT) {
			g.soft_timer = 0;
			fall(true);
			changed = true;
		}
	} else if (++g.gravity_timer >= fpr) {
		g.gravity_timer = 0;
		fall(false);
		changed = true;
	}
	return changed;
}

/* ---- input ------------------------------------------------------------- */

enum { EV_PRESS = 1, EV_REPEAT = 2, EV_RELEASE = 3 };
enum {
	K_NONE, K_LEFT, K_RIGHT, K_DOWN, K_CW, K_CCW, K_DROP, K_PAUSE, K_QUIT,
	K_GHOST, K_NEXT, K_STYLE, K_ENTER, K_RESTART, K_MUSIC, K_FOCUS_OUT, K_COUNT,
};
typedef void (*emit_fn)(int key, int ev);

static int key_for_char(int c, bool ctrl)
{
	if (ctrl)
		return c == 'c' || c == 'd' ? K_QUIT : K_NONE;
	switch (c < 128 ? tolower(c) : c) {
	case 'h': return K_LEFT;
	case 'l': return K_RIGHT;
	case 'j': return K_DOWN;
	case 'k': case 'x': return K_CW;
	case 'z': return K_CCW;
	case ' ': return K_DROP;
	case 'p': case 27: return K_PAUSE;
	case 'q': case 3: case 4: return K_QUIT;
	case 'g': return K_GHOST;
	case 'n': return K_NEXT;
	case 'c': return K_STYLE;
	case 'r': return K_RESTART;
	case 'm': return K_MUSIC;
	case '\r': case '\n': return K_ENTER;
	}
	return K_NONE;
}

static int key_for_arrow(int c)
{
	switch (c) {
	case 'A': return K_CW;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	}
	return K_NONE;
}

/*
 * One CSI sequence. Kitty reports keys as "CSI code;mods:event u" and arrows
 * as "CSI 1;mods:event A-D"; legacy terminals send bare "CSI A-D".
 */
static void handle_csi(const unsigned char *p, size_t n, int final, emit_fn emit)
{
	if (n && (p[0] == '?' || p[0] == '>' || p[0] == '<' || p[0] == '='))
		return; /* a reply to a query, not a key */
	int f[3][2] = { { 0 } }, fi = 0, si = 0;
	for (size_t i = 0; i < n; i++) {
		if (p[i] == ';') {
			if (++fi > 2)
				break;
			si = 0;
		} else if (p[i] == ':') {
			si = si < 1 ? si + 1 : si;
		} else if (isdigit(p[i])) {
			f[fi][si] = f[fi][si] * 10 + (p[i] - '0');
		}
	}
	int mods = f[1][0] ? f[1][0] - 1 : 0;
	int ev = f[1][1] ? f[1][1] : EV_PRESS;
	int k = K_NONE;
	if (final == 'u')
		k = key_for_char(f[0][0], mods & 4);
	else if (final == 'O' && n == 0)
		k = K_FOCUS_OUT;
	else if (final >= 'A' && final <= 'D')
		k = key_for_arrow(final);
	if (k)
		emit(k, ev);
}

/*
 * Parse terminal input. Returns bytes consumed; the rest is an unfinished
 * sequence. (Unused on Windows, which delivers keys as console records.)
 */
static MAYBE_UNUSED size_t parse(const unsigned char *b, size_t n, emit_fn emit)
{
	size_t i = 0;
	while (i < n) {
		if (b[i] != 27) {
			int k = key_for_char(b[i++], false);
			if (k)
				emit(k, EV_PRESS);
			continue;
		}
		if (i + 1 == n) { /* a lone Esc */
			if (g.precise) /* Esc arrives as CSI 27 u: this is a split sequence */
				return i;
			emit(K_PAUSE, EV_PRESS);
			i++;
			continue;
		}
		if (b[i + 1] == 'O') { /* SS3 arrows */
			if (i + 2 == n)
				return i;
			int k = key_for_arrow(b[i + 2]);
			if (k)
				emit(k, EV_PRESS);
			i += 3;
			continue;
		}
		if (b[i + 1] == ']') { /* OSC: a late theme reply, never a key */
			size_t j = i + 2;
			while (j < n && b[j] != 7 && !(b[j] == 27 && j + 1 < n && b[j + 1] == '\\'))
				j++;
			if (j == n || (b[j] == 27 && j + 1 == n))
				return i;
			i = j + (b[j] == 7 ? 1 : 2);
			continue;
		}
		if (b[i + 1] != '[') { /* Alt+key: drop the Esc */
			i++;
			continue;
		}
		size_t j = i + 2;
		while (j < n && b[j] >= 0x20 && b[j] <= 0x3f)
			j++;
		if (j == n)
			return i;
		handle_csi(b + i + 2, j - i - 2, b[j], emit);
		i = j + 1;
	}
	return i;
}

static void press_dir(int dir)
{
	if (!g.precise)
		return;
	*(dir < 0 ? &g.held_l : &g.held_r) = true;
	g.held_dir = dir;
	g.das = 0;
}

static void release_dir(int dir)
{
	*(dir < 0 ? &g.held_l : &g.held_r) = false;
	if (g.held_dir == dir) {
		g.held_dir = g.held_l ? -1 : g.held_r ? 1 : 0;
		g.das = 0;
	}
}

static void pause_game(void)
{
	if (g.state == ST_PLAY || g.state == ST_CLEAR) {
		g.resume_state = g.state;
		g.state = ST_PAUSE;
	}
}

static void on_key(int k, int ev)
{
	if (ev == EV_REPEAT)
		return; /* precise mode does its own repeating */
	if (ev == EV_RELEASE) {
		if (k == K_LEFT)
			release_dir(-1);
		else if (k == K_RIGHT)
			release_dir(1);
		else if (k == K_DOWN)
			g.soft_held = false;
		return;
	}

	switch (k) {
	case K_QUIT:
		g.quit = true;
		return;
	case K_FOCUS_OUT: /* releases while unfocused never arrive */
		g.held_l = g.held_r = g.soft_held = false;
		g.held_dir = 0;
		pause_game();
		return;
	case K_GHOST:
		g.ghost = !g.ghost;
		return;
	case K_NEXT:
		g.preview = !g.preview;
		return;
	case K_STYLE:
		g.classic = !g.classic;
		return;
	case K_MUSIC:
		g.music = !g.music;
		return;
	case K_LEFT:
		press_dir(-1);
		break;
	case K_RIGHT:
		press_dir(1);
		break;
	case K_DOWN:
		if (g.precise) {
			g.soft_held = g.soft_armed = true;
			g.soft_timer = 0;
		}
		break;
	}

	switch (g.state) {
	case ST_TITLE:
		if (k == K_LEFT || k == K_RIGHT)
			g.start_level = (g.start_level + (k == K_LEFT ? 9 : 1)) % 10;
		else if (k == K_DROP || k == K_ENTER)
			new_game();
		break;
	case ST_PLAY:
		switch (k) {
		case K_LEFT: shift(-1); break;
		case K_RIGHT: shift(1); break;
		case K_DOWN: fall(true); break;
		case K_CW: rotate(1); break;
		case K_CCW: rotate(-1); break;
		case K_DROP: hard_drop(); break;
		case K_PAUSE: pause_game(); break;
		}
		break;
	case ST_CLEAR:
		if (k == K_PAUSE)
			pause_game();
		break;
	case ST_PAUSE:
		if (k == K_PAUSE || k == K_ENTER || k == K_DROP)
			g.state = g.resume_state;
		break;
	case ST_OVER: /* only r: a mashed drop key must not skip this screen */
		if (k == K_RESTART)
			g.state = ST_TITLE;
		break;
	}
}

/* ---- high score -------------------------------------------------------- */

static bool hiscore_path(char *buf, size_t n, bool make_dir)
{
	int len;
#ifdef _WIN32
	const char *appdata = getenv("APPDATA");
	if (!appdata || !*appdata)
		return false;
	len = snprintf(buf, n, "%s\\termtris", appdata);
#else
	const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	if (xdg && *xdg)
		len = snprintf(buf, n, "%s/termtris", xdg);
	else if (home && *home)
		len = snprintf(buf, n, "%s/.local/share/termtris", home);
	else
		return false;
#endif
	if (len < 0 || (size_t)len + sizeof "/highscore" > n)
		return false;
	if (make_dir) /* mkdir -p; each call fails harmlessly if the folder exists */
		for (char *p = buf + 1;; p++)
			if (*p == '/' || *p == '\\' || !*p) {
				char c = *p;
				*p = 0;
#ifdef _WIN32
				_mkdir(buf);
#else
				mkdir(buf, 0700);
#endif
				if (!(*p = c))
					break;
			}
	strcat(buf, "/highscore");
	return true;
}

static void load_hiscore(void)
{
	char path[4096];
	FILE *f;
	if (!hiscore_path(path, sizeof path, false) || !(f = fopen(path, "r")))
		return;
	if (fscanf(f, "%d", &g.hiscore) != 1 || g.hiscore < 0)
		g.hiscore = 0;
	fclose(f);
}

static void save_hiscore(void)
{
	char path[4096];
	FILE *f;
	if (!hiscore_path(path, sizeof path, true) || !(f = fopen(path, "w")))
		return;
	fprintf(f, "%d\n", g.hiscore);
	fclose(f);
}

/* ---- colours ----------------------------------------------------------- */

/*
 * A colour is the terminal default, a palette index (so the theme applies),
 * or 24-bit RGB derived from the theme's own palette.
 */
typedef uint32_t Color;
#define PAL(n) (0x1000000u | (uint32_t)(n))
#define RGB(c) (0x2000000u | (uint32_t)(c))

/* Theme colours the terminal reported: palette 0-15, then fg and bg. */
enum { TH_FG = 16, TH_BG, TH_N };
static uint32_t theme_rgb[TH_N], theme_have; /* theme_have: bit per entry */

/* Palette slots for I O T S Z J L; the theme has no orange, see init_colors. */
static const int piece_pal[NPIECE] = { 6, 11, 5, 2, 1, 4, 208 };

static bool shaded;              /* every piece colour is known as RGB */
static uint32_t base_rgb[NPIECE];
static Color dot_color = PAL(8); /* "bright black", muted in most themes */

static uint32_t mix(uint32_t a, uint32_t b, int pct_b)
{
	uint32_t out = 0;
	for (int sh = 16; sh >= 0; sh -= 8) {
		int ca = a >> sh & 0xff, cb = b >> sh & 0xff;
		out |= (uint32_t)((ca * (100 - pct_b) + cb * pct_b + 50) / 100) << sh;
	}
	return out;
}

static uint32_t darken(uint32_t c, int pct) { return mix(c, 0, pct); }

static bool have(int i) { return theme_have >> i & 1; }

static void init_colors(void)
{
	shaded = true;
	for (int p = 0; p < NPIECE; p++) {
		if (p == PL) { /* orange: halfway between the theme's red and yellow */
			shaded &= have(1) && have(3);
			base_rgb[p] = mix(theme_rgb[1], theme_rgb[3], 50);
		} else {
			shaded &= have(piece_pal[p]);
			base_rgb[p] = theme_rgb[piece_pal[p]];
		}
	}
	if (have(TH_FG) && have(TH_BG))
		dot_color = RGB(mix(theme_rgb[TH_BG], theme_rgb[TH_FG], 28));
}

static int hexval(int c) { return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10; }

/* "rgb:RRRR/GGGG/BBBB", with 1-4 hex digits per channel. */
static bool parse_rgb(const char *s, const char *end, uint32_t *out)
{
	uint32_t rgb = 0;
	if (end - s < 4 || memcmp(s, "rgb:", 4))
		return false;
	s += 4;
	for (int i = 0; i < 3; i++) {
		unsigned v = 0;
		int digits = 0;
		while (s < end && isxdigit((unsigned char)*s) && digits < 5) {
			v = v * 16 + hexval((unsigned char)*s++);
			digits++;
		}
		if (!digits || digits > 4)
			return false;
		unsigned max = (1u << 4 * digits) - 1;
		rgb = rgb << 8 | (v * 255 + max / 2) / max;
		if (i < 2 && (s >= end || *s++ != '/'))
			return false;
	}
	*out = rgb;
	return true;
}

/* Pick the OSC 4 (palette) and OSC 10/11 (fg/bg) replies out of the input. */
static void parse_theme(const char *b, size_t n)
{
	const char *end = b + n;
	for (const char *p = b; p + 2 < end; p++) {
		if (p[0] != 27 || p[1] != ']')
			continue;
		const char *s = p + 2;
		int code = 0, idx = 0;
		while (s < end && isdigit((unsigned char)*s))
			code = code * 10 + *s++ - '0';
		if (s >= end || *s++ != ';')
			continue;
		if (code == 4) {
			while (s < end && isdigit((unsigned char)*s) && idx < 1000)
				idx = idx * 10 + *s++ - '0';
			if (s >= end || *s++ != ';' || idx > 15)
				continue;
		} else if (code == 10 || code == 11) {
			idx = code == 10 ? TH_FG : TH_BG;
		} else {
			continue;
		}
		uint32_t rgb;
		if (parse_rgb(s, end, &rgb)) {
			theme_rgb[idx] = rgb;
			theme_have |= 1u << idx;
		}
	}
}

/* ---- rendering --------------------------------------------------------- */

/*
 * Frames are drawn into `back`, then only the cells that differ from `front`
 * (what the terminal shows) are sent.
 */
typedef struct {
	char ch[5]; /* one UTF-8 character */
	unsigned char attr;
	Color fg, bg;
} Cell;
enum { A_BOLD = 1, A_DIM = 2 };
#define ACCENT PAL(6)

enum { BOARD_W = 2 * W + 4, LAYOUT_W = BOARD_W + 2 + 22, LAYOUT_H = H + 2 };

static int scr_w, scr_h;
static Cell *back, *front;
static void term_write(const char *s, size_t n);
static void term_size(int *w, int *h);
static bool full_redraw;
static char *ob;
static size_t ob_len, ob_cap;

static void obw(const char *s, size_t n)
{
	if (ob_len + n > ob_cap) {
		ob_cap = (ob_len + n) * 2;
		if (!(ob = realloc(ob, ob_cap)))
			exit(1);
	}
	memcpy(ob + ob_len, s, n);
	ob_len += n;
}

static void obs(const char *s) { obw(s, strlen(s)); }

static void obf(const char *fmt, ...)
{
	char buf[64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0)
		obw(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static int utf8_len(unsigned char c)
{
	return c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
}

static int text_width(const char *s)
{
	int w = 0;
	for (; *s; s++)
		w += ((unsigned char)*s & 0xc0) != 0x80;
	return w;
}

static void put(int x, int y, const char *s, Color fg, Color bg, int attr)
{
	while (*s) {
		int len = utf8_len((unsigned char)*s);
		if (x >= 0 && x < scr_w && y >= 0 && y < scr_h) {
			Cell *c = &back[y * scr_w + x];
			memset(c->ch, 0, sizeof c->ch);
			memcpy(c->ch, s, len);
			c->fg = fg;
			c->bg = bg;
			c->attr = attr;
		}
		s += len;
		x++;
	}
}

static bool cell_eq(const Cell *a, const Cell *b)
{
	return a->fg == b->fg && a->bg == b->bg && a->attr == b->attr &&
	       !memcmp(a->ch, b->ch, sizeof a->ch);
}

static int bx, by; /* top-left of the layout */

/* One brick, two columns wide, at screen position sx, sy. */
static void draw_brick(int sx, int sy, int piece, bool ghost)
{
	if (g.classic) {
		put(sx, sy, "[]", 0, 0, ghost ? A_DIM : A_BOLD);
	} else if (!shaded) {
		put(sx, sy, ghost ? "░░" : "██", PAL(piece_pal[piece]), 0, 0);
	} else if (ghost) {
		put(sx, sy, "░░", RGB(base_rgb[piece]), 0, 0);
	} else {
		/*
		 * Lit from the top left: the right half is a shade darker and a
		 * hairline shadow runs along the bottom edge.
		 */
		uint32_t c = base_rgb[piece];
		put(sx, sy, "▁", RGB(darken(c, 30)), RGB(c), 0);
		put(sx + 1, sy, "▁", RGB(darken(c, 36)), RGB(darken(c, 8)), 0);
	}
}

static void draw_cell(int x, int y, int v, bool ghost)
{
	int sx = bx + 2 + 2 * x, sy = by + y;
	if (v)
		draw_brick(sx, sy, v - 1, ghost);
	else
		put(sx, sy, " .", dot_color, 0, 0);
}

static void board_text(int row, const char *s, Color fg, int attr)
{
	put(bx + 2, by + row, "                    ", 0, 0, 0);
	put(bx + 2 + (2 * W - text_width(s)) / 2, by + row, s, fg, 0, attr);
}

static void draw_piece(int piece, int rot, int x, int y, bool ghost)
{
	for (int c = 0; c < 4; c++) {
		int cy = y + shape[piece][rot][c].y;
		if (cy >= 0)
			draw_cell(x + shape[piece][rot][c].x, cy, piece + 1, ghost);
	}
}

static void draw_stat(int x, int y, const char *label, int value)
{
	char buf[16];
	put(x, y, label, 0, 0, A_DIM);
	snprintf(buf, sizeof buf, "%d", value);
	put(x, y + 1, buf, 0, 0, A_BOLD);
}

static void draw(void)
{
	bool live = g.state == ST_PLAY || g.state == ST_CLEAR;

	/* The well exactly as the 1984 original drew it. */
	for (int y = 0; y < H; y++) {
		put(bx, by + y, "<!", 0, 0, 0);
		put(bx + 2 * W + 2, by + y, "!>", 0, 0, 0);
	}
	put(bx, by + H, "<!====================!>", 0, 0, 0);
	put(bx + 2, by + H + 1, "\\/\\/\\/\\/\\/\\/\\/\\/\\/\\/", 0, 0, 0);

	/* Cleared rows vanish from the middle outwards, as on the NES. */
	int span = g.clear_timer * 5 / CLEAR_FRAMES;
	for (int y = 0; y < H; y++) {
		bool clearing = false;
		for (int i = 0; i < g.nclear && g.state == ST_CLEAR; i++)
			clearing |= g.clear_rows[i] == y;
		for (int x = 0; x < W; x++) {
			int v = g.board[y][x];
			if (clearing && x >= 4 - span && x <= 5 + span)
				v = 0;
			if (!live && g.state != ST_OVER)
				v = 0; /* paused or title: hide the stack */
			draw_cell(x, y, v, false);
		}
	}
	if (g.state == ST_PLAY) {
		if (g.ghost) {
			int gy = g.py;
			while (fits(g.piece, g.rot, g.px, gy + 1))
				gy++;
			draw_piece(g.piece, g.rot, g.px, gy, true);
		}
		draw_piece(g.piece, g.rot, g.px, g.py, false);
	}

	char buf[32];
	switch (g.state) {
	case ST_TITLE:
		board_text(5, "SELECT LEVEL", 0, A_BOLD);
		snprintf(buf, sizeof buf, "<  %d  >", g.start_level);
		board_text(7, buf, ACCENT, A_BOLD);
		board_text(10, "SPACE TO START", 0, 0);
		board_text(13, g.precise ? "precise keys" : "basic keys", 0, A_DIM);
		break;
	case ST_PAUSE:
		board_text(9, "PAUSED", 0, A_BOLD);
		board_text(11, "p to resume", 0, A_DIM);
		break;
	case ST_OVER:
		board_text(7, "", 0, 0);
		board_text(8, "GAME OVER", 0, A_BOLD);
		board_text(9, "", 0, 0);
		board_text(10, g.new_high ? "NEW HIGH SCORE" : "", ACCENT, A_BOLD);
		board_text(11, "", 0, 0);
		board_text(12, "r restart  q quit", 0, A_DIM);
		board_text(13, "", 0, 0);
		break;
	}

	int x = bx + BOARD_W + 2;
	draw_stat(x, by, "SCORE", g.score);
	draw_stat(x, by + 3, "LEVEL", g.state == ST_TITLE ? g.start_level : g.level);
	draw_stat(x, by + 6, "LINES", g.lines);
	draw_stat(x, by + 9, "HIGH", g.score > g.hiscore ? g.score : g.hiscore);
	put(x, by + 12, "NEXT", 0, 0, A_DIM);
	if (live && g.preview) {
		int minx = 0, maxx = 0;
		for (int c = 0; c < 4; c++) {
			int cx = spawn_shape[g.next][c].x;
			minx = cx < minx ? cx : minx;
			maxx = cx > maxx ? cx : maxx;
		}
		int ox = x + (8 - 2 * (maxx - minx + 1)) / 2 - 2 * minx;
		for (int c = 0; c < 4; c++)
			draw_brick(ox + 2 * spawn_shape[g.next][c].x,
				   by + 13 + spawn_shape[g.next][c].y, g.next, false);
	}
	put(x, by + 17, "←→ move   ↑ x rotate", 0, 0, A_DIM);
	put(x, by + 18, "↓ soft   space drop", 0, 0, A_DIM);
	put(x, by + 19, "z ccw  p pause  q quit", 0, 0, A_DIM);
	put(x, by + 20, "g ghost  n next", 0, 0, A_DIM);
	put(x, by + 21, "c style  m music", 0, 0, A_DIM);
}

static void sgr_color(Color c, int base)
{
	if (c >> 24 == 1)
		obf(";%d;5;%u", base, c & 0xff);
	else if (c >> 24 == 2)
		obf(";%d;2;%u;%u;%u", base, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff);
}

static void flush(void)
{
	Cell cur = { .attr = 0xff }; /* matches nothing: the first cell sets SGR */
	int cx = -1, cy = -1;
	ob_len = 0;
	obs("\x1b[?2026h"); /* synchronized output: no half-drawn frames */
	if (full_redraw)
		obs("\x1b[0m\x1b[2J");
	for (int y = 0; y < scr_h; y++)
		for (int x = 0; x < scr_w; x++) {
			Cell *b = &back[y * scr_w + x], *f = &front[y * scr_w + x];
			if (!full_redraw && cell_eq(b, f))
				continue;
			if (cx != x || cy != y)
				obf("\x1b[%d;%dH", y + 1, x + 1);
			if (b->fg != cur.fg || b->bg != cur.bg || b->attr != cur.attr) {
				obs("\x1b[0");
				if (b->attr & A_BOLD)
					obs(";1");
				if (b->attr & A_DIM)
					obs(";2");
				sgr_color(b->fg, 38);
				sgr_color(b->bg, 48);
				obs("m");
				cur = *b;
			}
			obs(b->ch);
			cx = x + 1;
			cy = y;
			*f = *b;
		}
	obs("\x1b[0m\x1b[?2026l");
	term_write(ob, ob_len);
	full_redraw = false;
}

static void render(void)
{
	for (int i = 0; i < scr_w * scr_h; i++)
		back[i] = (Cell){ .ch = " " };
	if (scr_w < LAYOUT_W || scr_h < LAYOUT_H) {
		char buf[64];
		snprintf(buf, sizeof buf, "termtris needs a %dx%d terminal", LAYOUT_W, LAYOUT_H);
		put(0, 0, buf, 0, 0, 0);
		pause_game();
	} else {
		bx = (scr_w - LAYOUT_W) / 2;
		by = (scr_h - LAYOUT_H) / 2;
		draw();
	}
	flush();
}

static void resize(void)
{
	term_size(&scr_w, &scr_h);
	free(back);
	free(front);
	back = calloc((size_t)scr_w * scr_h, sizeof *back);
	front = calloc((size_t)scr_w * scr_h, sizeof *front);
	if (!back || !front)
		exit(1);
	full_redraw = true;
}

/* ---- music ------------------------------------------------------------- */

/*
 * Korobeiniki, the 19th-century Russian folk song that the Game Boy turned
 * into the Tetris theme. The melody is MIDI note numbers (0 = rest) with
 * lengths in eighth notes. The folk song is only these eight bars, so the
 * cycle plays them four times, arranged differently each pass, before it
 * loops. A small pulse/triangle synth plays it the way the Game Boy's sound
 * chip did, piped as raw audio to pw-play, paplay or aplay.
 */
static const unsigned char melody[][2] = {
	{ 76, 2 }, { 71, 1 }, { 72, 1 }, { 74, 2 }, { 72, 1 }, { 71, 1 },
	{ 69, 2 }, { 69, 1 }, { 72, 1 }, { 76, 2 }, { 74, 1 }, { 72, 1 },
	{ 71, 3 }, { 72, 1 }, { 74, 2 }, { 76, 2 },
	{ 72, 2 }, { 69, 2 }, { 69, 2 }, { 0, 2 },
	{ 0, 1 }, { 74, 2 }, { 77, 1 }, { 81, 2 }, { 79, 1 }, { 77, 1 },
	{ 76, 3 }, { 72, 1 }, { 76, 2 }, { 74, 1 }, { 72, 1 },
	{ 71, 2 }, { 71, 1 }, { 72, 1 }, { 74, 2 }, { 76, 2 },
	{ 72, 2 }, { 69, 2 }, { 69, 2 }, { 0, 2 },
};
/* Per bar: the bass root (MIDI) and the chord's pitch classes (E Am E Am Dm C E Am). */
static const unsigned char bass_root[] = { 40, 45, 40, 45, 38, 36, 40, 45 };
static const unsigned char bar_chord[][3] = {
	{ 4, 8, 11 }, { 9, 0, 4 }, { 4, 8, 11 }, { 9, 0, 4 },
	{ 2, 5, 9 }, { 0, 4, 7 }, { 4, 8, 11 }, { 9, 0, 4 },
};
static const struct pass {
	signed char shift;   /* lead transposition, semitones */
	bool harmony, echo;  /* what the second voice does */
	bool walk;           /* walking bass instead of octave pumps */
} passes[] = {
	{ 0, false, false, false }, /* the plain tune */
	{ 0, true, false, true },   /* a second voice in thirds, walking bass */
	{ 0, false, true, false },  /* the second voice echoes an eighth behind */
	{ 12, true, false, true },  /* up an octave over the harmony */
};
enum {
	RATE = 22050,
	EIGHTH = RATE / 5, /* 150 beats a minute */
	SONG_EIGHTHS = 8 * (int)sizeof bass_root,
	PASSES = sizeof passes / sizeof *passes,
};

static bool music_playing;
static int music_game;

/* The nearest tone of the bar's chord at least a minor third below m. */
static int harmony_note(int m, int bar)
{
	for (int n = m - 3; n > m - 15; n--)
		for (int k = 0; k < 3; k++)
			if (n % 12 == bar_chord[bar][k])
				return n;
	return m - 12;
}

/* A plucked note: quick fade in, gentle decay, and a breath before the next. */
static double note_env(long t, long len, int fade)
{
	double env = 1.0 - 0.35 * t / len;
	if (t < fade)
		env *= (double)t / fade;
	if (len - t < 4 * fade)
		env *= (double)(len - t) / (4 * fade);
	return env;
}

/* Where the synth is in the cycle, and its oscillator and filter state. */
typedef struct {
	long pos;
	double pm, p2, pb, lp;
} Synth;

static double hz[128];
static unsigned char note_of[SONG_EIGHTHS], start_of[SONG_EIGHTHS];

static void synth_init(void)
{
	hz[69] = 440.0;
	for (int n = 70; n < 128; n++)
		hz[n] = hz[n - 1] * 1.0594630943592953;
	for (int n = 68; n >= 0; n--)
		hz[n] = hz[n + 1] / 1.0594630943592953;
	/* which melody note sounds on each eighth, and where it began */
	for (int i = 0, at = 0; at < SONG_EIGHTHS; at += melody[i++][1])
		for (int k = 0; k < melody[i][1]; k++) {
			note_of[at + k] = i;
			start_of[at + k] = at;
		}
}

/* Render the next n samples of the arranged cycle, looping at its end. */
static void synth_render(Synth *sy, int16_t *buf, int n)
{
	enum { FADE = RATE / 400, PASS = SONG_EIGHTHS * EIGHTH, CYCLE = PASSES * PASS };
	for (int k = 0; k < n; k++, sy->pos = (sy->pos + 1) % CYCLE) {
		const struct pass *ps = &passes[sy->pos / PASS];
		long pp = sy->pos % PASS, step = pp / EIGHTH;
		int bar = step / 8;
		double s = 0;

		/* lead: 25% pulse */
		int i = note_of[step], note = melody[i][0];
		long t = pp - start_of[step] * EIGHTH, len = melody[i][1] * EIGHTH;
		if (note) {
			sy->pm += hz[note + ps->shift] / RATE;
			sy->pm -= sy->pm >= 1;
			s += (sy->pm < 0.25 ? 0.75 : -0.25) * 0.16 * note_env(t, len, FADE);
		}

		/* second voice: 12.5% pulse, harmony or an echo an eighth behind */
		int note2 = 0;
		double vol2 = 0;
		if (ps->harmony && note) {
			note2 = harmony_note(note + ps->shift, bar);
			vol2 = 0.10;
		} else if (ps->echo && pp >= EIGHTH) {
			long ep = pp - EIGHTH, es = ep / EIGHTH;
			i = note_of[es];
			note2 = melody[i][0];
			t = ep - start_of[es] * EIGHTH;
			len = melody[i][1] * EIGHTH;
			vol2 = 0.07;
		}
		if (note2) {
			sy->p2 += hz[note2] / RATE;
			sy->p2 -= sy->p2 >= 1;
			s += (sy->p2 < 0.125 ? 0.875 : -0.125) * vol2 * note_env(t, len, FADE);
		}

		/* bass: triangle, octave pumps or root-fifth-octave-fifth */
		static const int walk[4] = { 0, 7, 12, 7 };
		long ts = pp % EIGHTH, on = EIGHTH * 7 / 10;
		int bn = bass_root[bar] + (ps->walk ? walk[step % 4] : 12 * (step % 2));
		sy->pb += hz[bn] / RATE;
		sy->pb -= sy->pb >= 1;
		double benv = ts < FADE ? (double)ts / FADE
			      : ts < on - FADE ? 1
			      : ts < on ? (double)(on - ts) / FADE : 0;
		double tri = sy->pb < 0.5 ? 4 * sy->pb - 1 : 3 - 4 * sy->pb;
		s += tri * 0.11 * benv;

		sy->lp += 0.45 * (s - sy->lp); /* soften the square edges */
		buf[k] = (int16_t)(sy->lp * 32767);
	}
}

/*
 * Each platform provides music_start, music_stop, music_alive (false once
 * audio is unavailable), music_play (pause or resume) and music_restart
 * (back to the top of the cycle).
 */
#ifndef _WIN32

/*
 * A synth process loops the cycle into a pipe read by pw-play, paplay or
 * aplay. SIGSTOP/SIGCONT pause it; SIGUSR1 rewinds it.
 */
static pid_t synth_pid = -1, player_pid = -1;
static volatile sig_atomic_t music_rewind;

static void on_rewind(int sig)
{
	(void)sig;
	music_rewind = 1;
}

static void write_or_die(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			_exit(0); /* the player is gone */
		p += w;
		n -= w;
	}
}

static void synth_main(int fd)
{
	int16_t buf[RATE / 50];
	Synth sy = { 0 };
	synth_init();
	for (;;) {
		if (music_rewind) {
			music_rewind = 0;
			sy.pos = 0;
		}
		synth_render(&sy, buf, sizeof buf / sizeof *buf);
		write_or_die(fd, buf, sizeof buf);
	}
}

static void music_stop(void)
{
	if (synth_pid > 0) {
		kill(synth_pid, SIGKILL);
		waitpid(synth_pid, NULL, 0);
	}
	if (player_pid > 0) {
		kill(player_pid, SIGTERM);
		waitpid(player_pid, NULL, 0);
	}
	synth_pid = player_pid = -1;
}

static void music_start(void)
{
	int p[2];
	pid_t parent = getpid();
	if (pipe(p))
		return;
	fcntl(p[1], F_SETPIPE_SZ, 4096); /* ~0.1 s of audio: pauses stop promptly */

	player_pid = fork();
	if (player_pid == 0) {
		int null = open("/dev/null", O_WRONLY);
		dup2(p[0], STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		close(p[0]);
		close(p[1]);
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			_exit(0);
		execlp("pw-play", "pw-play", "--raw", "--rate=22050", "--channels=1",
		       "--format=s16", "-", (char *)NULL);
		execlp("paplay", "paplay", "--raw", "--rate=22050", "--channels=1",
		       "--format=s16le", (char *)NULL);
		execlp("aplay", "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "22050",
		       "-c", "1", (char *)NULL);
		_exit(127);
	}
	close(p[0]);

	synth_pid = fork();
	if (synth_pid == 0) {
		int null = open("/dev/null", O_RDWR);
		dup2(null, STDIN_FILENO); /* let go of the terminal */
		dup2(null, STDOUT_FILENO);
		struct sigaction sa = { .sa_handler = on_rewind };
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR1, &sa, NULL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGHUP, SIG_DFL);
		signal(SIGWINCH, SIG_DFL);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() != parent)
			_exit(0);
		raise(SIGSTOP); /* wait for the first game */
		synth_main(p[1]);
	}
	close(p[1]);
	if (synth_pid < 0 || player_pid < 0) {
		music_stop();
		return;
	}
	int st;
	waitpid(synth_pid, &st, WUNTRACED); /* until it has stopped itself */
	atexit(music_stop);
}

static bool music_alive(void)
{
	if (synth_pid <= 0)
		return false;
	if (waitpid(player_pid, NULL, WNOHANG) == player_pid) { /* no audio player */
		player_pid = -1;
		music_stop();
		return false;
	}
	return true;
}

static void music_play(bool on) { kill(synth_pid, on ? SIGCONT : SIGSTOP); }
static void music_restart(void) { kill(synth_pid, SIGUSR1); }

#else /* _WIN32 */

/*
 * A thread keeps a few short buffers queued on the default audio device;
 * waveOutPause/waveOutRestart pause and resume it. The lock keeps a rewind
 * from racing a buffer the thread is filling.
 */
enum { WAVE_BUFS = 4, WAVE_LEN = RATE / 25 }; /* 4 x 40 ms */
static HWAVEOUT wave;
static HANDLE wave_event, wave_thread;
static CRITICAL_SECTION wave_lock;
static WAVEHDR wave_hdr[WAVE_BUFS];
static int16_t wave_buf[WAVE_BUFS][WAVE_LEN];
static Synth wave_synth;
static volatile LONG wave_quit;

static DWORD WINAPI wave_main(LPVOID arg)
{
	(void)arg;
	while (!wave_quit) {
		EnterCriticalSection(&wave_lock);
		for (int i = 0; i < WAVE_BUFS; i++)
			if (wave_hdr[i].dwFlags & WHDR_DONE) {
				synth_render(&wave_synth, wave_buf[i], WAVE_LEN);
				wave_hdr[i].dwFlags &= ~WHDR_DONE;
				waveOutWrite(wave, &wave_hdr[i], sizeof wave_hdr[i]);
			}
		LeaveCriticalSection(&wave_lock);
		WaitForSingleObject(wave_event, INFINITE);
	}
	return 0;
}

static void music_stop(void)
{
	if (!wave)
		return;
	wave_quit = 1;
	SetEvent(wave_event);
	WaitForSingleObject(wave_thread, INFINITE);
	waveOutReset(wave);
	for (int i = 0; i < WAVE_BUFS; i++)
		waveOutUnprepareHeader(wave, &wave_hdr[i], sizeof wave_hdr[i]);
	waveOutClose(wave);
	wave = NULL;
}

static void music_start(void)
{
	WAVEFORMATEX fmt = {
		.wFormatTag = WAVE_FORMAT_PCM, .nChannels = 1, .nSamplesPerSec = RATE,
		.nAvgBytesPerSec = RATE * 2, .nBlockAlign = 2, .wBitsPerSample = 16,
	};
	synth_init();
	wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (!wave_event || waveOutOpen(&wave, WAVE_MAPPER, &fmt, (DWORD_PTR)wave_event, 0,
				       CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		wave = NULL;
		return;
	}
	waveOutPause(wave); /* silent until the first game */
	InitializeCriticalSection(&wave_lock);
	for (int i = 0; i < WAVE_BUFS; i++) {
		wave_hdr[i] = (WAVEHDR){ .lpData = (LPSTR)wave_buf[i], .dwBufferLength = sizeof wave_buf[i] };
		waveOutPrepareHeader(wave, &wave_hdr[i], sizeof wave_hdr[i]);
		wave_hdr[i].dwFlags |= WHDR_DONE; /* free to fill */
	}
	wave_thread = CreateThread(NULL, 0, wave_main, NULL, 0, NULL);
	if (!wave_thread) {
		waveOutClose(wave);
		wave = NULL;
		return;
	}
	atexit(music_stop);
}

static bool music_alive(void) { return wave != NULL; }

static void music_play(bool on)
{
	if (on)
		waveOutRestart(wave);
	else
		waveOutPause(wave);
}

static void music_restart(void)
{
	EnterCriticalSection(&wave_lock);
	waveOutReset(wave); /* drop queued audio; buffers come back marked done */
	wave_synth.pos = 0;
	if (!music_playing)
		waveOutPause(wave);
	LeaveCriticalSection(&wave_lock);
	SetEvent(wave_event);
}

#endif

/* Play only while a game is running; start the tune over for each new game. */
static void music_sync(void)
{
	if (!music_alive())
		return;
	if (g.games != music_game) {
		music_game = g.games;
		music_restart();
	}
	bool want = g.music && (g.state == ST_PLAY || g.state == ST_CLEAR);
	if (want != music_playing) {
		music_play(want);
		music_playing = want;
	}
}

/* ---- terminal ---------------------------------------------------------- */

/*
 * Each platform provides term_init/term_restore, term_write, term_size,
 * term_resized, probe_terminal (theme colours; true if key releases are
 * reported), input_wait (until input or a timeout, -1 for none) and
 * input_read (false once input is gone).
 */
static bool term_active, force_basic;

static bool has_reply(const char *b, size_t n, char final)
{
	for (size_t i = 0; i + 2 < n; i++) {
		if (b[i] != 27 || b[i + 1] != '[' || b[i + 2] != '?')
			continue;
		size_t j = i + 3;
		while (j < n && (isdigit((unsigned char)b[j]) || b[j] == ';'))
			j++;
		if (j < n && b[j] == final)
			return true;
	}
	return false;
}

/* The theme queries: palette slots the pieces use, then fg and bg. */
static void wr(const char *s);
static void query_theme(void)
{
	static const int slots[] = { 1, 2, 3, 4, 5, 6, 11 };
	for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) {
		char q[16];
		snprintf(q, sizeof q, "\x1b]4;%d;?\x1b\\", slots[i]);
		wr(q);
	}
	wr("\x1b]10;?\x1b\\\x1b]11;?\x1b\\");
}

#ifndef _WIN32

static struct termios orig_tio;
static volatile sig_atomic_t got_quit, got_winch;

static void term_write(const char *s, size_t n)
{
	while (n) {
		ssize_t w = write(STDOUT_FILENO, s, n);
		if (w < 0 && errno != EINTR)
			return;
		if (w > 0) {
			s += w;
			n -= w;
		}
	}
}

static void term_restore(void)
{
	if (!term_active)
		return;
	term_active = false;
	if (g.precise)
		wr("\x1b[<u"); /* pop our keyboard flags */
	wr("\x1b[?1004l\x1b[0m\x1b[?25h\x1b[?1049l");
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_tio);
}

static void on_signal(int sig)
{
	if (sig == SIGWINCH)
		got_winch = 1;
	else
		got_quit = 1;
}

static bool is_terminal(void) { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }

static void term_init(void)
{
	struct termios raw;
	tcgetattr(STDIN_FILENO, &orig_tio);
	raw = orig_tio;
	cfmakeraw(&raw);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	term_active = true;
	atexit(term_restore);

	struct sigaction sa = { .sa_handler = on_signal }; /* no SA_RESTART: wake ppoll */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);

	/* alternate screen, hide cursor, report focus changes */
	wr("\x1b[?1049h\x1b[?25l\x1b[?1004h");
}

static void term_size(int *w, int *h)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) || !ws.ws_col || !ws.ws_row)
		ws = (struct winsize){ .ws_row = 24, .ws_col = 80 };
	*w = ws.ws_col;
	*h = ws.ws_row;
}

static bool term_resized(void)
{
	if (!got_winch)
		return false;
	got_winch = 0;
	return true;
}

static int64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static uint32_t process_id(void) { return (uint32_t)getpid(); }

/*
 * Ask for the theme colours and the kitty keyboard flags, then for the device
 * attributes that every terminal answers. Replies come back in order, so once
 * the device attributes arrive everything else that will come has come. A
 * flags reply means the keyboard protocol works; if so, switch it on.
 */
static bool probe_terminal(void)
{
	char buf[2048];
	size_t n = 0;
	int64_t deadline = now_ns() + 500000000;
	query_theme();
	wr("\x1b[?u\x1b[c");
	while (n < sizeof buf && !has_reply(buf, n, 'c')) {
		int64_t left = deadline - now_ns();
		if (left <= 0)
			break;
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		if (poll(&pfd, 1, (int)(left / 1000000) + 1) <= 0)
			continue;
		ssize_t r = read(STDIN_FILENO, buf + n, sizeof buf - n);
		if (r > 0)
			n += r;
	}
	parse_theme(buf, n);
	if (!has_reply(buf, n, 'u') || force_basic)
		return false;
	wr("\x1b[>11u"); /* disambiguate + report releases + all keys as escapes */
	return true;
}

static bool input_wait(int64_t ns)
{
	struct timespec ts = { ns / 1000000000, ns % 1000000000 };
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	return ppoll(&pfd, 1, ns < 0 ? NULL : &ts, NULL) > 0;
}

static bool input_read(void)
{
	static unsigned char in[256];
	static size_t pend;
	ssize_t n = read(STDIN_FILENO, in + pend, sizeof in - pend);
	if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
		return false; /* hung up */
	if (n > 0) {
		pend += n;
		size_t used = parse(in, pend, on_key);
		memmove(in, in + used, pend - used);
		pend -= used;
		if (pend == sizeof in) /* garbage that never completes */
			pend = 0;
	}
	return true;
}

#else /* _WIN32 */

static HANDLE hin, hout, quit_event;
static DWORD orig_in_mode, orig_out_mode;
static UINT orig_cp;
static volatile LONG got_quit;
static bool key_down[K_COUNT];

static void term_write(const char *s, size_t n)
{
	while (n) {
		DWORD w;
		if (!WriteFile(hout, s, (DWORD)n, &w, NULL) || !w)
			return;
		s += w;
		n -= w;
	}
}

static void term_restore(void)
{
	if (!term_active)
		return;
	term_active = false;
	wr("\x1b[0m\x1b[?25h\x1b[?1049l");
	SetConsoleMode(hin, orig_in_mode);
	SetConsoleMode(hout, orig_out_mode);
	SetConsoleOutputCP(orig_cp);
	timeEndPeriod(1);
}

static BOOL WINAPI on_ctrl(DWORD type)
{
	(void)type; /* Ctrl+Break, closing the window, logoff, shutdown */
	got_quit = 1;
	SetEvent(quit_event);
	return TRUE;
}

static bool is_terminal(void)
{
	DWORD mode;
	hin = GetStdHandle(STD_INPUT_HANDLE);
	hout = GetStdHandle(STD_OUTPUT_HANDLE);
	return GetConsoleMode(hin, &mode) && GetConsoleMode(hout, &mode);
}

static void term_init(void)
{
	GetConsoleMode(hin, &orig_in_mode);
	GetConsoleMode(hout, &orig_out_mode);
	if (!SetConsoleMode(hout, orig_out_mode | ENABLE_PROCESSED_OUTPUT |
					  ENABLE_VIRTUAL_TERMINAL_PROCESSING |
					  DISABLE_NEWLINE_AUTO_RETURN)) {
		fputs("termtris: this console has no VT support (needs Windows 10 or newer)\n", stderr);
		exit(1);
	}
	orig_cp = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);
	/* keys as records (Ctrl+C included), resizes reported, no quick-edit pauses */
	SetConsoleMode(hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
	quit_event = CreateEventA(NULL, TRUE, FALSE, NULL);
	SetConsoleCtrlHandler(on_ctrl, TRUE);
	timeBeginPeriod(1); /* millisecond waits for 60 Hz frames */
	term_active = true;
	atexit(term_restore);
	wr("\x1b[?1049h\x1b[?25l");
}

static void term_size(int *w, int *h)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (!GetConsoleScreenBufferInfo(hout, &info)) {
		*w = 80;
		*h = 24;
		return;
	}
	*w = info.srWindow.Right - info.srWindow.Left + 1;
	*h = info.srWindow.Bottom - info.srWindow.Top + 1;
}

static bool term_resized(void)
{
	int w, h;
	term_size(&w, &h);
	return w != scr_w || h != scr_h;
}

static int64_t now_ns(void)
{
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return c.QuadPart / f.QuadPart * 1000000000 + c.QuadPart % f.QuadPart * 1000000000 / f.QuadPart;
}

static uint32_t process_id(void) { return (uint32_t)GetCurrentProcessId(); }

/*
 * The console answers queries by injecting the reply as typed characters, so
 * collect those until the device-attributes reply arrives. Key releases are
 * not probed: the first one that arrives switches precise keys on.
 */
static bool probe_terminal(void)
{
	char buf[2048];
	size_t n = 0;
	int64_t deadline = now_ns() + 500000000;
	SetConsoleMode(hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT);
	query_theme();
	wr("\x1b[c");
	while (n + 4 < sizeof buf && !has_reply(buf, n, 'c')) {
		int64_t left = deadline - now_ns();
		if (left <= 0 || WaitForSingleObject(hin, (DWORD)(left / 1000000) + 1) != WAIT_OBJECT_0)
			break;
		INPUT_RECORD rec[64];
		DWORD got = 0;
		if (!ReadConsoleInputW(hin, rec, 64, &got))
			break;
		for (DWORD i = 0; i < got && n + 4 < sizeof buf; i++) {
			KEY_EVENT_RECORD *ke = &rec[i].Event.KeyEvent;
			if (rec[i].EventType == KEY_EVENT && ke->bKeyDown && ke->uChar.UnicodeChar &&
			    ke->uChar.UnicodeChar < 128)
				buf[n++] = (char)ke->uChar.UnicodeChar;
		}
	}
	SetConsoleMode(hin, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
	parse_theme(buf, n);
	return false;
}

static int key_for_vk(WORD vk, bool ctrl)
{
	switch (vk) {
	case VK_LEFT: return K_LEFT;
	case VK_RIGHT: return K_RIGHT;
	case VK_DOWN: return K_DOWN;
	case VK_UP: return K_CW;
	case VK_SPACE: return K_DROP;
	case VK_RETURN: return K_ENTER;
	case VK_ESCAPE: return K_PAUSE;
	}
	if (vk >= 'A' && vk <= 'Z')
		return key_for_char(vk - 'A' + 'a', ctrl);
	return K_NONE;
}

/*
 * One console input record. Consoles that report key releases (conhost,
 * Windows Terminal) get precise keys from the first release on; until then,
 * or with --basic-keys, every press and OS repeat counts as a press.
 */
static void handle_record(const INPUT_RECORD *rec)
{
	if (rec->EventType == FOCUS_EVENT && !rec->Event.FocusEvent.bSetFocus) {
		memset(key_down, 0, sizeof key_down);
		on_key(K_FOCUS_OUT, EV_PRESS);
		return;
	}
	if (rec->EventType != KEY_EVENT)
		return;
	const KEY_EVENT_RECORD *ke = &rec->Event.KeyEvent;
	bool ctrl = ke->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
	int k = key_for_vk(ke->wVirtualKeyCode, ctrl);
	if (!ke->bKeyDown) {
		if (!force_basic)
			g.precise = true;
		if (k && key_down[k]) {
			key_down[k] = false;
			on_key(k, EV_RELEASE);
		}
		return;
	}
	if (!k)
		return;
	if (!g.precise) {
		for (WORD r = 0; r < (ke->wRepeatCount ? ke->wRepeatCount : 1); r++)
			on_key(k, EV_PRESS);
		key_down[k] = true;
		return;
	}
	on_key(k, key_down[k] ? EV_REPEAT : EV_PRESS);
	key_down[k] = true;
}

static bool input_wait(int64_t ns)
{
	HANDLE hs[2] = { hin, quit_event };
	DWORD ms = ns < 0 ? INFINITE : (DWORD)((ns + 999999) / 1000000);
	return WaitForMultipleObjects(2, hs, FALSE, ms) == WAIT_OBJECT_0;
}

static bool input_read(void)
{
	INPUT_RECORD rec[64];
	DWORD got = 0;
	if (!ReadConsoleInputW(hin, rec, 64, &got))
		return false;
	for (DWORD i = 0; i < got; i++)
		handle_record(&rec[i]);
	return true;
}

#endif

static void wr(const char *s) { term_write(s, strlen(s)); }

static void run(void)
{
	int64_t next = now_ns();
	bool dirty = true;

	while (!got_quit && !g.quit) {
		if (term_resized()) {
			resize();
			dirty = true;
		}
		if (dirty)
			render();
		dirty = false;
		music_sync();

		/* idle screens sleep until a key; live ones until the next frame */
		bool was_live = g.state == ST_PLAY || g.state == ST_CLEAR;
		int64_t wait = -1;
		if (was_live) {
			wait = next - now_ns();
			wait = wait < 0 ? 0 : wait;
		}
		if (input_wait(wait)) {
			if (!input_read())
				break;
			dirty = true;
		}

		bool live = g.state == ST_PLAY || g.state == ST_CLEAR;
		int64_t t = now_ns();
		if (live && !was_live)
			next = t + FRAME_NS;
		if (live) {
			if (t - next > 250000000) /* stalled: don't fast-forward */
				next = t;
			while (live && t >= next) {
				dirty |= tick();
				next += FRAME_NS;
				live = g.state == ST_PLAY || g.state == ST_CLEAR;
			}
		}
	}
}

#ifndef TERMTRIS_NO_MAIN
int main(int argc, char **argv)
{
	g.music = true;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--basic-keys")) {
			force_basic = true;
		} else if (!strcmp(argv[i], "--no-music")) {
			g.music = false;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
			puts("termtris " TERMTRIS_VERSION);
			return 0;
		} else {
			bool help = !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help");
			fprintf(help ? stdout : stderr,
				"usage: termtris [--basic-keys] [--no-music] [--version]\n"
				"  --basic-keys  ignore key releases and use plain key presses\n"
				"  --no-music    start with the music off (m toggles it)\n"
				"  --version     print the version and exit\n");
			return help ? 0 : 2;
		}
	}
	if (!is_terminal()) {
		fputs("termtris: needs a terminal\n", stderr);
		return 1;
	}

	init_shapes();
	g.rng = (uint32_t)now_ns() ^ process_id() << 16;
	if (!g.rng)
		g.rng = 1;
	g.preview = true;
	load_hiscore();

	term_init();
	g.precise = probe_terminal();
	init_colors();
	music_start();
	resize();
	run();
	return 0;
}
#endif
