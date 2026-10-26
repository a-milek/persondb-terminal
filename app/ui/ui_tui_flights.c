/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_flights — the flight database, as full-screen views on a serial
 * terminal: the departure schedule, each flight, the airports and their
 * departures and arrivals, and the store utilities.
 *
 * Parsing and printing only, like ui_tui.c: every record shown is read from
 * flightdb, so the screens show exactly what the store holds — an empty store
 * shows empty screens until a fill writes the schedule. The terminal itself
 * is tui.c; the sign-on screen in ui_tui_main.c starts this one and gets the
 * terminal back on S.
 *
 * Runs on the main thread; everything large here is static.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "flight_scenario.h"
#include "flightdb.h"
#include "tui.h"
#include "ui_tui_apps.h"

/* -- application state --------------------------------------------------- */

enum tab { TAB_FLIGHTS, TAB_AIRPORTS, TAB_STORE, TAB__COUNT };

static const char *const tab_name[TAB__COUNT] = {
	[TAB_FLIGHTS]  = "FLIGHTS",
	[TAB_AIRPORTS] = "AIRPORTS",
	[TAB_STORE]    = "UTILITIES",
};

static struct flight_scenario g_s;
static bool g_open;
static int g_open_rc;
static uint32_t g_na, g_nf;   /* records the store holds */
static enum tab g_tab;

/* Airports are few and every flight row names two, so all of them are kept
 * in RAM, reloaded after each write. */
static struct flightdb_airport g_apts[FLIGHTDB_AIRPORTS_MAX];
static int g_apts_rc;

/* Flight rows, read through a small cache as the person list is. */
#define ROW_CACHE 64

struct row {
	bool     valid;
	uint32_t index;
	int      rc;
	struct flightdb_flight f;
};

static struct row g_rows[ROW_CACHE];
static uint32_t g_fl_cur;
static uint32_t g_fl_top;
static bool g_fl_seen;        /* placed at the next departure once */

/* The flight view, over either tab. */
static bool g_fl_detail;
static uint32_t g_fl_index;
static int g_fl_rc;
static struct flightdb_flight g_fl;

/* Airports. The view lists the airport's flights, departures in time order
 * and arrivals by STA, as positions in the flight store. */
static uint32_t g_apt_cur;
static uint32_t g_apt_top;
static bool g_apt_detail;
static int g_apt_pane;        /* 0: departures, 1: arrivals */
static int g_apt_sel[2];
static uint16_t g_apt_dep[FLIGHTDB_FLIGHTS_MAX];
static uint16_t g_apt_arr[FLIGHTDB_FLIGHTS_MAX];
static uint16_t g_apt_sta[FLIGHTDB_FLIGHTS_MAX];
static int g_apt_dep_n;
static int g_apt_arr_n;

/* Utilities. */
static struct flightdb_stat g_st;
static int g_store_cur;

/* Scratch for single reads, kept off the stack. */
static struct flightdb_flight g_tmp;

/* -- the clock ----------------------------------------------------------- */

static uint32_t now_epoch(void)
{
	return FLIGHT_SCENARIO_NOW + (uint32_t)(k_uptime_get() / 1000);
}

static uint32_t now_minute(void)
{
	return (now_epoch() / 60u) % FLIGHTDB_MIN_PER_DAY;
}

/* A minute of the day as 1320; out-of-range minutes wrap into the day. */
static const char *hhmm(int min, char *out)
{
	int m = ((min % (int)FLIGHTDB_MIN_PER_DAY) + (int)FLIGHTDB_MIN_PER_DAY) %
		(int)FLIGHTDB_MIN_PER_DAY;

	snprintf(out, 5, "%02d%02d", m / 60, m % 60);
	return out;
}

/* -- reading the store --------------------------------------------------- */

static void airports_load(void)
{
	g_apts_rc = 0;
	for (uint32_t i = 0; i < g_na; i++) {
		int rc = flightdb_airport_get(g_s.db, i, &g_apts[i]);

		if (rc != 0) {
			memset(&g_apts[i], 0, sizeof(g_apts[i]));
			snprintf(g_apts[i].name, sizeof(g_apts[i].name), "read failed: %d", rc);
			g_apts_rc = rc;
		}
	}
}

