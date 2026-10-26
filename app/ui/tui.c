/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * tui — the terminal the full-screen frontends draw on. See tui.h.
 *
 * It owns the console UART while it runs: keys arrive through the UART's RX
 * interrupt, the screen goes out through uart_poll_out(), and the log backends
 * are paused so that a log line cannot land in the middle of a frame. Failures
 * are shown in the status line instead.
 *
 * Interrupts rather than uart_poll_in(), because polling loses bytes: the
 * nRF UARTE receives into a single byte that only a poll re-arms, and an
 * arrow key is a three-byte burst, the size report up to eight.
 *
 * Each frame is composed into a cell grid, and only the rows that changed
 * since the previous frame are sent. A full 80x24 frame is ~2 KB, ~200 ms at
 * 115 200 Bd; moving the cursor one row resends two rows.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include "tui.h"

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* -- screen: a cell grid, sent row by row ------------------------------- */


/* Every sequence starts with a reset, so a row can be sent on its own, and
 * sets a black background, so the screen is black on any terminal theme.
 * One phosphor colour: emphasis is intensity, reverse, underline and blink,
 * as on the real thing. */
static const char *const sgr[ST__COUNT] = {
	[ST_NORMAL]    = "\x1b[0;32;40m",
	[ST_DIM]       = "\x1b[0;2;32;40m",
	[ST_BOLD]      = "\x1b[0;1;92;40m",
	[ST_TITLE]     = "\x1b[0;1;92;40m",
	[ST_BAR]       = "\x1b[0;30;42m",
	[ST_TAB]       = "\x1b[0;30;102m",
	[ST_SEL]       = "\x1b[0;30;102m",
	[ST_SEL_IDLE]  = "\x1b[0;4;32;40m",
	[ST_KEY]       = "\x1b[0;1;92;40m",
	[ST_OK]        = "\x1b[0;1;92;40m",
	[ST_BAD]       = "\x1b[0;1;5;92;40m",
	[ST_WARN]      = "\x1b[0;1;4;92;40m",
	[ST_POPUP]     = "\x1b[0;32;40m",
	[ST_POPUP_BOX] = "\x1b[0;1;92;40m",
	[ST_FIELD]     = "\x1b[0;1;4;92;40m",
};

struct cell {
	char    ch;
	uint8_t st;
};

static struct cell grid[MAX_ROWS][MAX_COLS];
static uint32_t sent_hash[MAX_ROWS];
static bool sent_valid;
int cols = 80;
int rows = 24;
void (*draw)(void);
int cursor_r = -1;  /* shown only while a text field is edited */
int cursor_c;

static char obuf[256];
static size_t olen;

static void out_flush(void)
{
	for (size_t i = 0; i < olen; i++) {
		uart_poll_out(uart, obuf[i]);
	}
	olen = 0;
}

static void out_ch(char c)
{
	if (olen == sizeof(obuf)) {
		out_flush();
	}
	obuf[olen++] = c;
}

static void out_str(const char *s)
{
	while (*s != '\0') {
		out_ch(*s++);
	}
}

static void out_fmt(const char *fmt, ...)
{
	char tmp[32];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	out_str(tmp);
}

void grid_clear(void)
{
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			grid[r][c] = (struct cell){ ' ', ST_NORMAL };
		}
	}
	cursor_r = -1;
}

void fill(int r, int c, int w, char ch, uint8_t st)
{
	if (r < 0 || r >= rows) {
		return;
	}
	for (int i = 0; i < w && c + i < cols; i++) {
		if (c + i >= 0) {
			grid[r][c + i] = (struct cell){ ch, st };
		}
	}
}

/* Write @p s at (r, c), at most @p w cells; returns the cells written. */
int putn(int r, int c, int w, uint8_t st, const char *s)
{
	int n = 0;

	if (r < 0 || r >= rows) {
		return 0;
	}
	while (s[n] != '\0' && n < w && c + n < cols) {
		grid[r][c + n] = (struct cell){ (char)toupper((unsigned char)s[n]), st };
		n++;
	}
	return n;
}

