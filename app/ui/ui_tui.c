/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui — the person database, as full-screen views on a serial terminal.
 *
 * Parsing and printing only, like ui_shell.c (DESIGN.md F15): every action is
 * one call into scenario.h or persondb.h. What this frontend adds is
 * navigation — arrow keys over the persons, their permissions and their cards
 * — on any VT100-compatible terminal attached to the console UART, with no
 * tool on the host side. The terminal itself is tui.c; the sign-on screen in
 * ui_tui_main.c starts this one and gets the terminal back on S.
 *
 * Names are shown as SURNAME/GIVEN, as a PNR spells them.
 *
 * Runs on the main thread, whose stack prj.conf sizes for the scenario calls;
 * everything large here is static.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "scenario.h"
#include "tui.h"
#include "ui_tui_apps.h"

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
static struct scenario_report g_rep;
static int g_rep_rc = -EAGAIN;
static int g_store_cur;

/* Scratch for single reads, kept off the stack. */
static struct persondb_person g_tmp;

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

static void draw_footer(const char *const *hints, size_t n)
{
	char right[48];

	if (!g_open) {
		tui_footer(hints, n, " 1A  X SYSTEM  STORE NOT OPEN", "");
		return;
	}
	snprintf(right, sizeof(right), "REV %u  %u/%u PERSONS  LU PDB01 ", g_st.rev,
		 g_st.populated, g_st.n_persons);
	tui_footer(hints, n, " 1A  ONLINE", right);
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
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
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
		"S", "SYSTEM", "Q", "SIGN OFF",
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

static void draw_persons(void)
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

	/* scenario_erase() wipes the whole medium, so the flight store goes
	 * too; say so, since it is not shown from here. */
	if (!confirm("Reset", "Erase every person and card?",
		     "And any flight data. Cannot be undone.")) {
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

static void store_close(void)
{
	if (g_open) {
		scenario_close(&g_s);
		g_open = false;
	}
}

static void quit(void)
{
	sign_off(store_close);
}

int tui_persons_count(uint32_t *persons)
{
	struct scenario s;
	uint32_t rev;
	int rc = scenario_open(&s);

	if (rc == 0) {
		persondb_progress_get(s.db, persons, &rev);
		scenario_close(&s);
	}
	return rc;
}

void tui_persons_run(void)
{
	draw = draw_persons;
	g_tab = TAB_PEOPLE;
	g_detail = false;
	rows_invalidate();

	g_open_rc = scenario_open(&g_s);
	g_open = (g_open_rc == 0);
	stat_refresh();
	strcpy(g_chk_perm, scenario_permission(0));
	if (!g_open) {
		g_tab = TAB_STORE;
		status(ST_BAD, "open failed: %d (%s)", g_open_rc, errstr(g_open_rc));
	}

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
		case 's':
			if (TUI_STANDALONE) {
				break;
			}
			store_close();
			return;
		case 'q':
		case K_INTR:
			quit();
			break;
		default:
			break;
		}
	}
}
