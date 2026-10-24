/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui — the same operations, as full-screen views on a serial terminal.
 *
 * Parsing and printing only, like ui_shell.c (DESIGN.md F15): every action is
 * one call into scenario.h or persondb.h. What this frontend adds is
 * navigation — arrow keys over the persons, their permissions and their cards
 * — on any VT100-compatible terminal attached to the console UART, with no
 * tool on the host side.
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
 *
 * The look is a 1990s airline reservation terminal (Sabre, Amadeus — an IBM
 * 3270 underneath): green on black, upper case only, names as SURNAME/GIVEN,
 * dates as 24OCT26, a screen code on every view, PF keys (F3 back, F7/F8
 * page) and an operator information line at the bottom. Plain ASCII
 * throughout, so it needs nothing from the terminal beyond VT100 and colour.
 *
 * Runs on the main thread, whose stack prj.conf sizes for the scenario calls;
 * everything large here is static.
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

#include "scenario.h"

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"   /* posix_exit() — end the sim run cleanly */
#endif

static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* -- screen: a cell grid, sent row by row ------------------------------- */

/* Larger terminals are used up to this size; the rest stays blank. */
#define MAX_COLS 132
#define MAX_ROWS 50
#define MIN_COLS 72
#define MIN_ROWS 20

enum style {
	ST_NORMAL = 0,
	ST_DIM,
	ST_BOLD,
	ST_TITLE,
	ST_BAR,
	ST_TAB,
	ST_SEL,
	ST_SEL_IDLE,
	ST_KEY,
	ST_OK,
	ST_BAD,
	ST_WARN,
	ST_POPUP,
	ST_POPUP_BOX,
	ST_FIELD,
	ST__COUNT,
};

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
static int cols = 80;
static int rows = 24;
static int cursor_r = -1;  /* shown only while a text field is edited */
static int cursor_c;

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

static void grid_clear(void)
{
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			grid[r][c] = (struct cell){ ' ', ST_NORMAL };
		}
	}
	cursor_r = -1;
}

static void fill(int r, int c, int w, char ch, uint8_t st)
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
static int putn(int r, int c, int w, uint8_t st, const char *s)
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

static int __printf_like(5, 6) put(int r, int c, int w, uint8_t st, const char *fmt, ...)
{
	char tmp[MAX_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	return putn(r, c, w, st, tmp);
}

static void box(int r, int c, int h, int w, uint8_t st, const char *title)
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

static void present(void)
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

enum {
	K_NONE = -1,
	K_TAB = 0x09,
	K_ENTER = 0x0d,
	K_ESC = 0x1b,
	K_UP = 0x100,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_DEL,
	K_BTAB,
	K_BS,
	K_REDRAW,
	K_INTR,
	K_CPR,     /* cursor position report, the answer to a size query */
};

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

static int rx_start(void)
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

static int getkey(int timeout_ms)
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
static void detect_size(void)
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

static void screen_enter(void)
{
	/* Let whatever was logged at open reach the terminal first. */
	for (int i = 0; i < 50 && log_data_pending(); i++) {
		k_msleep(10);
	}
	logs_pause(true);
	out_str("\x1b[?1049h\x1b[?7l");    /* alternate screen, no autowrap */
	detect_size();
}

static void screen_leave(void)
{
	out_str("\x1b[0m\x1b(B\x1b[?7h\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l");
	out_flush();
	logs_pause(false);
}

/* -- application state --------------------------------------------------- */

enum tab { TAB_PEOPLE, TAB_CHECK, TAB_STORE, TAB__COUNT };

static const char *const tab_name[TAB__COUNT] = {
	[TAB_PEOPLE] = "PERSONS",
	[TAB_CHECK]  = "ACCESS",
	[TAB_STORE]  = "UTILITIES",
};

static struct scenario g_s;
static bool g_open;
static int g_open_rc;
static struct persondb_stat g_st;
static enum tab g_tab;

static char g_status[MAX_COLS];
static uint8_t g_status_st;

/* People list. Rows are read through a small cache, so moving inside a page
 * costs no flash reads and scrolling costs one. */
#define ROW_CACHE 64

struct row {
	bool     valid;
	uint32_t index;
	int      rc;
	uint32_t id;
	char     name[PERSONDB_NAME_MAX + 1];
	char     dept[PERSONDB_DEPT_MAX + 1];
	char     title[PERSONDB_TITLE_MAX + 1];
	uint8_t  n_perms;
	uint8_t  n_cards;
};

static struct row g_rows[ROW_CACHE];
static uint32_t g_list_cur;
static uint32_t g_list_top;

/* The person view, over the people list. */
static bool g_detail;
static uint32_t g_p_index;
static int g_p_rc;
static struct persondb_person g_p;
static int g_pane;            /* 0: permissions, 1: cards */
static int g_pane_sel[2];

/* Access check. */
#define HISTORY 8

struct check_entry {
	char     card[PERSONDB_CARD_LEN + 1];
	char     perm[PERSONDB_PERM_MAX + 1];
	int      rc;
	enum persondb_decision d;
	uint32_t id;
	char     name[PERSONDB_NAME_MAX + 1];
	int64_t  us;
};

static char g_chk_card[PERSONDB_CARD_LEN + 1];
static char g_chk_perm[PERSONDB_PERM_MAX + 1];
static int g_chk_field;       /* 0: card, 1: permission, 2: the button */
static struct check_entry g_hist[HISTORY];
static int g_hist_n;          /* newest first */

/* Store. */
#define LOG_LINES 16

static struct scenario_report g_rep;
static int g_rep_rc = -EAGAIN;
static int g_store_cur;
static char g_log[LOG_LINES][MAX_COLS];
static uint8_t g_log_st[LOG_LINES];
static int g_log_n;           /* newest last */

/* Scratch for single reads, kept off the stack. */
static struct persondb_person g_tmp;

static const char *errstr(int rc)
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

static void __printf_like(2, 3) status(uint8_t st, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_status, sizeof(g_status), fmt, ap);
	va_end(ap);
	g_status_st = st;
}