int __printf_like(5, 6) put(int r, int c, int w, uint8_t st, const char *fmt, ...)
{
	char tmp[MAX_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	return putn(r, c, w, st, tmp);
}

void box(int r, int c, int h, int w, uint8_t st, const char *title)
{
	fill(r, c, w, '-', st);
	fill(r + h - 1, c, w, '-', st);
	for (int i = 1; i < h - 1; i++) {
		fill(r + i, c, 1, '|', st);
		fill(r + i, c + w - 1, 1, '|', st);
	}
	fill(r, c, 1, '+', st);
	fill(r, c + w - 1, 1, '+', st);
	fill(r + h - 1, c, 1, '+', st);
	fill(r + h - 1, c + w - 1, 1, '+', st);
	if (title != NULL) {
		put(r, c + 2, w - 4, st, "[ %s ]", title);
	}
}

static uint32_t row_hash(int r)
{
	uint32_t h = 2166136261u;

	for (int c = 0; c < cols; c++) {
		h = (h ^ (uint8_t)grid[r][c].ch) * 16777619u;
		h = (h ^ grid[r][c].st) * 16777619u;
	}
	return h;
}

void present(void)
{
	out_str("\x1b[?25l");
	for (int r = 0; r < rows; r++) {
		uint32_t h = row_hash(r);
		/* Writing the bottom-right cell scrolls some terminals. */
		int w = (r == rows - 1) ? cols - 1 : cols;
		int cur = -1;

		if (sent_valid && sent_hash[r] == h) {
			continue;
		}
		out_fmt("\x1b[%d;1H", r + 1);
		for (int c = 0; c < w; c++) {
			const struct cell *cl = &grid[r][c];

			if (cl->st != cur) {
				out_str(sgr[cl->st]);
				cur = cl->st;
			}
			out_ch(cl->ch);
		}
		out_str("\x1b[0m");
		sent_hash[r] = h;
	}
	sent_valid = true;
	if (cursor_r >= 0) {
		out_fmt("\x1b[%d;%dH\x1b[?25h", cursor_r + 1, cursor_c + 1);
	}
	out_flush();
}

/* -- keyboard ------------------------------------------------------------ */


static int cpr_row, cpr_col;

RING_BUF_DECLARE(rx_ring, 64);
static K_SEM_DEFINE(rx_sem, 0, 1);

static void uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buf[16];
	int n;

	ARG_UNUSED(user_data);
	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) > 0) {
		n = uart_fifo_read(dev, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
		/* A full ring drops the rest: a key-repeat burst, nothing more. */
		ring_buf_put(&rx_ring, buf, (uint32_t)n);
		k_sem_give(&rx_sem);
	}
}

int rx_start(void)
{
	int rc = uart_irq_callback_user_data_set(uart, uart_isr, NULL);

	if (rc == 0) {
		uart_irq_rx_enable(uart);
	}
	return rc;
}

/* One byte from the console, or -1 after @p timeout_ms (< 0: wait forever). */
static int raw_in(int timeout_ms)
{
	k_timeout_t t = (timeout_ms < 0) ? K_FOREVER : K_MSEC(timeout_ms);
	uint8_t c;

	while (ring_buf_get(&rx_ring, &c, 1) != 1) {
		if (k_sem_take(&rx_sem, t) != 0) {
			return -1;
		}
	}
	return c;
}

/* The tail of a CSI or SS3 sequence: ESC [ <params> <final>, ESC O <final>.
 * The PF keys of a 3270 map onto the keys this UI already has: F3 is Esc
 * (back, cancel), F7 and F8 are PgUp and PgDn. */
