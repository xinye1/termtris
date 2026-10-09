/* Headless tests for the game rules, the input parsers and the synth. */
#define TERMTRIS_NO_MAIN
#include "termtris.c"

/* The high score lives under $XDG_DATA_HOME, or %APPDATA% on Windows. */
#ifdef _WIN32
#include <io.h>
#define DATA_VAR "APPDATA"
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
static void remove_path(const char *p)
{
	if (remove(p))
		_rmdir(p);
}
static bool make_temp_dir(char *buf, size_t n)
{
	const char *tmp = getenv("TEMP");
	snprintf(buf, n, "%s\\termtris-test-XXXXXX", tmp ? tmp : ".");
	return !_mktemp_s(buf, strlen(buf) + 1) && !_mkdir(buf);
}
#else
#define DATA_VAR "XDG_DATA_HOME"
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
static void remove_path(const char *p) { remove(p); } /* files and empty folders */
static bool make_temp_dir(char *buf, size_t n)
{
	snprintf(buf, n, "/tmp/termtris-test-XXXXXX");
	return mkdtemp(buf) != NULL;
}
#endif

static int fails, checks;
#define CHECK(c)                                                              \
	do {                                                                  \
		checks++;                                                     \
		if (!(c)) {                                                   \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
			fails++;                                              \
		}                                                             \
	} while (0)

/* Fresh game with `piece` in play and an O queued next. */
static void setup(int piece, int level)
{
	memset(&g, 0, sizeof g);
	g.rng = 12345;
	g.precise = true;
	g.start_level = g.level = level;
	g.last_roll = -1;
	g.next = piece;
	spawn();
	g.next = PO;
}

static void ticks(int n)
{
	while (n--)
		tick();
}

static bool has_cell(int piece, int rot, int x, int y)
{
	for (int c = 0; c < 4; c++)
		if (shape[piece][rot][c].x == x && shape[piece][rot][c].y == y)
			return true;
	return false;
}

static void fill_row(int y, int gap_from, int gap_to)
{
	for (int x = 0; x < W; x++)
		g.board[y][x] = x >= gap_from && x <= gap_to ? 0 : PZ + 1;
}

static void test_shapes(void)
{
	for (int p = 0; p < NPIECE; p++)
		for (int r = 0; r < nrot[p]; r++)
			for (int a = 0; a < 4; a++)
				for (int b = a + 1; b < 4; b++)
					CHECK(shape[p][r][a].x != shape[p][r][b].x ||
					      shape[p][r][a].y != shape[p][r][b].y);
	/* T spawns pointing down; one clockwise turn points it left. */
	CHECK(has_cell(PT, 0, 0, 1));
	CHECK(has_cell(PT, 1, -1, 0) && has_cell(PT, 1, 0, -1) && has_cell(PT, 1, 0, 1));
	/* J's foot swings from bottom-right to bottom-left. */
	CHECK(has_cell(PJ, 0, 1, 1) && has_cell(PJ, 1, -1, 1));
	/* Vertical I sits in the pivot column. */
	for (int c = 0; c < 4; c++)
		CHECK(shape[PI][1][c].x == 0);
}

static void test_spawn_and_gravity(void)
{
	setup(PT, 0);
	CHECK(g.state == ST_PLAY && g.px == 5 && g.py == 0);
	ticks(47);
	CHECK(g.py == 0);
	ticks(1);
	CHECK(g.py == 1);

	setup(PT, 19);
	ticks(2);
	CHECK(g.py == 1);
	setup(PT, 29);
	ticks(1);
	CHECK(g.py == 1);
}

