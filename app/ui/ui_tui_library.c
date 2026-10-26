/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_library — the library catalogue, as full-screen views on a serial
 * terminal: the catalogue, each book with its copies, the loans, and the
 * store utilities. A copy can be lent, returned and renewed from its book.
 *
 * Parsing and printing only, like ui_tui_flights.c: every record shown is
 * read from librarydb, so an empty store shows empty screens until a fill
 * writes the catalogue. The sign-on screen in ui_tui_main.c starts this one
 * and gets the terminal back on S.
 *
 * Runs on the main thread; everything large here is static.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "library_scenario.h"
#include "librarydb.h"
#include "tui.h"
#include "ui_tui_apps.h"

/* -- application state --------------------------------------------------- */

enum tab { TAB_BOOKS, TAB_LOANS, TAB_STORE, TAB__COUNT };

static const char *const tab_name[TAB__COUNT] = {
	[TAB_BOOKS] = "CATALOGUE",
	[TAB_LOANS] = "LOANS",
	[TAB_STORE] = "UTILITIES",
};

static struct library_scenario g_s;
static bool g_open;
static int g_open_rc;
static uint32_t g_nb, g_nl, g_na;   /* books, loans, loans out */
static enum tab g_tab;

/* Books and loans, each read through a small cache as the person list is. */
#define ROW_CACHE 64

struct book_row {
	bool     valid;
	uint32_t index;
	int      rc;
	struct librarydb_book b;
};

struct loan_row {
	bool     valid;
	uint32_t index;
	int      rc;
	struct librarydb_loan l;
};

static struct book_row g_books[ROW_CACHE];
static struct loan_row g_loans[ROW_CACHE];

static uint32_t g_bk_cur;
static uint32_t g_bk_top;
static uint32_t g_ln_cur;
static uint32_t g_ln_top;

/* The book view, over either tab. */
static bool g_bk_detail;
static uint32_t g_bk_index;
static int g_bk_rc;
static struct librarydb_book g_bk;
static struct librarydb_loan g_bk_loan[LIBRARYDB_COPIES_MAX];
static int g_bk_loan_rc[LIBRARYDB_COPIES_MAX];
static int g_copy_sel;

/* Utilities. */
static struct librarydb_stat g_st;
static int g_store_cur;

/* -- the clock ----------------------------------------------------------- */

static uint32_t now_epoch(void)
{
	return LIBRARY_SCENARIO_NOW + (uint32_t)(k_uptime_get() / 1000);
}

static uint16_t today(void)
{
	return library_scenario_today();
}

/* A day number as 24OCT26. */
static const char *day_str(uint16_t day, char *out, size_t sz)
{
	airdate((uint32_t)day * 86400u, out, sz);
	return out;
}

/* -- reading the store --------------------------------------------------- */

static void caches_drop(void)
{
	for (int i = 0; i < ROW_CACHE; i++) {
		g_books[i].valid = false;
		g_loans[i].valid = false;
	}
}

static void store_refresh(void)
{
	g_nb = 0;
	g_nl = 0;
	g_na = 0;
	if (g_open) {
		librarydb_counts(g_s.db, &g_nb, &g_nl, &g_na);
		librarydb_stat(g_s.db, &g_st);
	} else {
		memset(&g_st, 0, sizeof(g_st));
	}
	caches_drop();
}