static int escape_seq(bool ss3)
{
	int p[2] = { 0, 0 };
	int np = 0;
	int c;

	for (;;) {
		c = raw_in(50);
		if (c < 0) {
			return K_NONE;
		}
		if (c >= '0' && c <= '9') {
			p[np] = p[np] * 10 + (c - '0');
		} else if (c == ';') {
			np = MIN(np + 1, 1);
		} else if (c >= 0x40 && c <= 0x7e) {
			break;
		}
	}

	if (ss3 && c == 'R') {
		return K_ESC;           /* F3 */
	}
	switch (c) {
	case 'A': return K_UP;
	case 'B': return K_DOWN;
	case 'C': return K_RIGHT;
	case 'D': return K_LEFT;
	case 'H': return K_HOME;
	case 'F': return K_END;
	case 'Z': return K_BTAB;
	case 'R':
		cpr_row = p[0];
		cpr_col = p[1];
		return K_CPR;
	case '~':
		switch (p[0]) {
		case 1: case 7: return K_HOME;
		case 4: case 8: return K_END;
		case 3: return K_DEL;
		case 5: case 18: return K_PGUP;   /* 18: F7 */
		case 6: case 19: return K_PGDN;   /* 19: F8 */
		case 13: return K_ESC;            /* F3 on rxvt */
		default: return K_NONE;
		}
	default:
		return K_NONE;
	}
}

int getkey(int timeout_ms)
{
	static bool after_cr;
	int c = raw_in(timeout_ms);

	/* Terminals send Enter as CR, CR LF or LF: count one key for each. */
	if (c == '\n' && after_cr) {
		after_cr = false;
		c = raw_in(timeout_ms);
	}
	after_cr = (c == '\r');

	switch (c) {
	case -1:
		return K_NONE;
	case 0x1b: {
		int c2 = raw_in(50);

		if (c2 == '[' || c2 == 'O') {
			return escape_seq(c2 == 'O');
		}
		return K_ESC;   /* a lone Esc, or Alt+key: both mean "back" */
	}
	case '\r':
	case '\n':
		return K_ENTER;
	case 0x7f:
	case 0x08:
		return K_BS;
	case 0x0c:
		return K_REDRAW;
	case 0x03:
		return K_INTR;
	default:
		return c;
	}
}

/* Ask the terminal where the cursor lands at (999, 999). One that does not
 * answer gets 80x24. */
void detect_size(void)
{
	int64_t until;

	out_str("\x1b" "7\x1b[999;999H\x1b[6n\x1b" "8");
	out_flush();
	cols = 80;
	rows = 24;
	until = k_uptime_get() + 300;
	while (k_uptime_get() < until) {
		if (getkey(50) == K_CPR) {
			rows = CLAMP(cpr_row, MIN_ROWS, MAX_ROWS);
			cols = CLAMP(cpr_col, MIN_COLS, MAX_COLS);
			break;
		}
	}
	out_str("\x1b[0m\x1b[2J");
	sent_valid = false;
}

static void logs_pause(bool pause)
{
	STRUCT_SECTION_FOREACH(log_backend, backend) {
		if (pause) {
			log_backend_disable(backend);
		} else {
			log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_MAX_LEVEL);
		}
	}
}

void screen_enter(void)
{
	/* Let whatever was logged at open reach the terminal first. */
	for (int i = 0; i < 50 && log_data_pending(); i++) {
		k_msleep(10);
	}
	logs_pause(true);
	out_str("\x1b[?1049h\x1b[?7l");    /* alternate screen, no autowrap */
	detect_size();
}

void screen_leave(void)
{
	out_str("\x1b[0m\x1b(B\x1b[?7h\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l");
	out_flush();
	logs_pause(false);
}

/* -- the message line and the output log -------------------------------- */

static char g_status[MAX_COLS];
static uint8_t g_status_st;

char g_log[LOG_LINES][MAX_COLS];
uint8_t g_log_st[LOG_LINES];
int g_log_n;

const char *errstr(int rc)
{
	switch (rc) {
	case 0:        return "ok";
	case -ENOENT:  return "not found";
	case -EEXIST:  return "already exists";
	case -ENOSPC:  return "no space";
	case -EINVAL:  return "invalid argument";
	case -EIO:     return "I/O error";
	case -ENOTSUP: return "not supported by this build";
	case -ENODEV:  return "store not open";
	case -EILSEQ:  return "corrupt record";
	case -ENOMEM:  return "out of memory";
	case -EBUSY:   return "busy";
	default:       return "error";
	}
}