static void __printf_like(2, 3) log_line(uint8_t st, const char *fmt, ...)
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
static void airdate(uint32_t epoch, char *out, size_t sz)
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

/* "Alice Smith-0042" -> "SMITH-0042/ALICE", as a PNR spells it. */
static const char *pnr_name(const char *name)
{
	static char buf[PERSONDB_NAME_MAX + 2];
	const char *sp = strrchr(name, ' ');

	if (sp == NULL) {
		return name;
	}
	snprintf(buf, sizeof(buf), "%s/%.*s", sp + 1, (int)(sp - name), name);
	return buf;
}

static void stat_refresh(void)
{
	if (!g_open || persondb_stat(g_s.db, &g_st) != 0) {
		memset(&g_st, 0, sizeof(g_st));
	}
}

static void rows_invalidate(void)
{
	for (int i = 0; i < ROW_CACHE; i++) {
		g_rows[i].valid = false;
	}
	g_rep_rc = -EAGAIN;
}

static const struct row *row_get(uint32_t index)
{
	struct row *r = &g_rows[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = scenario_person_show(&g_s, index, &g_tmp);
	if (r->rc == 0) {
		r->id = g_tmp.id;
		strcpy(r->name, g_tmp.name);
		strcpy(r->dept, g_tmp.dept);
		strcpy(r->title, g_tmp.title);
		r->n_perms = g_tmp.n_perms;
		r->n_cards = g_tmp.n_cards;
	}
	return r;
}

static void person_load(uint32_t index)
{
	g_p_index = index;
	g_p_rc = scenario_person_show(&g_s, index, &g_p);
	if (g_p_rc != 0) {
		memset(&g_p, 0, sizeof(g_p));
	}
	g_pane_sel[0] = MIN(g_pane_sel[0], MAX((int)g_p.n_perms - 1, 0));
	g_pane_sel[1] = MIN(g_pane_sel[1], MAX((int)g_p.n_cards - 1, 0));
}

/* -- drawing ------------------------------------------------------------- */

static int body_top(void)
{
	return 2;
}

static int body_bottom(void)    /* last usable row */
{
	return rows - 4;
}

/* The screen code and name, the system, and a clock that runs from the
 * instant access checks are decided at. */
static void draw_header(void)
{
	static const char *const screen[TAB__COUNT] = {
		[TAB_PEOPLE] = "PDB100  PERSON DIRECTORY",
		[TAB_CHECK]  = "PDB200  ACCESS VERIFICATION",
		[TAB_STORE]  = "PDB900  SYSTEM UTILITIES",
	};
	static const char sys[] = "PERSONDB ACCESS CONTROL";
	uint32_t now = SCENARIO_BENCH_NOW + (uint32_t)(k_uptime_get() / 1000);
	char right[24], date[16];
	int c = 0;
	int n;

	airdate(now, date, sizeof(date));
	n = snprintf(right, sizeof(right), "%s %02u%02uZ", date, (now / 3600u) % 24u,
		     (now / 60u) % 60u);
	put(0, 1, cols, ST_TITLE, "%s",
	    (g_tab == TAB_PEOPLE && g_detail) ? "PDB110  PERSON RECORD DISPLAY" : screen[g_tab]);
	putn(0, (cols - (int)strlen(sys)) / 2 + 8, cols, ST_NORMAL, sys);
	putn(0, cols - n - 1, n, ST_NORMAL, right);

	/* The screens, on the separator: the current one in reverse. */
	fill(1, 0, cols, '-', ST_NORMAL);
	c = 2;
	for (int t = 0; t < TAB__COUNT; t++) {
		c += put(1, c, cols - c, (t == (int)g_tab) ? ST_TAB : ST_NORMAL, " %d %s ",
			 t + 1, tab_name[t]);
		c += 2;
	}
}

/* The message line, the PF key line — pairs of "key", "what it does" — and
 * the operator information line of a 3270: X SYSTEM when input is refused. */
static void draw_footer(const char *const *hints, size_t n)
{
	int r = rows - 3;
	int c = 1;

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
	if (g_open) {
		char right[48];
		int len = snprintf(right, sizeof(right), "REV %u  %u/%u PERSONS  LU PDB01 ",
				   g_st.rev, g_st.populated, g_st.n_persons);

		putn(r + 2, 0, cols, ST_BAR, " 1A  ONLINE");
		putn(r + 2, cols - len - 1, len, ST_BAR, right);
	} else {
		putn(r + 2, 0, cols, ST_BAR, " 1A  X SYSTEM  STORE NOT OPEN");
	}
}

static void draw_not_open(int r)
{
	put(r, 2, cols - 4, ST_BAD, "The store is not open: %d (%s).", g_open_rc,
	    errstr(g_open_rc));
	put(r + 2, 2, cols - 4, ST_NORMAL,
	    "A store this build cannot read can be erased from the Store tab.");
}

static void draw_people_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "TAB", "NEXT SCREEN", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;          /* rows below the column header */
	int name_w = CLAMP(cols - 59, 16, PERSONDB_NAME_MAX);
	uint32_t n = g_st.populated;

	draw_footer(hints, ARRAY_SIZE(hints));
	if (!g_open) {
		draw_not_open(top + 1);
		return;
	}
	if (n == 0) {
		put(top + 1, 2, cols - 4, ST_NORMAL, "The store is empty.");
		put(top + 3, 2, cols - 4, ST_NORMAL,
		    "Press f to generate persons, or go to the Store tab.");
		return;
	}

	g_list_cur = MIN(g_list_cur, n - 1);
	if (g_list_cur < g_list_top) {
		g_list_top = g_list_cur;
	} else if (g_list_cur >= g_list_top + (uint32_t)h) {
		g_list_top = g_list_cur - (uint32_t)h + 1;
	}

	put(top, 0, cols, ST_TITLE, "  %6s  %-7s  %-*s  %-12s  %-12s %5s %5s", "NBR",
	    "ID", name_w, "NAME", "DEPT", "TITLE", "PERM", "CARD");
	for (int i = 0; i < h; i++) {
		uint32_t index = g_list_top + (uint32_t)i;
		bool sel = (index == g_list_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct row *r;

		if (index >= n) {
			break;
		}
		r = row_get(index);
		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %6u  read failed: %d (%s)", index, r->rc, errstr(r->rc));
			continue;
		}
		put(top + 1 + i, 0, cols, st, "  %06u  %-7u  %-*.*s  %-12.12s  %-12.12s %5u %5u",
		    index, r->id, name_w, name_w, pnr_name(r->name), r->dept, r->title, r->n_perms,
		    r->n_cards);
	}
	{
		char pos[32];
		int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", g_list_cur + 1, n);

		putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
	}
}