static void test_das(void)
{
	setup(PT, 0);
	g.px = 1;
	on_key(K_RIGHT, EV_PRESS);
	CHECK(g.px == 2); /* immediate */
	ticks(DAS - 1);
	CHECK(g.px == 2);
	ticks(1);
	CHECK(g.px == 3); /* auto-shift starts */
	ticks(ARR - 1);
	CHECK(g.px == 3);
	ticks(1);
	CHECK(g.px == 4);
	ticks(ARR * 4);
	CHECK(g.px == 8);
	on_key(K_RIGHT, EV_REPEAT); /* OS repeats are ignored */
	on_key(K_RIGHT, EV_RELEASE);
	g.px = 5;
	ticks(20);
	CHECK(g.px == 5);

	/* Against the wall it stops, and the other direction takes over on release. */
	on_key(K_LEFT, EV_PRESS);
	on_key(K_RIGHT, EV_PRESS);
	ticks(40);
	CHECK(g.px == W - 2);
	on_key(K_RIGHT, EV_RELEASE);
	CHECK(g.held_dir == -1);
	ticks(DAS + ARR * 20);
	CHECK(g.px == 1);

	/* Holding through a line clear keeps the charge: the next piece slides at once. */
	setup(PI, 0);
	fill_row(H - 1, 3, 6);
	on_key(K_DROP, EV_PRESS);
	on_key(K_RIGHT, EV_PRESS);
	ticks(CLEAR_FRAMES);
	CHECK(g.state == ST_PLAY && g.px == 5);
	ticks(1);
	CHECK(g.px == 6);

	/* Basic keys: every press shifts once and nothing repeats on its own. */
	setup(PT, 0);
	g.precise = false;
	on_key(K_RIGHT, EV_PRESS);
	ticks(30);
	CHECK(g.px == 6 && g.held_dir == 0);
}

static void test_rotation_has_no_kicks(void)
{
	setup(PI, 0);
	on_key(K_CW, EV_PRESS);
	CHECK(g.rot == 1);
	g.px = 0;
	on_key(K_CW, EV_PRESS); /* horizontal would poke out at x = -2 */
	CHECK(g.rot == 1 && g.px == 0);

	setup(PO, 0);
	on_key(K_CW, EV_PRESS);
	CHECK(g.rot == 0);

	setup(PT, 0);
	on_key(K_CCW, EV_PRESS);
	CHECK(g.rot == 3);
}

static void test_single_and_tetris(void)
{
	setup(PI, 0);
	fill_row(H - 1, 3, 6);
	on_key(K_DROP, EV_PRESS);
	CHECK(g.state == ST_CLEAR && g.nclear == 1);
	CHECK(g.score == 2 * (H - 1));
	ticks(CLEAR_FRAMES);
	CHECK(g.state == ST_PLAY && g.lines == 1);
	CHECK(g.score == 2 * (H - 1) + 40);
	for (int x = 0; x < W; x++)
		CHECK(g.board[H - 1][x] == 0);

	setup(PI, 3);
	for (int y = H - 4; y < H; y++)
		fill_row(y, 9, 9);
	g.board[H - 5][0] = PT + 1; /* a cell above the stack must drop 4 rows */
	on_key(K_CW, EV_PRESS);
	g.px = 9;
	g.score = 0;
	on_key(K_DROP, EV_PRESS);
	CHECK(g.nclear == 4);
	ticks(CLEAR_FRAMES);
	CHECK(g.score == 2 * (H - 2) + 1200 * 4);
	CHECK(g.board[H - 1][0] == PT + 1);
}

static void test_levels(void)
{
	setup(PI, 0);
	g.lines = 9;
	fill_row(H - 1, 3, 6);
	on_key(K_DROP, EV_PRESS);
	ticks(CLEAR_FRAMES);
	CHECK(g.level == 1);

	setup(PI, 5); /* a level-5 start stays at 5 until 60 lines */
	g.lines = 50;
	fill_row(H - 1, 3, 6);
	on_key(K_DROP, EV_PRESS);
	ticks(CLEAR_FRAMES);
	CHECK(g.level == 5);
	setup(PI, 5);
	g.lines = 59;
	fill_row(H - 1, 3, 6);
	on_key(K_DROP, EV_PRESS);
	ticks(CLEAR_FRAMES);
	CHECK(g.level == 6);
}