static const struct book_row *book_get(uint32_t index)
{
	struct book_row *r = &g_books[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = librarydb_book_get(g_s.db, index, &r->b);
	return r;
}

static const struct loan_row *loan_get(uint32_t index)
{
	struct loan_row *r = &g_loans[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = librarydb_loan_get(g_s.db, index, &r->l);
	return r;
}

/* Read the book and the loan of each copy out. */
static void book_open(uint32_t index)
{
	g_bk_index = index;
	g_bk_rc = librarydb_book_get(g_s.db, index, &g_bk);
	if (g_bk_rc != 0) {
		memset(&g_bk, 0, sizeof(g_bk));
	}
	for (int c = 0; c < LIBRARYDB_COPIES_MAX; c++) {
		g_bk_loan_rc[c] = -ENOENT;
		if (g_bk_rc == 0 && c < g_bk.copies && g_bk.loan[c] != LIBRARYDB_NO_LOAN) {
			g_bk_loan_rc[c] = librarydb_loan_get(g_s.db, g_bk.loan[c], &g_bk_loan[c]);
		}
	}
	g_copy_sel = MIN(g_copy_sel, MAX(g_bk.copies - 1, 0));
	g_bk_detail = true;
}

/* -- drawing ------------------------------------------------------------- */

static void draw_header(void)
{
	static const char *const screen[TAB__COUNT] = {
		[TAB_BOOKS] = "LIB100  CATALOGUE",
		[TAB_LOANS] = "LIB200  LOANS",
		[TAB_STORE] = "LIB900  SYSTEM UTILITIES",
	};
	static const char sys[] = "LIBRARY CATALOGUE";
	uint32_t now = now_epoch();
	const char *code = g_bk_detail ? "LIB110  BOOK DISPLAY" : screen[g_tab];
	char right[24], date[16];
	int c;
	int n;

	airdate(now, date, sizeof(date));
	n = snprintf(right, sizeof(right), "%s %02u%02uZ", date, (now / 3600u) % 24u,
		     (now / 60u) % 60u);
	put(0, 1, cols, ST_TITLE, "%s", code);
	putn(0, (cols - (int)strlen(sys)) / 2 + 8, cols, ST_NORMAL, sys);
	putn(0, cols - n - 1, n, ST_NORMAL, right);

	fill(1, 0, cols, '-', ST_NORMAL);
	c = 2;
	for (int t = 0; t < TAB__COUNT; t++) {
		c += put(1, c, cols - c, (t == (int)g_tab) ? ST_TAB : ST_NORMAL, " %d %s ",
			 t + 1, tab_name[t]);
		c += 2;
	}
}

static void draw_footer(const char *const *hints, size_t n)
{
	char right[48];

	if (!g_open) {
		tui_footer(hints, n, " 1A  X SYSTEM  STORE NOT OPEN", "");
		return;
	}
	snprintf(right, sizeof(right), "%u BOOKS  %u ON LOAN  LU LIB01 ", g_nb, g_na);
	tui_footer(hints, n, " 1A  ONLINE", right);
}

static bool draw_unavailable(int r, uint32_t n, const char *what)
{
	if (!g_open) {
		put(r, 2, cols - 4, ST_BAD, "The library store is not open: %d (%s).", g_open_rc,
		    errstr(g_open_rc));
		put(r + 2, 2, cols - 4, ST_NORMAL,
		    "A store this build cannot read can be reset from Utilities.");
		return true;
	}
	if (n == 0) {
		put(r, 2, cols - 4, ST_NORMAL, "The library store holds no %s.", what);
		put(r + 2, 2, cols - 4, ST_NORMAL,
		    "Press F to load the catalogue, or go to Utilities.");
		return true;
	}
	return false;
}

static uint8_t status_style(enum librarydb_status s)
{
	switch (s) {
	case LIBRARYDB_OVERDUE:   return ST_BAD;
	case LIBRARYDB_DUE_TODAY:
	case LIBRARYDB_DUE_SOON:  return ST_WARN;
	case LIBRARYDB_RETURNED:  return ST_DIM;
	default:                  return ST_NORMAL;
	}
}

/* OVERDUE 6D, DUE TODAY, ... */
static const char *status_str(const struct librarydb_loan *l, char *out, size_t sz)
{
	enum librarydb_status s = librarydb_status(l, today());

	if (s == LIBRARYDB_OVERDUE) {
		snprintf(out, sz, "OVERDUE %uD", (unsigned)(today() - l->due));
	} else {
		snprintf(out, sz, "%s", librarydb_status_name(s));
	}
	return out;
}

static void list_scroll(uint32_t *cur, uint32_t *top, uint32_t n, int h)
{
	*cur = MIN(*cur, n - 1);
	if (*cur < *top) {
		*top = *cur;
	} else if (*cur >= *top + (uint32_t)h) {
		*top = *cur - (uint32_t)h + 1;
	}
}

static void draw_book_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int tw = CLAMP(cols - 50, 16, LIBRARYDB_TITLE_MAX);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nb, "books")) {
		return;
	}
	list_scroll(&g_bk_cur, &g_bk_top, g_nb, h);

	put(top, 0, cols, ST_TITLE, "  %-6s  %-11s %-*s  %-16s %4s  %s", "NBR", "CALL NO", tw,
	    "TITLE", "AUTHOR", "YEAR", "AVL");
	for (int i = 0; i < h && g_bk_top + (uint32_t)i < g_nb; i++) {
		uint32_t index = g_bk_top + (uint32_t)i;
		bool sel = (index == g_bk_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct book_row *r = book_get(index);
		const struct librarydb_book *b = &r->b;
		int avail;
		int c;

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %06u  read failed: %d (%s)", index, r->rc, errstr(r->rc));
			continue;
		}
		avail = librarydb_available(b);
		c = put(top + 1 + i, 0, cols, st, "  %06u  %-11s %-*.*s  %-16.16s %4u  ", index,
			b->call, tw, tw, b->title, b->author, b->year);
		put(top + 1 + i, c, cols - c, (sel || avail > 0) ? st : ST_WARN, "%d/%u", avail,
		    b->copies);
	}
	{
		char pos[32];
		int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", g_bk_cur + 1, g_nb);

		putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
	}
}