static void draw_list_pane(int r, int c, int h, int w, const char *title, bool focused,
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

static const char *perm_item(int i)
{
	return g_p.perm[i];
}

static const char *card_item(int i)
{
	return g_p.card[i];
}

static void draw_person(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "TAB", "PANE", "G", "GRANT", "D", "DELETE",
		"A", "ADD CARD", "C", "VERIFY", "F7/F8", "PREV/NEXT",
	};
	int top = body_top();
	char from[16], until[16], now[16];
	char title[32];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_p_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Person at index %u: %d (%s).", g_p_index,
		    g_p_rc, errstr(g_p_rc));
		return;
	}

	airdate(g_p.valid_from, from, sizeof(from));
	airdate(g_p.valid_until, until, sizeof(until));
	airdate(SCENARIO_BENCH_NOW, now, sizeof(now));

	put(top, 2, cols - 4, ST_BOLD, "1.%s", pnr_name(g_p.name));
	put(top, 42, cols, ST_NORMAL, "ID %u  NBR %06u", g_p.id, g_p_index);
	put(top + 1, 2, cols - 4, ST_NORMAL, "DEPT/TITLE  %s/%s", g_p.dept, g_p.title);
	put(top + 2, 2, cols - 4, ST_NORMAL, "VALID       %s-%s", from, until);
	if (SCENARIO_BENCH_NOW < g_p.valid_from) {
		put(top + 2, 42, cols, ST_WARN, "not yet valid on %s", now);
	} else if (SCENARIO_BENCH_NOW >= g_p.valid_until) {
		put(top + 2, 42, cols, ST_WARN, "expired on %s", now);
	} else {
		put(top + 2, 42, cols, ST_OK, "valid on %s", now);
	}

	{
		int r = top + 4;
		int h = body_bottom() - r + 1;
		int lw = cols / 2 - 1;

		snprintf(title, sizeof(title), "Permissions (%u)", g_p.n_perms);
		draw_list_pane(r, 1, h, lw, title, g_pane == 0, g_pane_sel[0], g_p.n_perms,
			       perm_item);
		snprintf(title, sizeof(title), "Cards (%u)", g_p.n_cards);
		draw_list_pane(r, lw + 2, h, cols - lw - 3, title, g_pane == 1,
			       g_pane_sel[1], g_p.n_cards, card_item);
	}
}

static const char *decision_word(const struct check_entry *e, uint8_t *st)
{
	if (e->rc != 0) {
		*st = ST_BAD;
		return "ERROR";
	}
	switch (e->d) {
	case PERSONDB_GRANTED:
		*st = ST_OK;
		return "GRANTED";
	case PERSONDB_DENIED:
		*st = ST_BAD;
		return "DENIED";
	case PERSONDB_EXPIRED:
		*st = ST_WARN;
		return "EXPIRED";
	default:
		*st = ST_WARN;
		return "UNKNOWN CARD";
	}
}