static void store_refresh(void)
{
	g_na = 0;
	g_nf = 0;
	if (g_open) {
		flightdb_counts(g_s.db, &g_na, &g_nf);
		flightdb_stat(g_s.db, &g_st);
		airports_load();
	} else {
		memset(&g_st, 0, sizeof(g_st));
	}
	for (int i = 0; i < ROW_CACHE; i++) {
		g_rows[i].valid = false;
	}
}

static const struct row *row_get(uint32_t index)
{
	struct row *r = &g_rows[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = flightdb_flight_get(g_s.db, index, &r->f);
	return r;
}

/* The stored airport with this code, or NULL. */
static const struct flightdb_airport *apt_by_code(const char *code)
{
	for (uint32_t i = 0; i < g_na; i++) {
		if (strcmp(g_apts[i].code, code) == 0) {
			return &g_apts[i];
		}
	}
	return NULL;
}

static const char *apt_city(const char *code)
{
	const struct flightdb_airport *a = apt_by_code(code);

	return a ? a->city : code;
}

static void flight_open(uint32_t index)
{
	g_fl_index = index;
	g_fl_rc = flightdb_flight_get(g_s.db, index, &g_fl);
	if (g_fl_rc != 0) {
		memset(&g_fl, 0, sizeof(g_fl));
	}
	g_fl_detail = true;
}

/*
 * The first flight not yet departed. The store is filled in departure order,
 * so this is a binary search — eight reads for a day's schedule, not 240. A
 * store written out of order would only make the answer approximate.
 */
static uint32_t next_departure(void)
{
	uint32_t lo = 0, hi = g_nf;
	uint32_t now = now_minute();

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const struct row *r = row_get(mid);

		if (r->rc == 0 && r->f.dep < now) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return (lo < g_nf) ? lo : 0;
}

/* -- drawing ------------------------------------------------------------- */

static void draw_header(void)
{
	static const char *const screen[TAB__COUNT] = {
		[TAB_FLIGHTS]  = "FLT100  DEPARTURE SCHEDULE",
		[TAB_AIRPORTS] = "FLT200  AIRPORT DIRECTORY",
		[TAB_STORE]    = "FLT900  SYSTEM UTILITIES",
	};
	static const char sys[] = "FLIGHT INFORMATION";
	uint32_t now = now_epoch();
	const char *code = screen[g_tab];
	char right[24], date[16];
	int c;
	int n;

	if (g_fl_detail) {
		code = "FLT110  FLIGHT INFORMATION DISPLAY";
	} else if (g_tab == TAB_AIRPORTS && g_apt_detail) {
		code = "FLT210  AIRPORT DISPLAY";
	}
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
	snprintf(right, sizeof(right), "%u FLIGHTS  %u AIRPORTS  LU FLT01 ", g_nf, g_na);
	tui_footer(hints, n, " 1A  ONLINE", right);
}

static bool draw_unavailable(int r, uint32_t n, const char *what)
{
	if (!g_open) {
		put(r, 2, cols - 4, ST_BAD, "The flight store is not open: %d (%s).", g_open_rc,
		    errstr(g_open_rc));
		put(r + 2, 2, cols - 4, ST_NORMAL,
		    "A store this build cannot read can be reset from Utilities.");
		return true;
	}
	if (n == 0) {
		put(r, 2, cols - 4, ST_NORMAL, "The flight store holds no %s.", what);
		put(r + 2, 2, cols - 4, ST_NORMAL,
		    "Press F to load the schedule, or go to Utilities.");
		return true;
	}
	return false;
}

static uint8_t status_style(enum flightdb_status s)
{
	switch (s) {
	case FLIGHTDB_CANCELLED:  return ST_BAD;
	case FLIGHTDB_DELAYED:    return ST_WARN;
	case FLIGHTDB_BOARDING:   return ST_BOLD;
	case FLIGHTDB_FINAL_CALL: return ST_BAD;
	case FLIGHTDB_DEPARTED:
	case FLIGHTDB_LANDED:     return ST_DIM;
	default:                  return ST_NORMAL;
	}
}

/* EST for a list: blank when on time, dashes when cancelled. */
static const char *est_str(const struct flightdb_flight *f, char *out)
{
	if (f->cancelled) {
		return strcpy(out, "----");
	}
	if (f->delay == 0) {
		return strcpy(out, "");
	}
	return hhmm(f->dep + f->delay, out);
}

static void draw_flight_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"N", "NOW", "F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int dw = CLAMP(cols - 57, 10, 20);
	uint32_t now = now_minute();

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nf, "flights")) {
		return;
	}
	g_fl_cur = MIN(g_fl_cur, g_nf - 1);
	if (g_fl_cur < g_fl_top) {
		g_fl_top = g_fl_cur;
	} else if (g_fl_cur >= g_fl_top + (uint32_t)h) {
		g_fl_top = g_fl_cur - (uint32_t)h + 1;
	}

	put(top, 0, cols, ST_TITLE, "  %-7s %-3s %-3s  %-*s  %4s  %4s  %-4s  %-4s  %s", "FLIGHT",
	    "FRM", "TO", dw, "DESTINATION", "STD", "EST", "EQPT", "GATE", "STATUS");
	for (int i = 0; i < h && g_fl_top + (uint32_t)i < g_nf; i++) {
		uint32_t index = g_fl_top + (uint32_t)i;
		bool sel = (index == g_fl_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct row *r = row_get(index);
		const struct flightdb_flight *f = &r->f;
		enum flightdb_status fs;
		char std[5], est[5];
		int c;

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %06u  read failed: %d (%s)", index, r->rc, errstr(r->rc));
			continue;
		}
		fs = flightdb_status(f, now);
		c = put(top + 1 + i, 0, cols, st, "  %-7s %-3s %-3s  %-*.*s  %4s  %4s  %-4s  %-4s  ",
			f->desig, f->from, f->to, dw, dw, apt_city(f->to), hhmm(f->dep, std),
			est_str(f, est), f->equipment, f->gate);
		put(top + 1 + i, c, cols - c, sel ? ST_SEL : status_style(fs), "%s",
		    flightdb_status_name(fs));
	}
	{
		char pos[32];
		int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", g_fl_cur + 1, g_nf);

		putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
	}
}