void __printf_like(2, 3) status(uint8_t st, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_status, sizeof(g_status), fmt, ap);
	va_end(ap);
	g_status_st = st;
}

void __printf_like(2, 3) log_line(uint8_t st, const char *fmt, ...)
{
	va_list ap;

	if (g_log_n == LOG_LINES) {
		memmove(g_log[0], g_log[1], sizeof(g_log[0]) * (LOG_LINES - 1));
		memmove(&g_log_st[0], &g_log_st[1], LOG_LINES - 1);
		g_log_n--;
	}
	va_start(ap, fmt);
	vsnprintf(g_log[g_log_n], sizeof(g_log[0]), fmt, ap);
	va_end(ap);
	g_log_st[g_log_n++] = st;
}

/* epoch seconds -> 24OCT26, the airline way, proleptic Gregorian
 * (H. Hinnant's civil_from_days), so no libc time support is needed. */
void airdate(uint32_t epoch, char *out, size_t sz)
{
	static const char *const mon[12] = {
		"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
		"JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
	};
	int64_t z = (int64_t)(epoch / 86400u) + 719468;
	int64_t era = z / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int d = (int)(doy - (153 * mp + 2) / 5 + 1);
	int m = (int)(mp < 10 ? mp + 3 : mp - 9);
	int y = (int)(yoe + era * 400 + (m <= 2 ? 1 : 0));

	snprintf(out, sz, "%02d%s%02d", d, mon[m - 1], y % 100);
}


/* -- drawing ------------------------------------------------------------- */

int body_top(void)
{
	return 2;
}

int body_bottom(void)    /* last usable row */
{
	return rows - 4;
}

/* The message line, the PF key line — pairs of "key", "what it does" — and
 * the operator information line of a 3270, which the frontend fills in. */
void tui_footer(const char *const *hints, size_t n, const char *bar_left,
		const char *bar_right)
{
	int r = rows - 3;
	int c = 1;
	int len = (int)strlen(bar_right);

	if (g_status[0] != '\0') {
		put(r, 1, cols - 2, g_status_st, "> %s", g_status);
	} else {
		putn(r, 1, 2, ST_NORMAL, ">");
	}
	for (size_t i = 0; i + 1 < n; i += 2) {
		int need = (int)(strlen(hints[i]) + strlen(hints[i + 1]) + 3);

		if (c + need > cols) {
			break;
		}
		c += put(r + 1, c, cols - c, ST_KEY, "%s", hints[i]);
		c += put(r + 1, c, cols - c, ST_NORMAL, "=%s  ", hints[i + 1]);
	}

	fill(r + 2, 0, cols, ' ', ST_BAR);
	putn(r + 2, 0, cols, ST_BAR, bar_left);
	putn(r + 2, cols - len - 1, len, ST_BAR, bar_right);
}

void draw_list_pane(int r, int c, int h, int w, const char *title, bool focused,
			   int sel, int n, const char *(*item)(int i))
{
	int inner = h - 2;
	int first = (sel >= inner) ? sel - inner + 1 : 0;

	box(r, c, h, w, focused ? ST_TITLE : ST_DIM, title);
	if (n == 0) {
		put(r + 1, c + 2, w - 4, ST_DIM, "(none)");
		return;
	}
	for (int i = 0; i < inner && first + i < n; i++) {
		int idx = first + i;
		uint8_t st = ST_NORMAL;

		if (idx == sel) {
			st = focused ? ST_SEL : ST_SEL_IDLE;
		}
		fill(r + 1 + i, c + 1, w - 2, ' ', st);
		put(r + 1 + i, c + 2, w - 4, st, "%s", item(idx));
	}
	if (first > 0) {
		putn(r, c + w - 8, 5, focused ? ST_TITLE : ST_DIM, " UP ");
	}
	if (first + inner < n) {
		putn(r + h - 1, c + w - 8, 5, focused ? ST_TITLE : ST_DIM, " MORE ");
	}
}