static void test_soft_drop(void)
{
	setup(PT, 0);
	on_key(K_DOWN, EV_PRESS);
	CHECK(g.py == 1);
	ticks(SOFT * 3);
	CHECK(g.py == 4);
	/* Hold through the lock: the next piece must not inherit the drop. */
	ticks(SOFT * H);
	CHECK(g.piece == PO && g.py == 0 && g.soft_held);
	ticks(SOFT * 4);
	CHECK(g.py == 0);
	CHECK(g.score == H - 2); /* one point per soft-dropped row */
	on_key(K_DOWN, EV_PRESS);
	ticks(SOFT);
	CHECK(g.py == 2);
	on_key(K_DOWN, EV_RELEASE);
	ticks(SOFT * 4);
	CHECK(g.py == 2);

	/* at levels 19-28 gravity matches soft drop, which must still score */
	setup(PT, 19);
	on_key(K_DOWN, EV_PRESS);
	ticks(SOFT * 4);
	CHECK(g.py == 5 && g.soft_rows == 5);
}

static void test_game_over(void)
{
	setup(PT, 0);
	for (int y = 1; y < H; y++)
		fill_row(y, 0, 0); /* no full rows, but no room to spawn */
	g.next = PT;
	g.score = 1;
	g.hiscore = 1000000;
	on_key(K_DROP, EV_PRESS);
	CHECK(g.state == ST_OVER && !g.new_high);
	on_key(K_DROP, EV_PRESS);
	CHECK(g.state == ST_OVER);
	on_key(K_RESTART, EV_PRESS);
	CHECK(g.state == ST_TITLE);
}

static void test_pause_and_focus(void)
{
	setup(PT, 0);
	on_key(K_LEFT, EV_PRESS);
	on_key(K_FOCUS_OUT, EV_PRESS);
	CHECK(g.state == ST_PAUSE && g.held_dir == 0);
	ticks(200);
	CHECK(g.py == 0);
	on_key(K_PAUSE, EV_PRESS);
	CHECK(g.state == ST_PLAY);
}

static void test_randomizer(void)
{
	int seen[NPIECE] = { 0 }, repeats = 0, prev = -1;
	bool in_range = true;
	g.rng = 99;
	g.last_roll = -1;
	for (int i = 0; i < 70000; i++) {
		int r = roll();
		if (r < 0 || r >= NPIECE) {
			in_range = false;
			break;
		}
		seen[r]++;
		repeats += r == prev;
		prev = r;
	}
	CHECK(in_range);
	for (int p = 0; p < NPIECE; p++)
		CHECK(seen[p] > 8000);
	CHECK(repeats > 1000 && repeats < 4000); /* NES: about 1 in 28 */

	/* same seed, same sequence: the other tests rely on it */
	int seq[64];
	bool same = true;
	g.rng = 4242;
	g.last_roll = -1;
	for (int i = 0; i < 64; i++)
		seq[i] = roll();
	g.rng = 4242;
	g.last_roll = -1;
	for (int i = 0; i < 64; i++)
		same &= roll() == seq[i];
	CHECK(same);
}

static int evs[32][2], nev;
static void collect(int k, int ev)
{
	evs[nev][0] = k;
	evs[nev++][1] = ev;
}

static size_t feed(const char *s)
{
	nev = 0;
	return parse((const unsigned char *)s, strlen(s), collect);
}