static void draw_airport_lines(int line, int last, int c, int w,
			       const char *code)
{
	const struct flightdb_airport *p = apt_by_code(code);

	if (line + 1 > last) {
		return;
	}
	if (p == NULL) {
		put(line, c, w, ST_BOLD, "%s", code);
		put(line + 1, c, w, ST_DIM, "not in the store");
		return;
	}
	put(line, c, w, ST_BOLD, "%s %s", p->code, p->name);
	put(line + 1, c, w, ST_NORMAL, "%s, %s  UTC%+d", p->city, p->country, p->utc_offset);
}

static void draw_flight(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "A", "DEP AIRPORT", "Z", "ARR AIRPORT",
		"Q", "SIGN OFF",
	};
	static const char *const cabin[FLIGHTDB_CABINS] = { "F FIRST", "C BUSINESS", "Y ECONOMY" };
	const struct flightdb_flight *f = &g_fl;
	const struct flightdb_airport *a, *b;
	enum flightdb_status fs;
	int top = body_top();
	int arr = f->dep + f->block;
	char t1[5], t2[5];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_fl_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Flight at position %u: %d (%s).", g_fl_index,
		    g_fl_rc, errstr(g_fl_rc));
		return;
	}
	a = apt_by_code(f->from);
	b = apt_by_code(f->to);
	fs = flightdb_status(f, now_minute());

	put(top, 2, cols - 4, ST_BOLD, "1.%s  %s", f->desig, f->airline);
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_fl_index + 1, g_nf);
	put(top + 1, 2, cols - 4, ST_NORMAL, "ROUTE       %s %s - %s %s", f->from,
	    apt_city(f->from), f->to, apt_city(f->to));
	put(top + 2, 2, cols - 4, ST_NORMAL, "EQUIPMENT   %-4s  DISTANCE %u NM  BLOCK %uH%02u",
	    f->equipment, f->distance, f->block / 60u, f->block % 60u);
	/* Local times need the airport's offset, which only a stored airport has. */
	put(top + 3, 2, cols - 4, ST_NORMAL, "DEPARTURE   STD %sZ", hhmm(f->dep, t1));
	if (a != NULL) {
		put(top + 3, 24, 8, ST_NORMAL, "%sLT", hhmm(f->dep + a->utc_offset * 60, t2));
	}
	put(top + 4, 2, cols - 4, ST_NORMAL, "ARRIVAL     STA %sZ", hhmm(arr, t1));
	if (b != NULL) {
		put(top + 4, 24, 8, ST_NORMAL, "%sLT", hhmm(arr + b->utc_offset * 60, t2));
	}
	if (f->cancelled) {
		put(top + 3, 46, cols, ST_BAD, "NOT OPERATING");
	} else if (f->delay > 0) {
		put(top + 3, 46, cols, ST_WARN, "ETD %sZ  +%u", hhmm(f->dep + f->delay, t1),
		    f->delay);
		put(top + 4, 46, cols, ST_WARN, "ETA %sZ", hhmm(arr + f->delay, t1));
	}
	put(top + 5, 2, cols - 4, ST_NORMAL, "GATE        %-4s  TERMINAL %u", f->gate, f->terminal);
	put(top + 6, 2, 12, ST_NORMAL, "STATUS");
	put(top + 6, 14, cols, status_style(fs), "%s", flightdb_status_name(fs));

	{
		int r = top + 8;
		int h = body_bottom() - r + 1;
		int lw = cols / 2 - 1;
		int rw = cols - lw - 3;
		int line = r + 1;
		unsigned int sold = 0, total = 0;

		if (h < 4) {
			return;
		}
		box(r, 1, h, lw, ST_DIM, "Cabin");
		put(line++, 3, lw - 4, ST_TITLE, "%-11s %5s %5s %5s", "", "SEATS", "BKD", "AVL");
		for (int c = 0; c < FLIGHTDB_CABINS; c++) {
			if (f->seats[c] == 0 || line >= r + h - 1) {
				continue;
			}
			put(line++, 3, lw - 4, ST_NORMAL, "%-11s %5u %5u %5u", cabin[c], f->seats[c],
			    f->booked[c], f->seats[c] - f->booked[c]);
			sold += f->booked[c];
			total += f->seats[c];
		}
		if (line < r + h - 1) {
			put(line, 3, lw - 4, ST_BOLD, "%-11s %5u %5u %4u%%", "LOAD", total, sold,
			    total ? sold * 100u / total : 0u);
		}

		box(r, lw + 2, h, rw, ST_DIM, "Airports");
		draw_airport_lines(r + 1, r + h - 2, lw + 4, rw - 4, f->from);
		draw_airport_lines(r + 4, r + h - 2, lw + 4, rw - 4, f->to);
	}
}