static void draw_book(void)
{
	static const char *const hints[] = {
		"F3", "BACK", "F7/F8", "PREV/NEXT", "L", "LEND", "R", "RETURN",
		"N", "RENEW", "Q", "SIGN OFF",
	};
	const struct librarydb_book *b = &g_bk;
	int top = body_top();

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_bk_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Book at position %u: %d (%s).", g_bk_index,
		    g_bk_rc, errstr(g_bk_rc));
		return;
	}
	put(top, 2, cols - 4, ST_BOLD, "1.%s", b->title);
	put(top + 1, 2, cols - 4, ST_NORMAL, "AUTHOR      %s", b->author);
	put(top + 1, 46, cols, ST_NORMAL, "NBR %u OF %u", g_bk_index + 1, g_nb);
	put(top + 2, 2, cols - 4, ST_NORMAL, "SUBJECT     %-15s CALL NO %s", b->subject, b->call);
	put(top + 3, 2, cols - 4, ST_NORMAL, "ISBN        %s  %u  %u PAGES", b->isbn, b->year,
	    b->pages);
	put(top + 4, 2, cols - 4, ST_NORMAL, "COPIES      %u  ON SHELF %d", b->copies,
	    librarydb_available(b));

	{
		int r = top + 6;
		int h = MIN(body_bottom() - r + 1, b->copies + 3);
		char d1[16], d2[16], s[24];

		if (h < 4) {
			return;
		}
		box(r, 1, h, cols - 2, ST_TITLE, "Copies");
		put(r + 1, 3, cols - 6, ST_TITLE, "%-4s %-6s  %-23s  %-7s  %-7s  %s", "CPY", "CARD",
		    "BORROWER", "OUT", "DUE", "STATUS");
		for (int c = 0; c < b->copies && r + 2 + c < r + h - 1; c++) {
			const struct librarydb_loan *l = &g_bk_loan[c];
			bool sel = (c == g_copy_sel);
			uint8_t st = sel ? ST_SEL : ST_NORMAL;
			int x;

			fill(r + 2 + c, 2, cols - 4, ' ', st);
			if (b->loan[c] == LIBRARYDB_NO_LOAN) {
				put(r + 2 + c, 3, cols - 6, st, "C%-3d %-6s  %-23s  %-7s  %-7s  %s", c + 1,
				    "", "", "", "", "ON SHELF");
				continue;
			}
			if (g_bk_loan_rc[c] != 0) {
				put(r + 2 + c, 3, cols - 6, sel ? ST_SEL : ST_BAD,
				    "C%-3d loan %u: read failed: %d", c + 1, b->loan[c], g_bk_loan_rc[c]);
				continue;
			}
			x = put(r + 2 + c, 3, cols - 6, st, "C%-3d %-6s  %-23s  %-7s  %-7s  ", c + 1,
				l->card, l->borrower, day_str(l->out, d1, sizeof(d1)),
				day_str(l->due, d2, sizeof(d2)));
			status_str(l, s, sizeof(s));
			if (l->renewals) {
				/* R2: renewed twice */
				snprintf(s + strlen(s), sizeof(s) - strlen(s), "  R%u", l->renewals);
			}
			put(r + 2 + c, 3 + x, cols - 6 - x,
			    sel ? ST_SEL : status_style(librarydb_status(l, today())), "%s", s);
		}
	}
}

