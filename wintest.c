/*
 * Plays termtris.exe in a real Windows console, as CI's stand-in for a
 * person: starts it in a new console, types keys into that console (key
 * releases included), reads the screen back and checks what the game drew.
 *
 *   wintest path\to\termtris.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

enum { MAX_ROWS = 100, MAX_COLS = 300 };
static HANDLE con_in, con_out;
static wchar_t screen[MAX_ROWS][MAX_COLS + 1];
static int rows, cols, fails;

static void key(WORD vk, wchar_t ch, bool down)
{
	INPUT_RECORD r = { .EventType = KEY_EVENT };
	r.Event.KeyEvent.bKeyDown = down;
	r.Event.KeyEvent.wRepeatCount = 1;
	r.Event.KeyEvent.wVirtualKeyCode = vk;
	r.Event.KeyEvent.wVirtualScanCode = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	r.Event.KeyEvent.uChar.UnicodeChar = ch;
	DWORD n;
	WriteConsoleInputW(con_in, &r, 1, &n);
}

static void tap(WORD vk, wchar_t ch)
{
	key(vk, ch, true);
	Sleep(40);
	key(vk, ch, false);
	Sleep(200);
}

/* Read the visible window into screen[]. */
static void grab(void)
{
	CONSOLE_SCREEN_BUFFER_INFO info;
	if (!GetConsoleScreenBufferInfo(con_out, &info))
		return;
	rows = info.srWindow.Bottom - info.srWindow.Top + 1;
	cols = info.srWindow.Right - info.srWindow.Left + 1;
	rows = rows > MAX_ROWS ? MAX_ROWS : rows;
	cols = cols > MAX_COLS ? MAX_COLS : cols;
	for (int r = 0; r < rows; r++) {
		DWORD n = 0;
		COORD at = { info.srWindow.Left, (SHORT)(info.srWindow.Top + r) };
		ReadConsoleOutputCharacterW(con_out, screen[r], cols, at, &n);
		screen[r][n] = 0;
	}
}

static bool has(const wchar_t *s)
{
	for (int r = 0; r < rows; r++)
		if (wcsstr(screen[r], s))
			return true;
	return false;
}

/* Poll the screen until it shows s, for up to ms. */
static bool wait_for(const wchar_t *s, int ms)
{
	for (int t = 0; t <= ms; t += 50) {
		grab();
		if (has(s))
			return true;
		Sleep(50);
	}
	return false;
}

static void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	fflush(stdout);
	fails += !ok;
}

/* The well: row and column of its top-left "<!". */
static bool find_well(int *wr, int *wc)
{
	for (int r = 0; r < rows; r++) {
		wchar_t *p = wcsstr(screen[r], L"<!");
		if (p) {
			*wr = r;
			*wc = (int)(p - screen[r]);
			return true;
		}
	}
	return false;
}

/* Leftmost and rightmost screen columns holding a brick ("[]") in the well. */
static bool brick_cols(int wr, int wc, int *lo, int *hi)
{
	*lo = MAX_COLS;
	*hi = -1;
	for (int r = wr; r < wr + 20 && r < rows; r++)
		for (int c = wc + 2; c < wc + 22 && c < cols; c += 2)
			if (screen[r][c] == L'[') {
				*lo = c < *lo ? c : *lo;
				*hi = c > *hi ? c : *hi;
			}
	return *hi >= 0;
}

static int number_under(const wchar_t *label)
{
	for (int r = 0; r + 1 < rows; r++) {
		wchar_t *p = wcsstr(screen[r], label);
		if (p)
			return (int)wcstol(screen[r + 1] + (p - screen[r]), NULL, 10);
	}
	return -1;
}

static void dump(void)
{
	grab();
	puts("--- screen ---");
	for (int r = 0; r < rows; r++) {
		char line[MAX_COLS * 4 + 1];
		int n = WideCharToMultiByte(CP_UTF8, 0, screen[r], -1, line, sizeof line, NULL, NULL);
		if (n > 0)
			puts(line);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fputs("usage: wintest path\\to\\termtris.exe\n", stderr);
		return 2;
	}
	char cmd[MAX_PATH + 32];
	snprintf(cmd, sizeof cmd, "\"%s\" --no-music", argv[1]);
	STARTUPINFOA si = { .cb = sizeof si };
	PROCESS_INFORMATION pi;
	if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
		printf("FAIL could not start %s (error %lu)\n", argv[1], GetLastError());
		return 1;
	}
	Sleep(1500); /* startup and the colour probe */

	FreeConsole();
	if (!AttachConsole(pi.dwProcessId)) {
		printf("FAIL could not attach to the game's console (error %lu)\n", GetLastError());
		TerminateProcess(pi.hProcess, 1);
		return 1;
	}
	con_in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			     NULL, OPEN_EXISTING, 0, NULL);
	con_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			      NULL, OPEN_EXISTING, 0, NULL);

	/* title screen: no key released yet, so basic keys */
	check(wait_for(L"SELECT LEVEL", 3000), "title screen drawn");
	check(has(L"<!") && has(L"!>") && has(L"\\/\\/"), "the 1984 well is drawn");
	check(has(L"basic keys"), "basic keys before any key release");

	/* the first release switches precise keys on; Right picks level 1 */
	tap(VK_RIGHT, 0);
	check(wait_for(L"<  1  >", 2000), "Right changes the start level");
	check(wait_for(L"precise keys", 2000), "a key release switches precise keys on");

	tap('C', L'c'); /* classic [] bricks, easy to find on screen */
	tap(VK_SPACE, L' ');
	check(wait_for(L"SCORE", 2000) && !has(L"SELECT LEVEL"), "Space starts a game");

	int wr = 0, wc = 0, lo, hi;
	grab();
	bool well = find_well(&wr, &wc);
	check(well && brick_cols(wr, wc, &lo, &hi) && hi < wc + 20, "a piece spawns away from the wall");

	/* hold Right with one key-down and no repeats: the game's own auto-shift */
	key(VK_RIGHT, 0, true);
	Sleep(800);
	key(VK_RIGHT, 0, false);
	Sleep(100);
	grab();
	check(well && brick_cols(wr, wc, &lo, &hi) && hi == wc + 20,
	      "holding Right slides the piece to the wall without OS repeats");

	tap('P', L'p');
	check(wait_for(L"PAUSED", 2000), "P pauses");
	tap('P', L'p');
	check(!wait_for(L"PAUSED", 400), "P resumes");

	tap(VK_SPACE, L' ');
	grab();
	check(number_under(L"SCORE") > 0, "a hard drop scores");

	if (fails)
		dump();
	tap('Q', L'q');
	DWORD code = 1;
	bool exited = WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0;
	if (exited)
		GetExitCodeProcess(pi.hProcess, &code);
	else
		TerminateProcess(pi.hProcess, 1);
	check(exited && code == 0, "Q quits cleanly");

	printf("%s\n", fails ? "console test FAILED" : "console test passed");
	return fails != 0;
}