/* -- popups -------------------------------------------------------------- */

/* Draw the current screen with a popup over it; returns its interior. */
struct popup popup_open(int h, int w, const char *title)
{
	struct popup p;

	w = MIN(w, cols - 2);
	h = MIN(h, rows - 2);
	p = (struct popup){ (rows - h) / 2, (cols - w) / 2, h, w };
	draw();
	for (int i = 0; i < h; i++) {
		fill(p.r + i, p.c, w, ' ', ST_POPUP);
	}
	box(p.r, p.c, h, w, ST_POPUP_BOX, title);
	return p;
}

void message(const char *title, uint8_t st, const char *l1, const char *l2)
{
	int w = MAX(MAX((int)strlen(l1), (int)strlen(l2)) + 6, 30);
	struct popup p = popup_open(6, w, title);

	put(p.r + 1, p.c + 3, p.w - 6, (st == ST_NORMAL) ? ST_POPUP : st, "%s", l1);
	put(p.r + 2, p.c + 3, p.w - 6, ST_POPUP, "%s", l2);
	put(p.r + 4, p.c + p.w - 10, 8, ST_FIELD, "[ OK ]");
	present();
	while (getkey(-1) == K_CPR) {
	}
}

bool confirm(const char *title, const char *l1, const char *l2)
{
	bool yes = false;
	int w = MAX(MAX((int)strlen(l1), (int)strlen(l2)) + 6, 34);

	for (;;) {
		struct popup p = popup_open(6, w, title);
		int k;

		put(p.r + 1, p.c + 3, p.w - 6, ST_POPUP, "%s", l1);
		put(p.r + 2, p.c + 3, p.w - 6, ST_POPUP, "%s", l2);
		put(p.r + 4, p.c + p.w - 20, 7, yes ? ST_FIELD : ST_POPUP, "[ Yes ]");
		put(p.r + 4, p.c + p.w - 11, 7, yes ? ST_POPUP : ST_FIELD, "[ No ]");
		present();

		k = getkey(-1);
		switch (k) {
		case K_LEFT: case K_RIGHT: case K_TAB:
			yes = !yes;
			break;
		case 'y': case 'Y':
			return true;
		case 'n': case 'N': case K_ESC:
			return false;
		case K_ENTER:
			return yes;
		default:
			break;
		}
	}
}

/* A one-line editor over a copy of @p out, which only Enter writes back.
 * @p hex restricts input to hex digits, upper-cased; @p digits to 0-9. */
bool input_text(const char *title, const char *prompt, char *out, size_t cap,
		       bool hex, bool digits)
{
	char buf[64];
	size_t len;

	cap = MIN(cap, sizeof(buf));
	snprintf(buf, cap, "%s", out);
	len = strlen(buf);
	int w = MAX((int)cap + 8, (int)strlen(prompt) + 6);

	w = MAX(w, 36);
	for (;;) {
		struct popup p = popup_open(7, w, title);
		int fw = MIN((int)cap + 1, p.w - 6);
		int k;

		put(p.r + 1, p.c + 3, p.w - 6, ST_POPUP, "%s", prompt);
		fill(p.r + 3, p.c + 3, fw, ' ', ST_FIELD);
		putn(p.r + 3, p.c + 3, fw, ST_FIELD, buf);
		put(p.r + 5, p.c + 3, p.w - 6, ST_POPUP, "ENTER=ACCEPT  F3=CANCEL");
		cursor_r = p.r + 3;
		cursor_c = p.c + 3 + MIN((int)len, fw - 1);
		present();

		k = getkey(-1);
		if (k == K_ENTER) {
			strcpy(out, buf);
			return true;
		}
		if (k == K_ESC) {
			return false;
		}
		if (k == K_BS && len > 0) {
			buf[--len] = '\0';
			continue;
		}
		if (k < 0x20 || k > 0x7e || len + 1 >= cap) {
			continue;
		}
		if (hex) {
			if (k >= 'a' && k <= 'f') {
				k -= 'a' - 'A';
			}
			if (!((k >= '0' && k <= '9') || (k >= 'A' && k <= 'F'))) {
				continue;
			}
		}
		if (digits && !(k >= '0' && k <= '9')) {
			continue;
		}
		buf[len++] = (char)k;
		buf[len] = '\0';
	}
}