static void test_parser(void)
{
	feed("\x1b[D");
	CHECK(nev == 1 && evs[0][0] == K_LEFT && evs[0][1] == EV_PRESS);
	feed("\x1bOC");
	CHECK(nev == 1 && evs[0][0] == K_RIGHT);
	feed("\x1b[1;1:3D\x1b[1;1:2B");
	CHECK(nev == 2 && evs[0][0] == K_LEFT && evs[0][1] == EV_RELEASE);
	CHECK(evs[1][0] == K_DOWN && evs[1][1] == EV_REPEAT);
	feed("\x1b[113u\x1b[99;5u\x1b[99u\x1b[32;1:3u\x1b[13u\x1b[27u");
	CHECK(nev == 6);
	CHECK(evs[0][0] == K_QUIT && evs[1][0] == K_QUIT && evs[2][0] == K_STYLE);
	CHECK(evs[3][0] == K_DROP && evs[3][1] == EV_RELEASE);
	CHECK(evs[4][0] == K_ENTER && evs[5][0] == K_PAUSE);
	feed("\x1b[57441u\x1b[?11u\x1b[?62;22c"); /* Shift itself and query replies */
	CHECK(nev == 0);
	feed("hjkl xzq\x03\r");
	CHECK(nev == 10 && evs[0][0] == K_LEFT && evs[2][0] == K_CW && evs[8][0] == K_QUIT);
	g.precise = false;
	feed("\x1b");
	CHECK(nev == 1 && evs[0][0] == K_PAUSE);
	g.precise = true; /* Esc is CSI 27 u here, so a bare Esc is a split read */
	CHECK(feed("\x1b") == 0 && nev == 0);
	CHECK(feed("x\x1b") == 1 && nev == 1);
	feed("\x1b[O\x1b[I");
	CHECK(nev == 1 && evs[0][0] == K_FOCUS_OUT);
	CHECK(feed("x\x1b[1;1:") == 1); /* an unfinished sequence waits for more */
	CHECK(nev == 1);
	/* late theme replies (OSC) are swallowed whole, with either terminator */
	feed("\x1b]4;1;rgb:cccc/2424/1d1d\x1b\\\x1b]11;rgb:28/28/28\x07q");
	CHECK(nev == 1 && evs[0][0] == K_QUIT);
	CHECK(feed("h\x1b]4;1;rgb:cc") == 1 && nev == 1); /* unfinished: wait */
	CHECK(feed("\x1b]10;rgb:ebeb/dbdb/b2b2\x1b") == 0 && nev == 0);
}

static void test_hiscore(const char *dir)
{
	char xdg[512];
	snprintf(xdg, sizeof xdg, "%s/no/such/share", dir); /* parents missing */
	set_env(DATA_VAR, xdg);
	g.hiscore = 4321;
	save_hiscore();
	g.hiscore = 0;
	load_hiscore();
	CHECK(g.hiscore == 4321);

	/* tidy up, deepest first */
	static const char *const made[] = {
		"/no/such/share/termtris/highscore", "/no/such/share/termtris",
		"/no/such/share", "/no/such", "/no",
	};
	for (size_t i = 0; i < sizeof made / sizeof *made; i++) {
		char path[600];
		snprintf(path, sizeof path, "%s%s", dir, made[i]);
		remove_path(path);
	}
	set_env(DATA_VAR, dir);
}

static void test_song(void)
{
	int eighths = 0;
	for (size_t i = 0; i < sizeof melody / sizeof *melody; i++) {
		eighths += melody[i][1];
		CHECK(melody[i][1] > 0);
		CHECK(!melody[i][0] || (melody[i][0] >= 57 && melody[i][0] <= 84));
	}
	CHECK(eighths == SONG_EIGHTHS); /* melody and bass stay in step */
	/* no note straddles a bar line */
	int at = 0;
	for (size_t i = 0; i < sizeof melody / sizeof *melody; i++) {
		CHECK(at / 8 == (at + melody[i][1] - 1) / 8);
		at += melody[i][1];
	}
}

static void test_arrangement(void)
{
	CHECK(PASSES >= 4);
	for (int pi = 0; pi < PASSES; pi++) {
		int at = 0;
		for (size_t i = 0; i < sizeof melody / sizeof *melody; i++) {
			int m = melody[i][0] + passes[pi].shift, bar = at / 8;
			at += melody[i][1];
			if (!melody[i][0])
				continue;
			CHECK(m < 128);
			int h = harmony_note(m, bar), pc = h % 12;
			/* a chord tone, between a minor third and an octave below */
			CHECK(m - h >= 3 && m - h <= 12);
			CHECK(pc == bar_chord[bar][0] || pc == bar_chord[bar][1] ||
			      pc == bar_chord[bar][2]);
		}
	}
	CHECK(harmony_note(76, 0) == 71); /* E5 over E major: B4 */
	CHECK(harmony_note(69, 1) == 64); /* A4 over A minor: E4 */
}

