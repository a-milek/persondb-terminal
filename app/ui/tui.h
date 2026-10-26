/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * tui — the terminal the full-screen frontends draw on: a cell grid sent row
 * by row to a VT100 on the console UART, the keyboard, and the popups.
 *
 * Shared by ui_tui.c (the person database) and ui_tui_flights.c (the flight
 * database), which ui_tui_main.c runs one at a time from its sign-on screen.
 * None of them touches storage: they draw what the database layer returns.
 *
 * The look is a 1990s airline reservation terminal (Sabre, Amadeus — an IBM
 * 3270 underneath): green on black, upper case only, dates as 24OCT26, a
 * screen code on every view, PF keys (F3 back, F7/F8 page) and an operator
 * information line at the bottom. Plain ASCII throughout, so it needs nothing
 * from the terminal beyond VT100 and colour.
 */

#ifndef APP_CBOR_PERSONDB_TUI_H_
#define APP_CBOR_PERSONDB_TUI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/toolchain.h>

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

/* The screen size, from the terminal's answer to detect_size(). */
extern int cols;
extern int rows;
/* The text cursor, shown only while a field is edited (cursor_r >= 0). */
extern int cursor_r;
extern int cursor_c;

/* The output log the utilities screens show, newest last. */
#define LOG_LINES 16
extern char g_log[LOG_LINES][MAX_COLS];
extern uint8_t g_log_st[LOG_LINES];
extern int g_log_n;

/** Draws the current view into the grid. Whichever screen is active sets it;
 *  the popups call it to draw what they cover. */
extern void (*draw)(void);

/* -- the grid ------------------------------------------------------------ */
void grid_clear(void);
void fill(int r, int c, int w, char ch, uint8_t st);
/** Write @p s at (r, c), upper case, at most @p w cells; returns the cells written. */
int putn(int r, int c, int w, uint8_t st, const char *s);
int __printf_like(5, 6) put(int r, int c, int w, uint8_t st, const char *fmt, ...);
void box(int r, int c, int h, int w, uint8_t st, const char *title);
/** Send the rows that changed since the last call. */
void present(void);

/* -- the terminal -------------------------------------------------------- */
int rx_start(void);
/** A key, or K_NONE after @p timeout_ms (< 0: wait forever). */
int getkey(int timeout_ms);
void detect_size(void);
void screen_enter(void);
void screen_leave(void);

/* -- text ---------------------------------------------------------------- */
const char *errstr(int rc);
void __printf_like(2, 3) status(uint8_t st, const char *fmt, ...);
void __printf_like(2, 3) log_line(uint8_t st, const char *fmt, ...);
/** epoch seconds -> 24OCT26 */
void airdate(uint32_t epoch, char *out, size_t sz);

/* -- layout -------------------------------------------------------------- */
int body_top(void);
int body_bottom(void);    /* last usable row */
void tui_footer(const char *const *hints, size_t n, const char *bar_left,
		const char *bar_right);
void draw_list_pane(int r, int c, int h, int w, const char *title, bool focused,
		    int sel, int n, const char *(*item)(int i));

/* -- popups -------------------------------------------------------------- */
struct popup {
	int r, c, h, w;
};

struct popup popup_open(int h, int w, const char *title);
void message(const char *title, uint8_t st, const char *l1, const char *l2);
bool confirm(const char *title, const char *l1, const char *l2);
bool input_text(const char *title, const char *prompt, char *out, size_t cap,
		bool hex, bool digits);
bool input_number(const char *title, const char *prompt, uint32_t *val,
		  uint32_t max);
int pick(const char *title, int n, const char *(*item)(int i), int sel);
bool busy(const char *title, const char *what, uint32_t done, uint32_t total);

/** Ask, then leave the screen. On native_sim @p close runs and the process
 *  exits; a board parks until a key. Returns false if the operator declined. */
bool sign_off(void (*close)(void));

#endif /* APP_CBOR_PERSONDB_TUI_H_ */