static void draw_loan_list(void)
{
	static const char *const hints[] = {
		"ENTER", "BOOK", "F7", "BKWD", "F8", "FWD", "O", "NEXT OVERDUE",
		"S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int tw = CLAMP(cols - 62, 10, LIBRARYDB_TITLE_MAX);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nl, "loans")) {
		return;
	}
	list_scroll(&g_ln_cur, &g_ln_top, g_nl, h);

	put(top, 0, cols, ST_TITLE, "  %-5s  %-6s %-16s %-*s  %-7s  %s", "LOAN", "CARD",
	    "BORROWER", tw, "TITLE", "DUE", "STATUS");
	for (int i = 0; i < h && g_ln_top + (uint32_t)i < g_nl; i++) {
		uint32_t index = g_ln_top + (uint32_t)i;
		bool sel = (index == g_ln_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct loan_row *r = loan_get(index);
		const struct librarydb_loan *l = &r->l;
		const struct book_row *br;
		char due[16], s[24];
		int c;

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %05u  read failed: %d (%s)", index, r->rc, errstr(r->rc));
			continue;
		}
		br = book_get(l->book);
		c = put(top + 1 + i, 0, cols, st, "  %05u  %-6s %-16.16s %-*.*s  %-7s  ", index,
			l->card, l->borrower, tw, tw, (br->rc == 0) ? br->b.title : "?",
			day_str(l->due, due, sizeof(due)));
		put(top + 1 + i, c, cols - c, sel ? ST_SEL : status_style(librarydb_status(l, today())),
		    "%s", status_str(l, s, sizeof(s)));
	}
	{
		char pos[32];
		int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", g_ln_cur + 1, g_nl);

		putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
	}
}

static const char *const store_actions[] = {
	"Fill books...",
	"Verify the store",
	"Refresh statistics",
	"Reset library store",
};