static void draw_check(void)
{
	static const char *const hints[] = {
		"ENTER", "EDIT/VERIFY", "C", "VERIFY NOW", "TAB", "NEXT SCREEN",
		"Q", "SIGN OFF",
	};
	static const char *const labels[] = { "Card", "Permission", "" };
	int top = body_top();
	char now[16];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (!g_open) {
		draw_not_open(top + 1);
		return;
	}

	airdate(SCENARIO_BENCH_NOW, now, sizeof(now));
	put(top, 2, cols - 4, ST_TITLE, "WOULD THIS CARD OPEN THIS DOOR");
	put(top + 1, 2, cols - 4, ST_NORMAL, "DECIDED AS OF %s, THE INSTANT THE BENCHMARK USES",
	    now);

	for (int f = 0; f < 3; f++) {
		int r = top + 3 + f;
		bool sel = (g_chk_field == f);

		put(r, 2, 2, ST_BOLD, "%s", sel ? ">" : " ");
		put(r, 4, 12, sel ? ST_BOLD : ST_NORMAL, "%s", labels[f]);
		if (f == 0) {
			fill(r, 16, PERSONDB_CARD_LEN + 2, ' ', sel ? ST_SEL : ST_FIELD);
			put(r, 17, PERSONDB_CARD_LEN, sel ? ST_SEL : ST_FIELD, "%s",
			    g_chk_card);
		} else if (f == 1) {
			fill(r, 16, PERSONDB_PERM_MAX + 2, ' ', sel ? ST_SEL : ST_FIELD);
			put(r, 17, PERSONDB_PERM_MAX, sel ? ST_SEL : ST_FIELD, "%s",
			    g_chk_perm);
		} else {
			put(r, 16, 12, sel ? ST_SEL : ST_FIELD, "[ Check ]");
		}
	}

	if (g_hist_n > 0) {
		const struct check_entry *e = &g_hist[0];
		uint8_t st;
		const char *w = decision_word(e, &st);
		int r = top + 7;

		box(r, 1, 4, cols - 2, ST_DIM, "Result");
		put(r + 1, 3, cols - 6, st, "%s", w);
		if (e->rc != 0) {
			put(r + 1, 4 + (int)strlen(w), cols, ST_NORMAL, "%d (%s)", e->rc,
			    errstr(e->rc));
		} else {
			put(r + 1, 4 + (int)strlen(w), cols, ST_DIM, "in %lld us",
			    (long long)e->us);
			if (e->d != PERSONDB_UNKNOWN_CARD) {
				put(r + 2, 3, cols - 6, ST_NORMAL, "%s (id %u) for %s",
				    pnr_name(e->name), e->id, e->perm);
			} else {
				put(r + 2, 3, cols - 6, ST_NORMAL, "no person holds card %s",
				    e->card);
			}
		}

		r += 5;
		if (r < body_bottom()) {
			put(r, 2, cols - 4, ST_TITLE, "Recent checks");
		}
		for (int i = 0; i < g_hist_n && r + 1 + i <= body_bottom(); i++) {
			e = &g_hist[i];
			w = decision_word(e, &st);
			put(r + 1 + i, 2, 14, st, "%s", w);
			put(r + 1 + i, 16, cols - 18, ST_NORMAL, "%-14s  %-22s  %s", e->card,
			    e->perm,
			    (e->rc == 0 && e->d != PERSONDB_UNKNOWN_CARD) ? pnr_name(e->name) : "");
		}
	}
}

static const char *const store_actions[] = {
	"Fill persons...",
	"Verify a sample",
	"Mutate (next revision)",
	"Benchmark",
	"Refresh statistics",
	"Reset store (erase all)",
};