static void draw_airport_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "/", "LOCATE", "F", "FILL", "TAB", "NEXT SCREEN",
		"S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int nw = CLAMP(cols - 52, 10, 24);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_na, "airports")) {
		return;
	}
	g_apt_cur = MIN(g_apt_cur, g_na - 1);
	if (g_apt_cur < g_apt_top) {
		g_apt_top = g_apt_cur;
	} else if (g_apt_cur >= g_apt_top + (uint32_t)h) {
		g_apt_top = g_apt_cur - (uint32_t)h + 1;
	}

	put(top, 0, cols, ST_TITLE, "  %-4s  %-*s  %-15s  %-15s %4s %5s %3s", "CODE", nw, "NAME",
	    "CITY", "COUNTRY", "UTC", "ELEV", "RWY");
	for (int i = 0; i < h && g_apt_top + (uint32_t)i < g_na; i++) {
		uint32_t idx = g_apt_top + (uint32_t)i;
		const struct flightdb_airport *p = &g_apts[idx];
		uint8_t st = (idx == g_apt_cur) ? ST_SEL : ST_NORMAL;

		fill(top + 1 + i, 0, cols, ' ', st);
		put(top + 1 + i, 0, cols, st, "  %-4s  %-*.*s  %-15.15s  %-15.15s %+4d %5u %3u",
		    p->code, nw, nw, p->name, p->city, p->country, p->utc_offset, p->elevation,
		    p->runways);
	}
}