static void draw_store(void)
{
	static const char *const hints[] = {
		"ENTER", "EXECUTE", "TAB", "NEXT SCREEN", "S", "SYSTEM", "Q", "SIGN OFF",
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
		put(r++, 2, ac - 4, ST_NORMAL, "Books        %u of %u", g_st.books,
		    LIBRARYDB_BOOKS_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Loans        %u of %u, %u out", g_st.loans,
		    LIBRARYDB_LOANS_MAX, g_st.active);
		put(r++, 2, ac - 4, ST_NORMAL, "Partition    %zu B", g_st.partition_bytes);
		put(r++, 2, ac - 4, ST_NORMAL, "Maps         %u + %u buckets", g_st.book_buckets,
		    g_st.loan_buckets);
		put(r++, 2, ac - 4, g_st.enospc_hits ? ST_BAD : ST_NORMAL, "Overflows    %u",
		    g_st.enospc_hits);
		put(r++, 2, ac - 4, ST_DIM, "Map ops      %llu get, %llu set",
		    (unsigned long long)g_st.map_gets, (unsigned long long)g_st.map_sets);
		if (g_st.io_measured) {
			put(r++, 2, ac - 4, ST_DIM, "Flash        %llu rd, %llu wr, %llu er",
			    (unsigned long long)g_st.flash_reads,
			    (unsigned long long)g_st.flash_writes,
			    (unsigned long long)g_st.flash_erases);
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

static void draw_library(void)
{
	grid_clear();
	draw_header();
	if (g_bk_detail) {
		draw_book();
		return;
	}
	switch (g_tab) {
	case TAB_BOOKS:
		draw_book_list();
		break;
	case TAB_LOANS:
		draw_loan_list();
		break;
	default:
		draw_store();
		break;
	}
}

/* -- actions ------------------------------------------------------------- */

/* Fill in batches, so progress shows and F3 can stop between them; each
 * batch is published by the scenario layer before the next starts. */
#define FILL_BATCH 16u

static void act_fill(void)
{
	uint32_t left = library_scenario_books_defined() -
			MIN(g_nb, library_scenario_books_defined());
	uint32_t want = MIN(200u, left);
	uint32_t done = 0;
	int64_t t0;
	bool stopped = false;
	char l1[64], l2[64];
	int rc = 0;

	if (!g_open) {
		status(ST_BAD, "fill: store not open");
		return;
	}
	if (left == 0) {
		message("Fill", ST_NORMAL, "The store already holds the whole",
			"catalogue the generator defines.");
		return;
	}
	snprintf(l1, sizeof(l1), "Books to add (1..%u):", left);
	if (!input_number("Fill", l1, &want, left)) {
		return;
	}
	t0 = k_uptime_get();    /* after the prompt: time the writing only */
	while (done < want && !stopped) {
		uint32_t w = 0;

		stopped = busy("Fill", "Writing books and loans...", done, want);
		if (stopped) {
			break;
		}
		rc = library_scenario_fill(&g_s, MIN(FILL_BATCH, want - done), &w);
		done += w;
		if (rc != 0 || w == 0) {
			break;
		}
	}
	store_refresh();

	snprintf(l1, sizeof(l1), "%u books written in %lld ms%s", done,
		 (long long)(k_uptime_get() - t0), stopped ? " (stopped)" : "");
	snprintf(l2, sizeof(l2), "store holds %u books, %u on loan", g_nb, g_na);
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "fill: %s; %s", l1, l2);
	if (rc != 0) {
		snprintf(l2, sizeof(l2), "fill stopped: %d (%s)", rc, errstr(rc));
		log_line(ST_BAD, "%s", l2);
	}
	message("Fill", rc != 0 ? ST_BAD : ST_OK, l1, l2);
}

static void act_verify(void)
{
	struct library_verify_report v;
	char l1[64], l2[64];
	bool pass;
	int rc;

	if (!g_open) {
		status(ST_BAD, "verify: store not open");
		return;
	}
	busy("Verify", "Checking books and loans...", 0, 0);
	rc = library_scenario_verify(&g_s, &v);
	if (rc != 0) {
		log_line(ST_BAD, "verify: %d (%s)", rc, errstr(rc));
		return;
	}
	pass = (v.bad == 0);
	snprintf(l1, sizeof(l1), "%s: %u books, %u loans in %lld ms", pass ? "PASS" : "FAIL",
		 v.books_checked, v.loans_checked, (long long)v.ms);
	snprintf(l2, sizeof(l2), "%u records differ or disagree", v.bad);
	log_line(pass ? ST_OK : ST_BAD, "verify %s; %s", l1, l2);
	store_refresh();
	message("Verify", pass ? ST_OK : ST_BAD, l1, l2);
}

static void act_reset(void)
{
	int rc;

	if (!confirm("Reset", "Erase every book and loan?",
		     "The other stores are kept.")) {
		return;
	}
	busy("Reset", "Erasing the library store...", 0, 0);
	rc = g_open ? library_scenario_erase(&g_s) : library_scenario_open(&g_s);
	if (!g_open && rc == 0) {
		rc = library_scenario_erase(&g_s);
	}
	g_open = (rc == 0);
	g_open_rc = rc;
	g_bk_cur = 0;
	g_bk_top = 0;
	g_ln_cur = 0;
	g_ln_top = 0;
	g_bk_detail = false;
	store_refresh();
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "reset: %d (%s)", rc, errstr(rc));
	status(rc != 0 ? ST_BAD : ST_OK, "reset: %s", errstr(rc));
}

static void upper(char *s)
{
	for (; *s != '\0'; s++) {
		*s = (char)toupper((unsigned char)*s);
	}
}

/* After a change: the counts, the caches and the book on screen. */
static void after_write(int rc, const char *what)
{
	store_refresh();
	book_open(g_bk_index);
	if (rc != 0) {
		status(ST_BAD, "%s: %d (%s)", what, rc, errstr(rc));
	}
}

static void act_lend(void)
{
	char card[LIBRARYDB_CARD_MAX + 1] = "";
	char name[LIBRARYDB_NAME_MAX + 1] = "";
	char d[16];
	int rc;

	if (g_bk.loan[g_copy_sel] != LIBRARYDB_NO_LOAN) {
		status(ST_WARN, "copy %d is already out", g_copy_sel + 1);
		return;
	}
	if (!input_text("Lend", "Borrower card, e.g. B10423:", card, sizeof(card), false,
			false) || card[0] == '\0') {
		return;
	}
	if (!input_text("Lend", "Borrower, SURNAME/GIVEN:", name, sizeof(name), false, false) ||
	    name[0] == '\0') {
		return;
	}
	upper(card);
	upper(name);
	rc = librarydb_lend(g_s.db, g_bk_index, (uint8_t)g_copy_sel, card, name, today(), NULL);
	after_write(rc, "lend");
	if (rc == 0) {
		status(ST_OK, "copy %d lent to %s, due %s", g_copy_sel + 1, card,
		       day_str((uint16_t)(today() + LIBRARYDB_LOAN_DAYS), d, sizeof(d)));
	}
}

static void act_return(void)
{
	int rc;

	if (g_bk.loan[g_copy_sel] == LIBRARYDB_NO_LOAN) {
		status(ST_WARN, "copy %d is on the shelf", g_copy_sel + 1);
		return;
	}
	rc = librarydb_return(g_s.db, g_bk_index, (uint8_t)g_copy_sel, today());
	after_write(rc, "return");
	if (rc == 0) {
		status(ST_OK, "copy %d returned", g_copy_sel + 1);
	}
}

static void act_renew(void)
{
	char d[16];
	int rc;

	if (g_bk.loan[g_copy_sel] == LIBRARYDB_NO_LOAN) {
		status(ST_WARN, "copy %d is on the shelf", g_copy_sel + 1);
		return;
	}
	rc = librarydb_renew(g_s.db, g_bk_index, (uint8_t)g_copy_sel, today());
	if (rc == -EDQUOT) {
		status(ST_BAD, "copy %d has been renewed %u times already", g_copy_sel + 1,
		       LIBRARYDB_RENEWALS_MAX);
		return;
	}
	after_write(rc, "renew");
	if (rc == 0) {
		status(ST_OK, "copy %d renewed, due %s", g_copy_sel + 1,
		       day_str(g_bk_loan[g_copy_sel].due, d, sizeof(d)));
	}
}

static bool starts_with(const char *s, const char *prefix)
{
	for (; *prefix != '\0'; s++, prefix++) {
		if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) {
			return false;
		}
	}
	return true;
}