static void draw_store(void)
{
	static const char *const hints[] = {
		"ENTER", "EXECUTE", "TAB", "NEXT SCREEN", "Q", "SIGN OFF",
	};
	int top = body_top();
	int r = top + 1;
	int ac = MIN(48, cols - 30);

	draw_footer(hints, ARRAY_SIZE(hints));

	put(top, 2, ac - 4, ST_TITLE, "Statistics");
	if (!g_open) {
		put(r, 2, ac - 4, ST_BAD, "not open: %d (%s)", g_open_rc, errstr(g_open_rc));
		r += 2;
	} else {
		if (g_rep_rc == -EAGAIN) {
			g_rep_rc = scenario_report_get(&g_s, &g_rep);
		}
		if (g_rep_rc != 0) {
			put(r++, 2, ac - 4, ST_BAD, "report: %d (%s)", g_rep_rc,
			    errstr(g_rep_rc));
		} else {
			const struct persondb_stat *st = &g_rep.st;

			put(r++, 2, ac - 4, ST_NORMAL, "Persons      %u of %u", st->populated,
			    st->n_persons);
			put(r++, 2, ac - 4, ST_NORMAL, "Revision     %u", st->rev);
			put(r++, 2, ac - 4, ST_NORMAL, "Credentials  %u", g_rep.credentials);
			put(r++, 2, ac - 4, ST_NORMAL, "Live content %llu B, %u.%u %%",
			    (unsigned long long)g_rep.logical_bytes, g_rep.fill_permille / 10,
			    g_rep.fill_permille % 10);
			put(r++, 2, ac - 4, ST_NORMAL, "Partition    %zu B", st->partition_bytes);
			put(r++, 2, ac - 4, ST_NORMAL, "Maps         %u + %u buckets",
			    st->people_buckets, st->cred_buckets);
			put(r++, 2, ac - 4, st->enospc_hits ? ST_BAD : ST_NORMAL,
			    "Overflows    %u", st->enospc_hits);
			if (st->io_measured) {
				put(r++, 2, ac - 4, ST_DIM, "Flash        %llu rd, %llu wr, %llu er",
				    (unsigned long long)st->flash_reads,
				    (unsigned long long)st->flash_writes,
				    (unsigned long long)st->flash_erases);
			}
		}
	}

	put(top, ac, cols - ac, ST_TITLE, "Actions");
	for (int i = 0; i < (int)ARRAY_SIZE(store_actions); i++) {
		bool sel = (i == g_store_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;

		fill(top + 1 + i, ac, cols - ac - 1, ' ', st);
		put(top + 1 + i, ac + 1, cols - ac - 2, st, "%s", store_actions[i]);
	}

	r = MAX(r, top + 1 + (int)ARRAY_SIZE(store_actions)) + 1;
	if (r < body_bottom()) {
		int h = body_bottom() - r;
		int first = MAX(g_log_n - h, 0);

		put(r, 2, cols - 4, ST_TITLE, "Output");
		for (int i = first; i < g_log_n; i++) {
			put(r + 1 + (i - first), 2, cols - 4, g_log_st[i], "%s", g_log[i]);
		}
	}
}

static void draw(void)
{
	grid_clear();
	draw_header();
	switch (g_tab) {
	case TAB_PEOPLE:
		if (g_detail) {
			draw_person();
		} else {
			draw_people_list();
		}
		break;
	case TAB_CHECK:
		draw_check();
		break;
	default:
		draw_store();
		break;
	}
}

/* -- popups -------------------------------------------------------------- */

struct popup {
	int r, c, h, w;
};

/* Draw the current screen with a popup over it; returns its interior. */
static struct popup popup_open(int h, int w, const char *title)
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

static void message(const char *title, uint8_t st, const char *l1, const char *l2)
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

static bool confirm(const char *title, const char *l1, const char *l2)
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
static bool input_text(const char *title, const char *prompt, char *out, size_t cap,
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

static bool input_number(const char *title, const char *prompt, uint32_t *val,
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
static int pick(const char *title, int n, const char *(*item)(int i), int sel)
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
static bool busy(const char *title, const char *what, uint32_t done, uint32_t total)
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

/* -- actions ------------------------------------------------------------- */

static void after_write(void)
{
	rows_invalidate();
	stat_refresh();
}

static void act_fill(void)
{
	uint32_t left = g_st.n_persons - g_st.populated;
	uint32_t want = MIN(500u, left);
	uint32_t done = 0;
	int64_t t0;
	int rc = 0;
	bool stopped = false;
	char l1[64], l2[64];

	if (!g_open) {
		status(ST_BAD, "fill: store not open");
		return;
	}
	if (left == 0) {
		message("Fill", ST_NORMAL, "The store already holds every person",
			"the dataset defines.");
		return;
	}
	snprintf(l1, sizeof(l1), "Persons to add (1..%u):", left);
	if (!input_number("Fill", l1, &want, left)) {
		return;
	}

	t0 = scenario_now_us();
	while (done < want && !stopped) {
		struct scenario_fill_report f;
		uint32_t chunk = MIN((uint32_t)CONFIG_APP_CBOR_PERSONDB_FILL_BATCH, want - done);

		stopped = busy("Fill", "Writing persons...", done, want);
		if (stopped) {
			break;
		}
		rc = scenario_fill(&g_s, chunk, &f);
		done += f.written;
		if (rc != 0 || f.written == 0) {
			break;
		}
	}
	after_write();

	snprintf(l1, sizeof(l1), "%u written in %lld ms%s", done,
		 (long long)(scenario_since_us(t0) / 1000), stopped ? " (stopped)" : "");
	snprintf(l2, sizeof(l2), "%u of %u persons now in the store", g_st.populated,
		 g_st.n_persons);
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "fill: %s, %s", l1, l2);
	if (rc != 0) {
		snprintf(l2, sizeof(l2), "fill stopped: %d (%s)", rc, errstr(rc));
		log_line(ST_BAD, "%s", l2);
	}
	message("Fill", rc != 0 ? ST_BAD : ST_OK, l1, l2);
}

static void act_verify(void)
{
	struct scenario_verify_report v;
	uint32_t n = CONFIG_APP_CBOR_PERSONDB_VERIFY_SAMPLES;
	char l1[64], l2[64];
	bool pass;
	int rc;

	if (!g_open) {
		status(ST_BAD, "verify: store not open");
		return;
	}
	if (!input_number("Verify", "Persons to sample:", &n, 65535)) {
		return;
	}
	busy("Verify", "Comparing the store with the generator...", 0, 0);
	rc = scenario_verify(&g_s, n, &v);
	if (rc != 0) {
		log_line(ST_BAD, "verify: %d (%s)", rc, errstr(rc));
		snprintf(l1, sizeof(l1), "verify failed: %d (%s)", rc, errstr(rc));
		message("Verify", ST_BAD, l1, "");
		return;
	}
	pass = v.bad_records == 0 && v.bad_cards == 0 && v.missing == 0;
	snprintf(l1, sizeof(l1), "%s: %u persons, %u cards in %lld ms",
		 pass ? "PASS" : "FAIL", v.persons_checked, v.cards_checked, (long long)v.ms);
	snprintf(l2, sizeof(l2), "%u bad records, %u bad cards, %u missing", v.bad_records,
		 v.bad_cards, v.missing);
	log_line(pass ? ST_OK : ST_BAD, "verify %s; %s", l1, l2);
	message("Verify", pass ? ST_OK : ST_BAD, l1, l2);
}

static void act_mutate(void)
{
	struct scenario_mutate_report m;
	char l1[64], l2[64];
	int rc;

	if (!g_open) {
		status(ST_BAD, "mutate: store not open");
		return;
	}
	busy("Mutate", "Revoking and granting temporary cards...", 0, 0);
	rc = scenario_mutate(&g_s, &m);
	after_write();
	if (rc != 0) {
		log_line(ST_BAD, "mutate: %d (%s)", rc, errstr(rc));
		snprintf(l1, sizeof(l1), "mutate failed: %d (%s)", rc, errstr(rc));
		message("Mutate", ST_BAD, l1, "");
		return;
	}
	snprintf(l1, sizeof(l1), "revision %u -> %u in %lld ms", m.rev_from, m.rev_to,
		 (long long)m.ms);
	snprintf(l2, sizeof(l2), "%u revoked, %u assigned, %u skipped", m.revoked,
		 m.assigned, m.skipped);
	log_line(ST_NORMAL, "mutate: %s, %s", l1, l2);
	message("Mutate", ST_OK, l1, l2);
}

static void act_bench(void)
{
	uint32_t n = CONFIG_APP_CBOR_PERSONDB_BENCH_SAMPLES;
	int rc = 0;

	if (!g_open) {
		status(ST_BAD, "bench: store not open");
		return;
	}
	if (!input_number("Benchmark", "Operations per phase:", &n, 65535)) {
		return;
	}
	log_line(ST_TITLE, "%-6s %6s %8s %9s %10s %6s", "phase", "ops", "ms", "us/op",
		 "flash ops", "amp");
	for (enum scenario_bench_kind k = 0; k < SCENARIO_BENCH_KINDS; k++) {
		struct bench_result b;
		char what[48];

		snprintf(what, sizeof(what), "Phase: %s", scenario_bench_name(k));
		if (busy("Benchmark", what, k, SCENARIO_BENCH_KINDS)) {
			log_line(ST_WARN, "stopped");
			break;
		}
		rc = scenario_bench(&g_s, k, n, &b);
		if (rc != 0) {
			log_line(ST_BAD, "%s: %d (%s)", scenario_bench_name(k), rc, errstr(rc));
			break;
		}
		log_line(ST_NORMAL, "%-6s %6u %8lld %9u %10llu %5ux", b.name, b.ops,
			 (long long)b.ms, b.us_per_op, (unsigned long long)b.flash_ops,
			 b.amplification);
	}
	after_write();
	status(rc != 0 ? ST_BAD : ST_OK, "benchmark %s", rc != 0 ? "failed" : "done");
}

static void act_reset(void)
{
	int rc;

	if (!confirm("Reset", "Erase every person and card?", "This cannot be undone.")) {
		return;
	}
	busy("Reset", "Erasing the store...", 0, 0);
	rc = scenario_erase(&g_s);
	g_open = (rc == 0);
	g_open_rc = rc;
	g_list_cur = 0;
	g_list_top = 0;
	g_detail = false;
	g_hist_n = 0;
	after_write();
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "reset: %d (%s)", rc, errstr(rc));
	status(rc != 0 ? ST_BAD : ST_OK, "reset: %s", errstr(rc));
}

static void run_check(void)
{
	struct check_entry *e;
	int64_t t;

	if (g_chk_card[0] == '\0' || g_chk_perm[0] == '\0') {
		status(ST_WARN, "enter a card and a permission first");
		return;
	}
	memmove(&g_hist[1], &g_hist[0], sizeof(g_hist[0]) * (HISTORY - 1));
	g_hist_n = MIN(g_hist_n + 1, HISTORY);
	e = &g_hist[0];
	memset(e, 0, sizeof(*e));
	strcpy(e->card, g_chk_card);
	strcpy(e->perm, g_chk_perm);

	t = scenario_now_us();
	e->rc = persondb_check(g_s.db, e->card, e->perm, SCENARIO_BENCH_NOW, &e->d, &g_tmp);
	e->us = scenario_since_us(t);
	if (e->rc == 0 && e->d != PERSONDB_UNKNOWN_CARD) {
		e->id = g_tmp.id;
		strcpy(e->name, g_tmp.name);
	}
	status(ST_NORMAL, "");
}

/* The vocabulary, plus a last entry for a permission typed by hand. Entries
 * the person in g_vocab_of already holds are marked. */
static const struct persondb_person *g_vocab_of;

static bool holds(const struct persondb_person *p, const char *perm)
{
	for (uint8_t i = 0; p != NULL && i < p->n_perms; i++) {
		if (strcmp(p->perm[i], perm) == 0) {
			return true;
		}
	}
	return false;
}

static const char *vocab_item(int i)
{
	static char buf[PERSONDB_PERM_MAX + 12];
	const char *perm;

	if (i >= scenario_permission_count()) {
		return "Other (type it)...";
	}
	perm = scenario_permission((uint8_t)i);
	snprintf(buf, sizeof(buf), "%-*s%s", PERSONDB_PERM_MAX, perm,
		 holds(g_vocab_of, perm) ? " held" : "");
	return buf;
}

/* @p of: mark what this person holds, or NULL. */
static bool pick_permission(const char *title, char *out,
			    const struct persondb_person *of)
{
	int n = scenario_permission_count();
	int sel = -1;

	for (int i = 0; i < n; i++) {
		const char *perm = scenario_permission((uint8_t)i);

		if (strcmp(out, perm) == 0 || (sel < 0 && out[0] == '\0' && !holds(of, perm))) {
			sel = i;
		}
	}
	g_vocab_of = of;
	sel = pick(title, n + 1, vocab_item, MAX(sel, 0));
	if (sel < 0) {
		return false;
	}
	if (sel < n) {
		strcpy(out, scenario_permission((uint8_t)sel));
		return true;
	}
	return input_text(title, "Permission:", out, PERSONDB_PERM_MAX + 1, false, false) &&
	       out[0] != '\0';
}

static void person_grant(void)
{
	char perm[PERSONDB_PERM_MAX + 1] = "";
	int rc;

	if (!pick_permission("Grant permission", perm, &g_p)) {
		return;
	}
	rc = persondb_permission_grant(g_s.db, g_p.id, perm);
	status(rc != 0 ? ST_BAD : ST_OK, "grant %s to %u: %s", perm, g_p.id, errstr(rc));
	after_write();
	person_load(g_p_index);
	for (int i = 0; i < g_p.n_perms; i++) {
		if (strcmp(g_p.perm[i], perm) == 0) {
			g_pane = 0;
			g_pane_sel[0] = i;
		}
	}
}

static void person_add_card(void)
{
	char card[PERSONDB_CARD_LEN + 1] = "";
	int rc;

	if (!input_text("Assign card", "Card id, 14 hex digits:", card, sizeof(card), true,
			false)) {
		return;
	}
	if (strlen(card) != PERSONDB_CARD_LEN) {
		status(ST_BAD, "a card id is exactly %d hex digits", PERSONDB_CARD_LEN);
		return;
	}
	rc = persondb_card_assign(g_s.db, g_p.id, card);
	status(rc != 0 ? ST_BAD : ST_OK, "assign %s to %u: %s", card, g_p.id, errstr(rc));
	after_write();
	person_load(g_p_index);
	g_pane = 1;
	g_pane_sel[1] = MAX((int)g_p.n_cards - 1, 0);
}

static void person_remove(void)
{
	char l1[96];
	int rc;

	if (g_pane == 0 && g_p.n_perms > 0) {
		char perm[PERSONDB_PERM_MAX + 1];

		strcpy(perm, g_p.perm[g_pane_sel[0]]);
		snprintf(l1, sizeof(l1), "Revoke %s from %s?", perm, pnr_name(g_p.name));
		if (!confirm("Revoke permission", l1, "")) {
			return;
		}
		rc = persondb_permission_revoke(g_s.db, g_p.id, perm);
		status(rc != 0 ? ST_BAD : ST_OK, "revoke %s from %u: %s", perm, g_p.id,
		       errstr(rc));
	} else if (g_pane == 1 && g_p.n_cards > 0) {
		char card[PERSONDB_CARD_LEN + 1];

		strcpy(card, g_p.card[g_pane_sel[1]]);
		snprintf(l1, sizeof(l1), "Take card %s from %s?", card, pnr_name(g_p.name));
		if (!confirm("Unassign card", l1, "")) {
			return;
		}
		rc = persondb_card_revoke_from(g_s.db, g_p.id, card);
		status(rc != 0 ? ST_BAD : ST_OK, "unassign %s: %s", card, errstr(rc));
	} else {
		return;
	}
	after_write();
	person_load(g_p_index);
}

/* From a person to the check tab, with the selection filled in. */
static void person_check(void)
{
	if (g_p.n_cards == 0) {
		status(ST_WARN, "this person holds no card");
		return;
	}
	strcpy(g_chk_card, g_p.card[(g_pane == 1) ? g_pane_sel[1] : 0]);
	if (g_pane == 0 && g_p.n_perms > 0) {
		strcpy(g_chk_perm, g_p.perm[g_pane_sel[0]]);
	} else if (g_chk_perm[0] == '\0') {
		strcpy(g_chk_perm, scenario_permission(0));
	}
	g_tab = TAB_CHECK;
	g_chk_field = 2;
	run_check();
}

/* -- key handling, per view ---------------------------------------------- */

static void go_to_index(void)
{
	uint32_t index = g_list_cur;

	if (g_st.populated == 0) {
		return;
	}
	if (input_number("Go to", "Dataset index:", &index, g_st.populated)) {
		g_list_cur = MIN(index, g_st.populated - 1);
	}
}

static bool keys_people_list(int k)
{
	uint32_t n = g_st.populated;
	uint32_t page = (uint32_t)MAX(body_bottom() - body_top() - 1, 1);

	switch (k) {
	case K_UP:
		g_list_cur -= (g_list_cur > 0) ? 1 : 0;
		return true;
	case K_DOWN:
		g_list_cur += (g_list_cur + 1 < n) ? 1 : 0;
		return true;
	case K_PGUP:
		g_list_cur = (g_list_cur > page) ? g_list_cur - page : 0;
		return true;
	case K_PGDN:
		g_list_cur = (n > 0) ? MIN(g_list_cur + page, n - 1) : 0;
		return true;
	case K_HOME:
		g_list_cur = 0;
		return true;
	case K_END:
		g_list_cur = (n > 0) ? n - 1 : 0;
		return true;
	case K_ENTER:
		if (g_open && n > 0) {
			g_pane = 0;
			g_pane_sel[0] = 0;
			g_pane_sel[1] = 0;
			person_load(g_list_cur);
			g_detail = true;
		}
		return true;
	case '/':
	case 'g':
		go_to_index();
		return true;
	case 'f':
		act_fill();
		return true;
	default:
		return false;
	}
}

static bool keys_person(int k)
{
	int n = (g_pane == 0) ? g_p.n_perms : g_p.n_cards;
	int *sel = &g_pane_sel[g_pane];

	switch (k) {
	case K_ESC:
	case K_BS:
		g_detail = false;
		return true;
	case K_UP:
		*sel = (*sel > 0) ? *sel - 1 : 0;
		return true;
	case K_DOWN:
		*sel = (*sel + 1 < n) ? *sel + 1 : *sel;
		return true;
	case K_LEFT:
	case K_RIGHT:
	case K_TAB:
		g_pane ^= 1;
		return true;
	case K_PGUP:
		if (g_p_index > 0) {
			person_load(g_p_index - 1);
			g_list_cur = g_p_index;
		}
		return true;
	case K_PGDN:
		if (g_p_index + 1 < g_st.populated) {
			person_load(g_p_index + 1);
			g_list_cur = g_p_index;
		}
		return true;
	case 'g':
		person_grant();
		return true;
	case 'd':
	case K_DEL:
		person_remove();
		return true;
	case 'a':
		person_add_card();
		return true;
	case 'c':
		person_check();
		return true;
	case 'r':
		person_load(g_p_index);
		return true;
	default:
		return false;
	}
}

static bool keys_check(int k)
{
	switch (k) {
	case K_UP:
		g_chk_field = (g_chk_field > 0) ? g_chk_field - 1 : 0;
		return true;
	case K_DOWN:
		g_chk_field = MIN(g_chk_field + 1, 2);
		return true;
	case 'c':
		run_check();
		return true;
	case K_ENTER:
		if (g_chk_field == 0) {
			input_text("Card", "Card id, 14 hex digits:", g_chk_card,
				   sizeof(g_chk_card), true, false);
			g_chk_field = 1;
		} else if (g_chk_field == 1) {
			if (pick_permission("Permission", g_chk_perm, NULL)) {
				g_chk_field = 2;
			}
		} else {
			run_check();
		}
		return true;
	default:
		return false;
	}
}

static bool keys_store(int k)
{
	int n = (int)ARRAY_SIZE(store_actions);

	switch (k) {
	case K_UP:
		g_store_cur = (g_store_cur > 0) ? g_store_cur - 1 : n - 1;
		return true;
	case K_DOWN:
		g_store_cur = (g_store_cur + 1 < n) ? g_store_cur + 1 : 0;
		return true;
	case K_ENTER:
		switch (g_store_cur) {
		case 0: act_fill(); break;
		case 1: act_verify(); break;
		case 2: act_mutate(); break;
		case 3: act_bench(); break;
		case 4:
			after_write();
			status(ST_NORMAL, "statistics refreshed");
			break;
		default: act_reset(); break;
		}
		return true;
	default:
		return false;
	}
}

static void quit(void)
{
	if (!confirm("Sign off", "Sign off this terminal?", "")) {
		return;
	}
	screen_leave();
#ifdef CONFIG_ARCH_POSIX
	if (g_open) {
		scenario_close(&g_s);
	}
	posix_exit(0);
#else
	/* A board has nowhere to exit to: park until a key brings it back. */
	printk("SIGNED OFF. PRESS ANY KEY TO SIGN IN.\n");
	while (getkey(-1) == K_NONE) {
	}
	screen_enter();
#endif
}

int ui_main(void)
{
	int rc = rx_start();

	if (rc != 0) {
		printk("persondb: console UART has no RX interrupt: %d\n", rc);
		return rc;
	}

	g_open_rc = scenario_open(&g_s);
	g_open = (g_open_rc == 0);
	stat_refresh();
	strcpy(g_chk_perm, scenario_permission(0));
	if (!g_open) {
		g_tab = TAB_STORE;
		status(ST_BAD, "open failed: %d (%s)", g_open_rc, errstr(g_open_rc));
	}

	screen_enter();
	for (;;) {
		bool used = false;
		int k;

		draw();
		present();
		k = getkey(1000);       /* a second without a key: redraw the clock */
		if (k == K_NONE) {
			continue;
		}
		if (k >= 'A' && k <= 'Z') {
			k = tolower(k);
		}

		switch (g_tab) {
		case TAB_PEOPLE:
			used = g_detail ? keys_person(k) : keys_people_list(k);
			break;
		case TAB_CHECK:
			used = keys_check(k);
			break;
		default:
			used = keys_store(k);
			break;
		}
		if (used) {
			continue;
		}

		switch (k) {
		case K_TAB:
		case K_RIGHT:
			g_tab = (g_tab + 1) % TAB__COUNT;
			break;
		case K_BTAB:
		case K_LEFT:
			g_tab = (g_tab + TAB__COUNT - 1) % TAB__COUNT;
			break;
		case '1': case '2': case '3':
			g_tab = (enum tab)(k - '1');
			break;
		case K_REDRAW:
			detect_size();
			break;
		case 'q':
		case K_INTR:
			quit();
			break;
		default:
			break;
		}
	}
	return 0;
}