static const char *apt_item(int pane, int i)
{
	static char buf[48];
	uint32_t index = (pane == 0) ? g_apt_dep[i] : g_apt_arr[i];
	const struct row *r = row_get(index);
	const struct flightdb_flight *f = &r->f;
	enum flightdb_status fs;
	char t[5];

	if (r->rc != 0) {
		snprintf(buf, sizeof(buf), "read failed: %d", r->rc);
		return buf;
	}
	fs = flightdb_status(f, now_minute());
	snprintf(buf, sizeof(buf), "%s  %-7s %s  %s",
		 hhmm((pane == 0) ? f->dep : f->dep + f->block, t), f->desig,
		 (pane == 0) ? f->to : f->from, flightdb_status_name(fs));
	return buf;
}

static const char *apt_dep_item(int i)
{
	return apt_item(0, i);
}

static const char *apt_arr_item(int i)
{
	return apt_item(1, i);
}

static void draw_airport(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "TAB", "PANE", "ENTER", "FLIGHT", "F7/F8", "PREV/NEXT",
		"Q", "SIGN OFF",
	};
	const struct flightdb_airport *p = &g_apts[g_apt_cur];
	int top = body_top();
	char t[5];

	draw_footer(hints, ARRAY_SIZE(hints));
	put(top, 2, cols - 4, ST_BOLD, "1.%s  %s", p->code, p->name);
	put(top, 46, cols, ST_NORMAL, "LOCAL TIME %s",
	    hhmm((int)now_minute() + p->utc_offset * 60, t));
	put(top + 1, 2, cols - 4, ST_NORMAL, "CITY        %s, %s", p->city, p->country);
	put(top + 2, 2, cols - 4, ST_NORMAL, "FIELD       UTC%+d  ELEV %u FT  %u RWY  %u GATES",
	    p->utc_offset, p->elevation, p->runways, p->gates);

	{
		int r = top + 4;
		int h = body_bottom() - r + 1;
		int lw = cols / 2 - 1;
		char title[32];

		snprintf(title, sizeof(title), "Departures (%d)", g_apt_dep_n);
		draw_list_pane(r, 1, h, lw, title, g_apt_pane == 0, g_apt_sel[0], g_apt_dep_n,
			       apt_dep_item);
		snprintf(title, sizeof(title), "Arrivals (%d)", g_apt_arr_n);
		draw_list_pane(r, lw + 2, h, cols - lw - 3, title, g_apt_pane == 1, g_apt_sel[1],
			       g_apt_arr_n, apt_arr_item);
	}
}