static void test_theme(void)
{
	/* kitty-style 4-digit replies with ST, and 2-digit ones with BEL */
	const char *r = "\x1b]4;1;rgb:cccc/2424/1d1d\x1b\\"
			"\x1b]4;2;rgb:98/97/1a\x07"
			"\x1b]4;3;rgb:d7d7/9999/2121\x1b\\"
			"\x1b]4;4;rgb:4545/8585/8888\x1b\\"
			"\x1b]4;5;rgb:b1b1/6262/8686\x1b\\"
			"\x1b]4;6;rgb:6868/9d9d/6a6a\x1b\\"
			"\x1b]4;11;rgb:fafa/bdbd/2f2f\x1b\\"
			"\x1b]10;rgb:ebeb/dbdb/b2b2\x1b\\"
			"\x1b]11;rgb:2828/2828/2828\x1b\\"
			"\x1b]4;200;rgb:ffff/0000/0000\x1b\\" /* out of range: ignored */
			"\x1b]4;7;rgb:zz/00/00\x1b\\";        /* malformed: ignored */
	theme_have = 0;
	parse_theme(r, strlen(r));
	CHECK(theme_rgb[1] == 0xcc241d && theme_rgb[2] == 0x98971a);
	CHECK(theme_rgb[TH_FG] == 0xebdbb2 && theme_rgb[TH_BG] == 0x282828);
	CHECK(!have(7) && have(11));
	init_colors();
	CHECK(shaded);
	CHECK(base_rgb[PL] == mix(0xcc241d, 0xd79921, 50));
	CHECK(dot_color == RGB24(mix(0x282828, 0xebdbb2, 28)));
	CHECK(mix(0xffffff, 0, 50) == 0x808080 && darken(0x646464, 50) == 0x323232);

	/* nothing reported: flat palette colours, no shading */
	theme_have = 0;
	dot_color = PAL(8);
	init_colors();
	CHECK(!shaded && dot_color == PAL(8));
}

static void test_synth(void)
{
	enum { N = 4096 };
	static int16_t a[N], b[N];
	Synth s1 = { 0 }, s2 = { 0 };
	synth_init();
	synth_render(&s1, a, N);
	synth_render(&s2, b, N);
	CHECK(!memcmp(a, b, sizeof a)); /* a rewind replays the same sound */
	int peak = 0;
	for (int i = 0; i < N; i++)
		peak = abs(a[i]) > peak ? abs(a[i]) : peak;
	CHECK(peak > 1000 && peak < 20000); /* audible, far from clipping */
	long cycle = (long)PASSES * SONG_EIGHTHS * EIGHTH;
	s1.pos = cycle - 10;
	synth_render(&s1, a, 20);
	CHECK(s1.pos == 10); /* loops back to the top */
}

#ifdef _WIN32
static INPUT_RECORD key_rec(WORD vk, bool down, DWORD state)
{
	INPUT_RECORD r = { .EventType = KEY_EVENT };
	r.Event.KeyEvent.bKeyDown = down;
	r.Event.KeyEvent.wRepeatCount = 1;
	r.Event.KeyEvent.wVirtualKeyCode = vk;
	r.Event.KeyEvent.dwControlKeyState = state;
	return r;
}

static void feed_key(WORD vk, bool down, DWORD state)
{
	INPUT_RECORD r = key_rec(vk, down, state);
	handle_record(&r);
}

static void test_windows_keys(void)
{
	setup(PT, 0);
	g.precise = false;
	memset(key_down, 0, sizeof key_down);
	/* before any release arrives, presses and OS repeats each move once */
	feed_key(VK_RIGHT, true, 0);
	feed_key(VK_RIGHT, true, 0);
	CHECK(g.px == 7 && !g.precise && g.held_dir == 0);
	/* the first release switches precise keys on */
	feed_key(VK_RIGHT, false, 0);
	CHECK(g.precise);
	/* now holding auto-shifts on the game's timing; OS repeats are ignored */
	feed_key(VK_LEFT, true, 0);
	feed_key(VK_LEFT, true, 0);
	CHECK(g.px == 6 && g.held_dir == -1);
	ticks(DAS);
	CHECK(g.px == 5);
	feed_key(VK_LEFT, false, 0);
	CHECK(g.held_dir == 0);
	/* letters map like the Unix keys; Ctrl+C quits */
	feed_key('G', true, 0);
	CHECK(g.ghost);
	feed_key('C', true, LEFT_CTRL_PRESSED);
	CHECK(g.quit);

	/* losing focus pauses and forgets held keys */
	setup(PT, 0);
	memset(key_down, 0, sizeof key_down);
	feed_key(VK_LEFT, true, 0);
	INPUT_RECORD f = { .EventType = FOCUS_EVENT };
	f.Event.FocusEvent.bSetFocus = FALSE;
	handle_record(&f);
	CHECK(g.state == ST_PAUSE && g.held_dir == 0 && !key_down[K_LEFT]);

	/* --basic-keys never switches to precise keys */
	setup(PT, 0);
	g.precise = false;
	force_basic = true;
	feed_key(VK_RIGHT, false, 0);
	CHECK(!g.precise);
	force_basic = false;
}