bool input_number(const char *title, const char *prompt, uint32_t *val,
			 uint32_t max)
{
	char buf[11];

	snprintf(buf, sizeof(buf), "%u", *val);
	for (;;) {
		if (!input_text(title, prompt, buf, sizeof(buf), false, true)) {
			return false;
		}
		*val = (uint32_t)strtoul(buf, NULL, 10);
		if (*val >= 1 && *val <= max) {
			return true;
		}
		message(title, ST_BAD, "Out of range.", "");
		snprintf(buf, sizeof(buf), "%u", MIN(MAX(*val, 1u), max));
	}
}

/* A scrolling list; returns the chosen index, or -1. */
int pick(const char *title, int n, const char *(*item)(int i), int sel)
{
	int w = 36;

	for (int i = 0; i < n; i++) {
		w = MAX(w, (int)strlen(item(i)) + 8);
	}
	for (;;) {
		struct popup p = popup_open(MIN(n + 4, rows - 4), w, title);
		int inner = p.h - 4;
		int first = (sel >= inner) ? sel - inner + 1 : 0;
		int k;

		for (int i = 0; i < inner && first + i < n; i++) {
			uint8_t st = (first + i == sel) ? ST_FIELD : ST_POPUP;

			fill(p.r + 1 + i, p.c + 2, p.w - 4, ' ', st);
			put(p.r + 1 + i, p.c + 3, p.w - 6, st, "%s", item(first + i));
		}
		put(p.r + p.h - 2, p.c + 3, p.w - 6, ST_POPUP, "ENTER=SELECT  F3=CANCEL");
		present();

		k = getkey(-1);
		switch (k) {
		case K_UP:
			sel = (sel > 0) ? sel - 1 : n - 1;
			break;
		case K_DOWN:
			sel = (sel + 1 < n) ? sel + 1 : 0;
			break;
		case K_PGUP:
			sel = MAX(sel - inner, 0);
			break;
		case K_PGDN:
			sel = MIN(sel + inner, n - 1);
			break;
		case K_HOME:
			sel = 0;
			break;
		case K_END:
			sel = n - 1;
			break;
		case K_ENTER:
			return sel;
		case K_ESC:
			return -1;
		default:
			break;
		}
	}
}

/* Progress for a long operation; returns true if Esc was pressed. */
bool busy(const char *title, const char *what, uint32_t done, uint32_t total)
{
	struct popup p = popup_open(7, 50, title);
	int bw = p.w - 6;
	int k;

	put(p.r + 1, p.c + 3, bw, ST_POPUP, "%s", what);
	if (total > 0) {
		int filled = (int)((uint64_t)bw * MIN(done, total) / total);

		fill(p.r + 3, p.c + 3, bw, '.', ST_DIM);
		fill(p.r + 3, p.c + 3, filled, '#', ST_BOLD);
		put(p.r + 4, p.c + 3, bw, ST_POPUP, "%u of %u", done, total);
	}
	put(p.r + 5, p.c + 3, bw, ST_POPUP, "F3=STOP AFTER THIS STEP");
	present();

	while ((k = getkey(0)) != K_NONE) {
		if (k == K_ESC) {
			return true;
		}
	}
	return false;
}

bool sign_off(void (*close)(void))
{
	if (!confirm("Sign off", "Sign off this terminal?", "")) {
		return false;
	}
	screen_leave();
#ifdef CONFIG_ARCH_POSIX
	close();
	posix_exit(0);
#else
	ARG_UNUSED(close);
	/* A board has nowhere to exit to: park until a key brings it back. */
	printk("SIGNED OFF. PRESS ANY KEY TO SIGN IN.\n");
	while (getkey(-1) == K_NONE) {
	}
	screen_enter();
#endif
	return true;
}