static const char *const store_actions[] = {
	"Fill flights...",
	"Verify the store",
	"Refresh statistics",
	"Reset flight store",
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
		put(r++, 2, ac - 4, ST_NORMAL, "Airports     %u of %u", g_st.airports,
		    FLIGHTDB_AIRPORTS_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Flights      %u of %u", g_st.flights,
		    FLIGHTDB_FLIGHTS_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Partition    %zu B", g_st.partition_bytes);
		put(r++, 2, ac - 4, ST_NORMAL, "Maps         %u + %u buckets", g_st.airport_buckets,
		    g_st.flight_buckets);
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
		if (g_apts_rc != 0) {
			put(r++, 2, ac - 4, ST_BAD, "Airports     read failed: %d", g_apts_rc);
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

static void draw_flights(void)
{
	grid_clear();
	draw_header();
	if (g_fl_detail) {
		draw_flight();
		return;
	}
	switch (g_tab) {
	case TAB_FLIGHTS:
		draw_flight_list();
		break;
	case TAB_AIRPORTS:
		if (g_apt_detail) {
			draw_airport();
		} else {
			draw_airport_list();
		}
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
	uint32_t left = flight_scenario_flights_defined() - MIN(g_nf,
								 flight_scenario_flights_defined());
	uint32_t want = left;
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
			"schedule the generator defines.");
		return;
	}
	snprintf(l1, sizeof(l1), "Flights to add (1..%u):", left);
	if (!input_number("Fill", l1, &want, left)) {
		return;
	}
	t0 = k_uptime_get();    /* after the prompt: time the writing only */
	while (done < want && !stopped) {
		uint32_t w = 0;

		stopped = busy("Fill", "Writing airports and flights...", done, want);
		if (stopped) {
			break;
		}
		rc = flight_scenario_fill(&g_s, MIN(FILL_BATCH, want - done), &w);
		done += w;
		if (rc != 0 || w == 0) {
			break;
		}
	}
	store_refresh();

	snprintf(l1, sizeof(l1), "%u flights written in %lld ms%s", done,
		 (long long)(k_uptime_get() - t0), stopped ? " (stopped)" : "");
	snprintf(l2, sizeof(l2), "store holds %u airports, %u flights", g_na, g_nf);
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "fill: %s; %s", l1, l2);
	if (rc != 0) {
		snprintf(l2, sizeof(l2), "fill stopped: %d (%s)", rc, errstr(rc));
		log_line(ST_BAD, "%s", l2);
	}
	message("Fill", rc != 0 ? ST_BAD : ST_OK, l1, l2);
}

static void act_verify(void)
{
	struct flight_verify_report v;
	char l1[64], l2[64];
	bool pass;
	int rc;

	if (!g_open) {
		status(ST_BAD, "verify: store not open");
		return;
	}
	busy("Verify", "Comparing the store with the generator...", 0, 0);
	rc = flight_scenario_verify(&g_s, &v);
	if (rc != 0) {
		log_line(ST_BAD, "verify: %d (%s)", rc, errstr(rc));
		return;
	}
	pass = (v.bad == 0);
	snprintf(l1, sizeof(l1), "%s: %u airports, %u flights in %lld ms", pass ? "PASS" : "FAIL",
		 v.airports_checked, v.flights_checked, (long long)v.ms);
	snprintf(l2, sizeof(l2), "%u records differ or are unreadable", v.bad);
	log_line(pass ? ST_OK : ST_BAD, "verify %s; %s", l1, l2);
	store_refresh();
	message("Verify", pass ? ST_OK : ST_BAD, l1, l2);
}

static void act_reset(void)
{
	int rc;

	if (!confirm("Reset", "Erase every airport and flight?",
		     "Persons in the other store are kept.")) {
		return;
	}
	busy("Reset", "Erasing the flight store...", 0, 0);
	rc = g_open ? flight_scenario_erase(&g_s) : flight_scenario_open(&g_s);
	if (!g_open && rc == 0) {
		rc = flight_scenario_erase(&g_s);
	}
	g_open = (rc == 0);
	g_open_rc = rc;
	g_fl_cur = 0;
	g_fl_top = 0;
	g_fl_detail = false;
	g_apt_cur = 0;
	g_apt_detail = false;
	store_refresh();
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "reset: %d (%s)", rc, errstr(rc));
	status(rc != 0 ? ST_BAD : ST_OK, "reset: %s", errstr(rc));
}

/* Collect the airport's flights. Every stored flight is read once, so this
 * costs one read per flight; the progress popup says so on the board. */
static void airport_load(uint32_t a)
{
	const char *code;
	uint32_t now = now_minute();

	g_apt_cur = a;
	code = g_apts[a].code;
	g_apt_dep_n = 0;
	g_apt_arr_n = 0;
	g_apt_sel[0] = 0;
	g_apt_sel[1] = 0;
	busy("Airport", "Reading the schedule...", 0, 0);
	for (uint32_t i = 0; i < g_nf; i++) {
		const struct flightdb_flight *f;

		if (flightdb_flight_get(g_s.db, i, &g_tmp) != 0) {
			continue;
		}
		f = &g_tmp;
		if (strcmp(f->from, code) == 0) {
			if (f->dep < now) {
				g_apt_sel[0] = g_apt_dep_n + 1;
			}
			g_apt_dep[g_apt_dep_n++] = (uint16_t)i;
		}
		if (strcmp(f->to, code) == 0) {
			/* Insertion by arrival time, wrapped into the day. */
			uint16_t t = (uint16_t)((f->dep + f->block) % FLIGHTDB_MIN_PER_DAY);
			int j = g_apt_arr_n++;

			while (j > 0 && g_apt_sta[j - 1] > t) {
				g_apt_sta[j] = g_apt_sta[j - 1];
				g_apt_arr[j] = g_apt_arr[j - 1];
				j--;
			}
			g_apt_sta[j] = t;
			g_apt_arr[j] = (uint16_t)i;
			if (t < now) {
				g_apt_sel[1]++;
			}
		}
	}
	g_apt_sel[0] = MIN(g_apt_sel[0], MAX(g_apt_dep_n - 1, 0));
	g_apt_sel[1] = MIN(g_apt_sel[1], MAX(g_apt_arr_n - 1, 0));
	g_apt_detail = true;
}