static void feed_char(wchar_t c)
{
	INPUT_RECORD r = key_rec(c >= 'a' && c <= 'z' ? (WORD)(c - 'a' + 'A') : c == 27 ? VK_ESCAPE : 0,
				 true, 0);
	r.Event.KeyEvent.uChar.UnicodeChar = c;
	handle_record(&r);
}

static void feed_text(const char *s)
{
	while (*s)
		feed_char((unsigned char)*s++);
}

static void replies_pending(int64_t ns)
{
	replies_done = false;
	reply_state = 0;
	replies_deadline = now_ns() + ns;
}

static void test_windows_late_replies(void)
{
	/* late colour replies are swallowed, not read as r/g/c key presses */
	setup(PT, 0);
	memset(key_down, 0, sizeof key_down);
	replies_pending(5000000000LL);
	feed_text("\x1b]4;1;rgb:cccc/2424/1d1d\x1b\\");
	feed_text("\x1b]11;rgb:28/28/28\x07");
	CHECK(!g.ghost && !g.classic && g.state == ST_PLAY);
	/* the device-attributes reply is the last; keys work normally after it */
	feed_text("\x1b[?62;22c");
	CHECK(replies_done);
	feed_char('g');
	CHECK(g.ghost);

	/* a real Esc still pauses, once the next key shows it began no reply */
	setup(PT, 0);
	memset(key_down, 0, sizeof key_down);
	replies_pending(5000000000LL);
	feed_char(27);
	CHECK(g.state == ST_PLAY); /* held back */
	feed_key(VK_LEFT, true, 0);
	CHECK(g.state == ST_PAUSE);

	/*
	 * replies cut off by the probe deadline (mid-OSC and mid-CSI) are still
	 * recognised when their rest arrives as typed characters
	 */
	static const char *const cut[][2] = {
		{ "\x1b]4;1;rgb:ccc", "c/2424/1d1d\x1b\\\x1b[?62;22c" }, /* one c: a toggle shows */
		{ "\x1b[?6", "2;22c" },
	};
	for (size_t i = 0; i < sizeof cut / sizeof *cut; i++) {
		char buf[64];
		size_t n = 0;
		setup(PT, 0);
		memset(key_down, 0, sizeof key_down);
		reply_state = 0;
		for (const char *p = cut[i][0]; *p; p++)
			probe_char(buf, &n, (unsigned char)*p);
		probe_done(buf, n);
		CHECK(!replies_done);
		feed_text(cut[i][1]);
		CHECK(!g.classic && replies_done); /* the 'c's were reply, not keys */
	}

	/* after the deadline the filter stops */
	setup(PT, 0);
	memset(key_down, 0, sizeof key_down);
	replies_pending(-1);
	feed_char('g');
	CHECK(g.ghost && replies_done);
}
#endif

int main(void)
{
	char dir[512];
	if (!make_temp_dir(dir, sizeof dir))
		return 1;
	set_env(DATA_VAR, dir); /* keep the real high score untouched */
	init_shapes();

	test_shapes();
	test_spawn_and_gravity();
	test_das();
	test_rotation_has_no_kicks();
	test_single_and_tetris();
	test_levels();
	test_soft_drop();
	test_game_over();
	test_pause_and_focus();
	test_randomizer();
	test_parser();
	test_song();
	test_hiscore(dir);
	test_arrangement();
	test_theme();
	test_synth();
#ifdef _WIN32
	test_windows_keys();
	test_windows_late_replies();
#endif

	remove_path(dir);
	printf("%d/%d checks passed\n", checks - fails, checks);
	return fails != 0;
}