/* The next book, after the cursor and round, whose title, author or call
 * number starts with the text, or whose ISBN is it. */
static void book_locate(void)
{
	char buf[LIBRARYDB_TITLE_MAX + 1] = "";

	if (!input_text("Locate", "Title, author, call no or ISBN:", buf, sizeof(buf), false,
			false) || buf[0] == '\0') {
		return;
	}
	busy("Locate", "Searching the catalogue...", 0, 0);
	for (uint32_t k = 1; k <= g_nb; k++) {
		uint32_t i = (g_bk_cur + k) % g_nb;
		const struct book_row *r = book_get(i);
		const struct librarydb_book *b = &r->b;

		if (r->rc == 0 &&
		    (starts_with(b->title, buf) || starts_with(b->author, buf) ||
		     starts_with(b->call, buf) || strcmp(b->isbn, buf) == 0 ||
		     (starts_with(b->title, "The ") && starts_with(b->title + 4, buf)))) {
			g_bk_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no book matches %s", buf);
}

/* The next loan still out and overdue, after the cursor and round. */
static void loan_next_overdue(void)
{
	busy("Overdue", "Searching the loans...", 0, 0);
	for (uint32_t k = 1; k <= g_nl; k++) {
		uint32_t i = (g_ln_cur + k) % g_nl;
		const struct loan_row *r = loan_get(i);

		if (r->rc == 0 && librarydb_status(&r->l, today()) == LIBRARYDB_OVERDUE) {
			g_ln_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_OK, "no loan is overdue");
}

/* -- key handling, per view ---------------------------------------------- */

/* Up, down, page and the ends, over a list of @p n. */
static bool keys_cursor(int k, uint32_t *cur, uint32_t n)
{
	uint32_t page = (uint32_t)MAX(body_bottom() - body_top() - 1, 1);

	switch (k) {
	case K_UP:
		*cur -= (*cur > 0) ? 1 : 0;
		return true;
	case K_DOWN:
		*cur += (*cur + 1 < n) ? 1 : 0;
		return true;
	case K_PGUP:
		*cur = (*cur > page) ? *cur - page : 0;
		return true;
	case K_PGDN:
		*cur = (n > 0) ? MIN(*cur + page, n - 1) : 0;
		return true;
	case K_HOME:
		*cur = 0;
		return true;
	case K_END:
		*cur = (n > 0) ? n - 1 : 0;
		return true;
	default:
		return false;
	}
}

static bool keys_book_list(int k)
{
	if (keys_cursor(k, &g_bk_cur, g_nb)) {
		return true;
	}
	switch (k) {
	case K_ENTER:
		if (g_open && g_nb > 0) {
			g_copy_sel = 0;
			book_open(g_bk_cur);
		}
		return true;
	case '/':
		if (g_nb > 0) {
			book_locate();
		}
		return true;
	case 'f':
		act_fill();
		return true;
	default:
		return false;
	}
}

static bool keys_book(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_bk_detail = false;
		return true;
	case K_UP:
		g_copy_sel -= (g_copy_sel > 0) ? 1 : 0;
		return true;
	case K_DOWN:
		g_copy_sel += (g_copy_sel + 1 < g_bk.copies) ? 1 : 0;
		return true;
	case K_PGUP:
		if (g_bk_index > 0) {
			g_copy_sel = 0;
			book_open(g_bk_index - 1);
			g_bk_cur = g_bk_index;
		}
		return true;
	case K_PGDN:
		if (g_bk_index + 1 < g_nb) {
			g_copy_sel = 0;
			book_open(g_bk_index + 1);
			g_bk_cur = g_bk_index;
		}
		return true;
	case 'l':
		if (g_bk_rc == 0) {
			act_lend();
		}
		return true;
	case 'r':
		if (g_bk_rc == 0) {
			act_return();
		}
		return true;
	case 'n':
		if (g_bk_rc == 0) {
			act_renew();
		}
		return true;
	default:
		return false;
	}
}

static bool keys_loan_list(int k)
{
	if (keys_cursor(k, &g_ln_cur, g_nl)) {
		return true;
	}
	switch (k) {
	case K_ENTER:
		if (g_open && g_nl > 0) {
			const struct loan_row *r = loan_get(g_ln_cur);

			if (r->rc == 0) {
				g_copy_sel = r->l.copy;
				book_open(r->l.book);
				g_bk_cur = g_bk_index;
			}
		}
		return true;
	case 'o':
		if (g_nl > 0) {
			loan_next_overdue();
		}
		return true;
	case 'f':
		act_fill();
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
		case 2:
			store_refresh();
			status(ST_NORMAL, "statistics refreshed");
			break;
		default: act_reset(); break;
		}
		return true;
	default:
		return false;
	}
}

static void store_close(void)
{
	if (g_open) {
		library_scenario_close(&g_s);
		g_open = false;
	}
}

int tui_library_count(uint32_t *books, uint32_t *on_loan)
{
	struct library_scenario s;
	int rc = library_scenario_open(&s);

	if (rc == 0) {
		librarydb_counts(s.db, books, NULL, on_loan);
		library_scenario_close(&s);
	}
	return rc;
}

void tui_library_run(void)
{
	draw = draw_library;
	g_tab = TAB_BOOKS;
	g_bk_detail = false;

	g_open_rc = library_scenario_open(&g_s);
	g_open = (g_open_rc == 0);
	store_refresh();
	if (!g_open) {
		g_tab = TAB_STORE;
		status(ST_BAD, "open failed: %d (%s)", g_open_rc, errstr(g_open_rc));
	}

	for (;;) {
		bool used;
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

		if (g_bk_detail) {
			used = keys_book(k);
		} else if (g_tab == TAB_BOOKS) {
			used = keys_book_list(k);
		} else if (g_tab == TAB_LOANS) {
			used = keys_loan_list(k);
		} else {
			used = keys_store(k);
		}
		if (used) {
			continue;
		}

		switch (k) {
		case K_TAB:
		case K_RIGHT:
			g_tab = (g_tab + 1) % TAB__COUNT;
			g_bk_detail = false;
			break;
		case K_BTAB:
		case K_LEFT:
			g_tab = (g_tab + TAB__COUNT - 1) % TAB__COUNT;
			g_bk_detail = false;
			break;
		case '1': case '2': case '3':
			g_tab = (enum tab)(k - '1');
			g_bk_detail = false;
			break;
		case K_REDRAW:
			detect_size();
			break;
		case 's':
			store_close();
			return;
		case 'q':
		case K_INTR:
			sign_off(store_close);
			break;
		default:
			break;
		}
	}
}