static void airport_open(const char *code)
{
	for (uint32_t i = 0; i < g_na; i++) {
		if (strcmp(g_apts[i].code, code) == 0) {
			g_fl_detail = false;
			g_tab = TAB_AIRPORTS;
			g_apt_pane = 0;
			airport_load(i);
			return;
		}
	}
	status(ST_WARN, "airport %s is not in the store", code);
}

static void flight_locate(void)
{
	char buf[FLIGHTDB_DESIG_MAX + 1] = "";
	char want[FLIGHTDB_DESIG_MAX + 1];
	size_t n = 0;

	if (!input_text("Locate", "Flight, e.g. XA0421:", buf, sizeof(buf), false, false)) {
		return;
	}
	for (const char *p = buf; *p != '\0'; p++) {
		if (*p != ' ') {
			want[n++] = (char)toupper((unsigned char)*p);
		}
	}
	want[n] = '\0';
	busy("Locate", "Searching the schedule...", 0, 0);
	for (uint32_t i = 0; i < g_nf; i++) {
		const struct row *r = row_get(i);

		if (r->rc == 0 && strcmp(r->f.desig, want) == 0) {
			g_fl_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no flight %s in the store", want);
}

static void airport_locate(void)
{
	char buf[FLIGHTDB_CODE_LEN + 1] = "";

	if (!input_text("Locate", "Airport code:", buf, sizeof(buf), false, false)) {
		return;
	}
	for (char *p = buf; *p != '\0'; p++) {
		*p = (char)toupper((unsigned char)*p);
	}
	for (uint32_t i = 0; i < g_na; i++) {
		if (strcmp(g_apts[i].code, buf) == 0) {
			g_apt_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no airport %s in the store", buf);
}

/* -- key handling, per view ---------------------------------------------- */

static bool keys_flight_list(int k)
{
	uint32_t n = g_nf;
	uint32_t page = (uint32_t)MAX(body_bottom() - body_top() - 1, 1);

	switch (k) {
	case K_UP:
		g_fl_cur -= (g_fl_cur > 0) ? 1 : 0;
		return true;
	case K_DOWN:
		g_fl_cur += (g_fl_cur + 1 < n) ? 1 : 0;
		return true;
	case K_PGUP:
		g_fl_cur = (g_fl_cur > page) ? g_fl_cur - page : 0;
		return true;
	case K_PGDN:
		g_fl_cur = (n > 0) ? MIN(g_fl_cur + page, n - 1) : 0;
		return true;
	case K_HOME:
		g_fl_cur = 0;
		return true;
	case K_END:
		g_fl_cur = (n > 0) ? n - 1 : 0;
		return true;
	case K_ENTER:
		if (g_open && n > 0) {
			flight_open(g_fl_cur);
		}
		return true;
	case '/':
		if (n > 0) {
			flight_locate();
		}
		return true;
	case 'n':
		if (n > 0) {
			g_fl_cur = next_departure();
			g_fl_top = g_fl_cur;
		}
		return true;
	case 'f':
		act_fill();
		return true;
	default:
		return false;
	}
}

static bool keys_flight(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_fl_detail = false;
		return true;
	case K_PGUP:
		if (g_fl_index > 0) {
			flight_open(g_fl_index - 1);
			g_fl_cur = g_fl_index;
		}
		return true;
	case K_PGDN:
		if (g_fl_index + 1 < g_nf) {
			flight_open(g_fl_index + 1);
			g_fl_cur = g_fl_index;
		}
		return true;
	case 'a':
		if (g_fl_rc == 0) {
			airport_open(g_fl.from);
		}
		return true;
	case 'z':
		if (g_fl_rc == 0) {
			airport_open(g_fl.to);
		}
		return true;
	default:
		return false;
	}
}

static bool keys_airport_list(int k)
{
	uint32_t n = g_na;

	switch (k) {
	case K_UP:
		g_apt_cur = (g_apt_cur > 0) ? g_apt_cur - 1 : (n > 0 ? n - 1 : 0);
		return true;
	case K_DOWN:
		g_apt_cur = (g_apt_cur + 1 < n) ? g_apt_cur + 1 : 0;
		return true;
	case K_HOME:
		g_apt_cur = 0;
		return true;
	case K_END:
		g_apt_cur = (n > 0) ? n - 1 : 0;
		return true;
	case K_ENTER:
		if (g_open && n > 0) {
			g_apt_pane = 0;
			airport_load(g_apt_cur);
		}
		return true;
	case '/':
		if (n > 0) {
			airport_locate();
		}
		return true;
	case 'f':
		act_fill();
		return true;
	default:
		return false;
	}
}

static bool keys_airport(int k)
{
	int n = (g_apt_pane == 0) ? g_apt_dep_n : g_apt_arr_n;
	int *sel = &g_apt_sel[g_apt_pane];

	switch (k) {
	case K_ESC:
	case K_BS:
		g_apt_detail = false;
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
		g_apt_pane ^= 1;
		return true;
	case K_PGUP:
		airport_load((g_apt_cur + g_na - 1) % g_na);
		return true;
	case K_PGDN:
		airport_load((g_apt_cur + 1) % g_na);
		return true;
	case K_ENTER:
		if (n > 0) {
			flight_open((g_apt_pane == 0) ? g_apt_dep[*sel] : g_apt_arr[*sel]);
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
		flight_scenario_close(&g_s);
		g_open = false;
	}
}

/* The schedule opens at the next departure, the first time it is shown with
 * flights in it. */
static void place_schedule(void)
{
	if (!g_fl_seen && g_nf > 0) {
		g_fl_seen = true;
		g_fl_cur = next_departure();
		g_fl_top = g_fl_cur;
	}
}

int tui_flights_count(uint32_t *airports, uint32_t *flights)
{
	struct flight_scenario s;
	int rc = flight_scenario_open(&s);

	if (rc == 0) {
		flightdb_counts(s.db, airports, flights);
		flight_scenario_close(&s);
	}
	return rc;
}

void tui_flights_run(void)
{
	draw = draw_flights;
	g_tab = TAB_FLIGHTS;
	g_fl_detail = false;
	g_fl_seen = false;
	g_apt_detail = false;

	g_open_rc = flight_scenario_open(&g_s);
	g_open = (g_open_rc == 0);
	store_refresh();
	if (!g_open) {
		g_tab = TAB_STORE;
		status(ST_BAD, "open failed: %d (%s)", g_open_rc, errstr(g_open_rc));
	}

	for (;;) {
		bool used;
		int k;

		place_schedule();
		draw();
		present();
		k = getkey(1000);       /* a second without a key: redraw the clock */
		if (k == K_NONE) {
			continue;
		}
		if (k >= 'A' && k <= 'Z') {
			k = tolower(k);
		}

		if (g_fl_detail) {
			used = keys_flight(k);
		} else if (g_tab == TAB_FLIGHTS) {
			used = keys_flight_list(k);
		} else if (g_tab == TAB_AIRPORTS) {
			used = g_apt_detail ? keys_airport(k) : keys_airport_list(k);
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
			g_fl_detail = false;
			break;
		case K_BTAB:
		case K_LEFT:
			g_tab = (g_tab + TAB__COUNT - 1) % TAB__COUNT;
			g_fl_detail = false;
			break;
		case '1': case '2': case '3':
			g_tab = (enum tab)(k - '1');
			g_fl_detail = false;
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
			sign_off(store_close);
			break;
		default:
			break;
		}
	}
}
