/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_flights — the flight database, as full-screen views on a serial
 * terminal: the departure schedule, each flight with its aircraft, crew and
 * manifest, the airports and their departures and arrivals, the airlines and
 * their fleets, each aircraft's rotation, each crew member's duties, the
 * passengers and their bookings, each booking and its bags, and the store
 * utilities.
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

enum tab {
	TAB_FLIGHTS, TAB_AIRPORTS, TAB_AIRLINES, TAB_FLEET, TAB_CREW, TAB_PASSENGERS,
	TAB_BOOKINGS, TAB_BAGS, TAB_STORE, TAB__COUNT
};

static const char *const tab_name[TAB__COUNT] = {
	[TAB_FLIGHTS]    = "FLIGHTS",
	[TAB_AIRPORTS]   = "AIRPORTS",
	[TAB_AIRLINES]   = "AIRLINES",
	[TAB_FLEET]      = "FLEET",
	[TAB_CREW]       = "CREW",
	[TAB_PASSENGERS] = "PASSENGERS",
	[TAB_BOOKINGS]   = "BOOKINGS",
	[TAB_BAGS]       = "BAGGAGE",
	[TAB_STORE]      = "UTILITIES",
};

/* The tab bar on a terminal too narrow for the names. */
static const char *const tab_code[TAB__COUNT] = {
	[TAB_FLIGHTS]    = "FLT",
	[TAB_AIRPORTS]   = "APT",
	[TAB_AIRLINES]   = "AIRL",
	[TAB_FLEET]      = "FLEET",
	[TAB_CREW]       = "CREW",
	[TAB_PASSENGERS] = "PAX",
	[TAB_BOOKINGS]   = "PNR",
	[TAB_BAGS]       = "BAG",
	[TAB_STORE]      = "UTIL",
};

static struct flight_scenario g_s;
static bool g_open;
static int g_open_rc;
static uint32_t g_na, g_nf, g_np, g_nb;   /* records the store holds */
static uint32_t g_nl, g_nk, g_nc, g_ng;
static enum tab g_tab;

/* Airports are few and every flight row names two, so all of them are kept
 * in RAM, reloaded after each write. Airlines and aircraft too: they are few,
 * and a flight's aircraft is found by looking through every rotation. */
static struct flightdb_airport g_apts[FLIGHTDB_AIRPORTS_MAX];
static int g_apts_rc;
static struct flightdb_airline g_als[FLIGHTDB_AIRLINES_MAX];
static int g_als_rc;
static struct flightdb_aircraft g_acs[FLIGHTDB_AIRCRAFT_MAX];
static int g_acs_rc;

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

/* Passenger and booking rows, through caches like the flight rows. */
struct prow {
	bool     valid;
	uint32_t index;
	int      rc;
	struct flightdb_passenger p;
};

struct brow {
	bool     valid;
	uint32_t index;
	int      rc;
	struct flightdb_booking b;
};

static struct prow g_prows[ROW_CACHE];
static struct brow g_brows[ROW_CACHE];

/*
 * Who holds each booking and on which flight: what a manifest and a
 * passenger's bookings are picked from. Read from the store once, the first
 * time either is wanted, and dropped after every write; NO_REF marks a
 * booking that could not be read.
 */
#define NO_REF UINT16_MAX
static uint16_t g_bk_pax[FLIGHTDB_BOOKINGS_MAX];
static uint16_t g_bk_flt[FLIGHTDB_BOOKINGS_MAX];
static bool g_bk_index_ok;

/* Passengers. The view lists the passenger's bookings. */
#define PAX_BOOKINGS_MAX 32
static uint32_t g_px_cur;
static uint32_t g_px_top;
static bool g_px_detail;
static uint32_t g_px_index;
static int g_px_rc;
static struct flightdb_passenger g_px;
static uint16_t g_px_bk[PAX_BOOKINGS_MAX];
static int g_px_bk_n;
static int g_px_sel;

/* Bookings. */
static uint32_t g_bk_cur;
static uint32_t g_bk_top;
static bool g_bk_detail;
static uint32_t g_bk_index;
static int g_bk_rc;
static struct flightdb_booking g_bk;

/* The manifest popup over the flight view: positions in the booking store,
 * as many as the largest cabin layout has seats. */
#define MANIFEST_MAX 320
static uint16_t g_man[MANIFEST_MAX];
static int g_man_n;

/* Crew and bag rows, cached like the others. */
struct crow {
	bool     valid;
	uint32_t index;
	int      rc;
	struct flightdb_crew c;
};

struct grow {
	bool     valid;
	uint32_t index;
	int      rc;
	struct flightdb_bag g;
};

static struct crow g_crows[ROW_CACHE];
static struct grow g_grows[ROW_CACHE];

/* Which flights each crew member works, and whose booking each bag is: read
 * once when first wanted, like the booking index, and dropped after a write. */
static uint8_t g_cr_n[FLIGHTDB_CREW_MAX];
static uint16_t g_cr_duty[FLIGHTDB_CREW_MAX][FLIGHTDB_DUTIES_MAX];
static bool g_cr_index_ok;
static uint16_t g_bag_bk[FLIGHTDB_BAGS_MAX];
static bool g_bag_index_ok;

/* Airlines: the view lists the airline's fleet, as positions in g_acs. */
static uint32_t g_al_cur;
static bool g_al_detail;
static uint16_t g_al_fleet[FLIGHTDB_AIRCRAFT_MAX];
static int g_al_fleet_n;
static int g_al_sel;

/* Fleet: the view lists the aircraft's rotation. */
static uint32_t g_ac_cur;
static uint32_t g_ac_top;
static bool g_ac_detail;
static int g_ac_sel;

/* Crew: the view lists the duties. */
static uint32_t g_cr_cur;
static uint32_t g_cr_top;
static bool g_cr_detail;
static uint32_t g_cr_index;
static int g_cr_rc;
static struct flightdb_crew g_cr;
static int g_cr_sel;

/* Baggage. */
static uint32_t g_bg_cur;
static uint32_t g_bg_top;
static bool g_bg_detail;
static uint32_t g_bg_index;
static int g_bg_rc;
static struct flightdb_bag g_bg;

/* The crew popup over the flight view, and the booking view's bags. */
#define FLIGHT_CREW_MAX 16
static uint16_t g_pick[FLIGHT_CREW_MAX];
static int g_pick_n;
#define BOOKING_BAGS_MAX 8
static uint16_t g_bk_bags[BOOKING_BAGS_MAX];
static int g_bk_bags_n;

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

static void airlines_load(void)
{
	g_als_rc = 0;
	for (uint32_t i = 0; i < g_nl; i++) {
		int rc = flightdb_airline_get(g_s.db, i, &g_als[i]);

		if (rc != 0) {
			memset(&g_als[i], 0, sizeof(g_als[i]));
			snprintf(g_als[i].name, sizeof(g_als[i].name), "read failed: %d", rc);
			g_als_rc = rc;
		}
	}
}

static void aircraft_load(void)
{
	g_acs_rc = 0;
	for (uint32_t i = 0; i < g_nk; i++) {
		int rc = flightdb_aircraft_get(g_s.db, i, &g_acs[i]);

		if (rc != 0) {
			memset(&g_acs[i], 0, sizeof(g_acs[i]));
			snprintf(g_acs[i].reg, sizeof(g_acs[i].reg), "ERR%d", -rc % 100);
			g_acs_rc = rc;
		}
	}
}

static void store_refresh(void)
{
	struct flightdb_counts c = { 0 };

	if (g_open) {
		flightdb_counts(g_s.db, &c);
		flightdb_stat(g_s.db, &g_st);
	} else {
		memset(&g_st, 0, sizeof(g_st));
	}
	g_na = c.airports;
	g_nf = c.flights;
	g_np = c.passengers;
	g_nb = c.bookings;
	g_nl = c.airlines;
	g_nk = c.aircraft;
	g_nc = c.crew;
	g_ng = c.bags;
	if (g_open) {
		airports_load();
		airlines_load();
		aircraft_load();
	}
	for (int i = 0; i < ROW_CACHE; i++) {
		g_rows[i].valid = false;
		g_prows[i].valid = false;
		g_brows[i].valid = false;
		g_crows[i].valid = false;
		g_grows[i].valid = false;
	}
	g_bk_index_ok = false;
	g_cr_index_ok = false;
	g_bag_index_ok = false;
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

static const struct prow *prow_get(uint32_t index)
{
	struct prow *r = &g_prows[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = flightdb_passenger_get(g_s.db, index, &r->p);
	return r;
}

static const struct brow *brow_get(uint32_t index)
{
	struct brow *r = &g_brows[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = flightdb_booking_get(g_s.db, index, &r->b);
	return r;
}

/* Every booking read once; the progress popup says so on the board. */
static void booking_index_load(void)
{
	if (g_bk_index_ok) {
		return;
	}
	busy("Bookings", "Reading the booking store...", 0, 0);
	for (uint32_t i = 0; i < g_nb; i++) {
		const struct brow *r = brow_get(i);

		g_bk_pax[i] = (r->rc == 0) ? r->b.passenger : NO_REF;
		g_bk_flt[i] = (r->rc == 0) ? r->b.flight : NO_REF;
	}
	g_bk_index_ok = true;
}

static const struct crow *crow_get(uint32_t index)
{
	struct crow *r = &g_crows[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = flightdb_crew_get(g_s.db, index, &r->c);
	return r;
}

static const struct grow *grow_get(uint32_t index)
{
	struct grow *r = &g_grows[index % ROW_CACHE];

	if (r->valid && r->index == index) {
		return r;
	}
	r->valid = true;
	r->index = index;
	r->rc = flightdb_bag_get(g_s.db, index, &r->g);
	return r;
}

static void crew_index_load(void)
{
	if (g_cr_index_ok) {
		return;
	}
	busy("Crew", "Reading the crew store...", 0, 0);
	for (uint32_t i = 0; i < g_nc; i++) {
		const struct crow *r = crow_get(i);

		g_cr_n[i] = (r->rc == 0) ? r->c.n_duties : 0;
		if (r->rc == 0) {
			memcpy(g_cr_duty[i], r->c.duties, sizeof(g_cr_duty[i]));
		}
	}
	g_cr_index_ok = true;
}

static void bag_index_load(void)
{
	if (g_bag_index_ok) {
		return;
	}
	busy("Baggage", "Reading the baggage store...", 0, 0);
	for (uint32_t i = 0; i < g_ng; i++) {
		const struct grow *r = grow_get(i);

		g_bag_bk[i] = (r->rc == 0) ? r->g.booking : NO_REF;
	}
	g_bag_index_ok = true;
}

/* The stored aircraft flying this flight, or -1. */
static int aircraft_of(uint32_t flight)
{
	for (uint32_t i = 0; i < g_nk; i++) {
		for (int k = 0; k < g_acs[i].n_sectors; k++) {
			if (g_acs[i].sectors[k] == flight) {
				return (int)i;
			}
		}
	}
	return -1;
}

static int airline_by_code(const char *code)
{
	for (uint32_t i = 0; i < g_nl; i++) {
		if (strcmp(g_als[i].code, code) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static int fleet_size(const char *code)
{
	int n = 0;

	for (uint32_t i = 0; i < g_nk; i++) {
		n += (strcmp(g_acs[i].airline, code) == 0);
	}
	return n;
}

/* SURNAME/GIVEN TITLE, the way a reservation system writes a name. */
static const char *pax_name(const struct flightdb_passenger *p, char *out, size_t sz)
{
	snprintf(out, sz, "%s/%s %s", p->surname, p->given, p->title);
	return out;
}

/* The name of the passenger at @p index, or why there is none. */
static const char *pax_name_at(uint32_t index, char *out, size_t sz)
{
	const struct prow *r = prow_get(index);

	if (r->rc != 0) {
		snprintf(out, sz, "passenger %u: %s", index, errstr(r->rc));
		return out;
	}
	return pax_name(&r->p, out, sz);
}

/* Checked in is the news for a confirmed booking; otherwise the status. */
static const char *bk_status_str(const struct flightdb_booking *b)
{
	if (b->status == FLIGHTDB_BK_CONFIRMED && b->checked_in) {
		return "CHECKED IN";
	}
	return flightdb_booking_status_name(b->status);
}

static uint8_t bk_status_style(const struct flightdb_booking *b)
{
	switch (b->status) {
	case FLIGHTDB_BK_CANCELLED:  return ST_BAD;
	case FLIGHTDB_BK_WAITLISTED: return ST_WARN;
	default:                     return b->checked_in ? ST_OK : ST_NORMAL;
	}
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
		[TAB_FLIGHTS]    = "FLT100  DEPARTURE SCHEDULE",
		[TAB_AIRPORTS]   = "FLT200  AIRPORT DIRECTORY",
		[TAB_AIRLINES]   = "FLT300  AIRLINE DIRECTORY",
		[TAB_FLEET]      = "FLT400  FLEET LIST",
		[TAB_CREW]       = "FLT500  CREW LIST",
		[TAB_PASSENGERS] = "FLT600  PASSENGER LIST",
		[TAB_BOOKINGS]   = "FLT700  BOOKING LIST",
		[TAB_BAGS]       = "FLT800  BAGGAGE LIST",
		[TAB_STORE]      = "FLT900  SYSTEM UTILITIES",
	};
	/* Full names if they fit, else the codes, else the codes closer up. */
	static const struct {
		bool        code;
		const char *fmt;
		int         gap;
	} bars[] = {
		{ false, " %d %s ", 2 }, { true, " %d %s ", 2 }, { true, " %d %s ", 1 },
		{ true, "%d %s", 1 },
	};
	int bar = 0;
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
	} else if (g_tab == TAB_AIRLINES && g_al_detail) {
		code = "FLT310  AIRLINE DISPLAY";
	} else if (g_tab == TAB_FLEET && g_ac_detail) {
		code = "FLT410  AIRCRAFT DISPLAY";
	} else if (g_tab == TAB_CREW && g_cr_detail) {
		code = "FLT510  CREW DISPLAY";
	} else if (g_tab == TAB_PASSENGERS && g_px_detail) {
		code = "FLT610  PASSENGER DISPLAY";
	} else if (g_tab == TAB_BOOKINGS && g_bk_detail) {
		code = "FLT710  BOOKING DISPLAY";
	} else if (g_tab == TAB_BAGS && g_bg_detail) {
		code = "FLT810  BAG DISPLAY";
	}
	airdate(now, date, sizeof(date));
	n = snprintf(right, sizeof(right), "%s %02u%02uZ", date, (now / 3600u) % 24u,
		     (now / 60u) % 60u);
	put(0, 1, cols, ST_TITLE, "%s", code);
	putn(0, (cols - (int)strlen(sys)) / 2 + 8, cols, ST_NORMAL, sys);
	putn(0, cols - n - 1, n, ST_NORMAL, right);

	fill(1, 0, cols, '-', ST_NORMAL);
	for (; bar + 1 < (int)ARRAY_SIZE(bars); bar++) {
		int w = 2;

		for (int t = 0; t < TAB__COUNT; t++) {
			w += snprintf(NULL, 0, bars[bar].fmt, t + 1,
				      bars[bar].code ? tab_code[t] : tab_name[t]) + bars[bar].gap;
		}
		if (w - bars[bar].gap <= cols) {
			break;
		}
	}
	c = 2;
	for (int t = 0; t < TAB__COUNT; t++) {
		c += put(1, c, cols - c, (t == (int)g_tab) ? ST_TAB : ST_NORMAL, bars[bar].fmt,
			 t + 1, bars[bar].code ? tab_code[t] : tab_name[t]);
		c += bars[bar].gap;
	}
}

static void draw_footer(const char *const *hints, size_t n)
{
	char right[48];

	if (!g_open) {
		tui_footer(hints, n, " 1A  X SYSTEM  STORE NOT OPEN", "");
		return;
	}
	snprintf(right, sizeof(right), "%u FLIGHTS  %u BOOKINGS  LU FLT01 ", g_nf, g_nb);
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
		"F3", "RETURN", "M", "MANIFEST", "C", "CREW", "T", "TAIL", "A/Z", "AIRPORTS",
		"Q", "SIGN OFF",
	};
	static const char *const cabin[FLIGHTDB_CABINS] = { "F FIRST", "C BUSINESS", "Y ECONOMY" };
	const struct flightdb_flight *f = &g_fl;
	int ac = aircraft_of(g_fl_index);
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
	if (ac >= 0) {
		put(top + 5, 46, cols, ST_NORMAL, "TAIL %s", g_acs[ac].reg);
	}
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

static void draw_passenger_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int nw = CLAMP(cols - 52, 14, 27);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_np, "passengers")) {
		return;
	}
	g_px_cur = MIN(g_px_cur, g_np - 1);
	if (g_px_cur < g_px_top) {
		g_px_top = g_px_cur;
	} else if (g_px_cur >= g_px_top + (uint32_t)h) {
		g_px_top = g_px_cur - (uint32_t)h + 1;
	}

	put(top, 0, cols, ST_TITLE, "  %-4s  %-*s %-4s  %-12s %4s  %-9s  %s", "NBR", nw, "NAME",
	    "TTL", "NATIONALITY", "BORN", "FFP", "TIER");
	for (int i = 0; i < h && g_px_top + (uint32_t)i < g_np; i++) {
		uint32_t index = g_px_top + (uint32_t)i;
		bool sel = (index == g_px_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct prow *r = prow_get(index);
		const struct flightdb_passenger *p = &r->p;
		char name[40];

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %04u  read failed: %d (%s)", index, r->rc, errstr(r->rc));
			continue;
		}
		snprintf(name, sizeof(name), "%s/%s", p->surname, p->given);
		put(top + 1 + i, 0, cols, st, "  %04u  %-*.*s %-4s  %-12.12s %4u  %-9s  %s", index + 1,
		    nw, nw, name, p->title, p->nationality, p->birth_year, p->ffp,
		    flightdb_tier_name(p->tier));
	}
	{
		char pos[32];
		int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", g_px_cur + 1, g_np);

		putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
	}
}

/* One booking as a line of a list: the flight, its route and the seat. */
static const char *bk_line(uint32_t index, char *buf, size_t sz)
{
	const struct brow *r = brow_get(index);
	const struct flightdb_booking *b = &r->b;
	const struct row *fr;
	char t[5];

	if (r->rc != 0) {
		snprintf(buf, sz, "booking %u: %s", index, errstr(r->rc));
		return buf;
	}
	fr = row_get(b->flight);
	if (fr->rc != 0) {
		snprintf(buf, sz, "%s  flight %u: %s", b->locator, b->flight, errstr(fr->rc));
		return buf;
	}
	snprintf(buf, sz, "%s  %-7s %s  %s-%s  %s %-3s  %s", b->locator, fr->f.desig,
		 hhmm(fr->f.dep, t), fr->f.from, fr->f.to, flightdb_cabin_code(b->cabin), b->seat,
		 bk_status_str(b));
	return buf;
}

static const char *px_bk_item(int i)
{
	static char buf[64];

	return bk_line(g_px_bk[i], buf, sizeof(buf));
}

static void draw_passenger(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "ENTER", "BOOKING", "Q", "SIGN OFF",
	};
	const struct flightdb_passenger *p = &g_px;
	int top = body_top();
	char name[40];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_px_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Passenger at position %u: %d (%s).", g_px_index,
		    g_px_rc, errstr(g_px_rc));
		return;
	}
	put(top, 2, cols - 4, ST_BOLD, "1.%s", pax_name(p, name, sizeof(name)));
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_px_index + 1, g_np);
	put(top + 1, 2, cols - 4, ST_NORMAL, "NATIONALITY %s", p->nationality);
	put(top + 2, 2, cols - 4, ST_NORMAL, "BORN        %u", p->birth_year);
	if (p->ffp[0] != '\0') {
		put(top + 3, 2, cols - 4, ST_NORMAL, "FREQ FLYER  %s  %s", p->ffp,
		    flightdb_tier_name(p->tier));
	} else {
		put(top + 3, 2, cols - 4, ST_DIM, "FREQ FLYER  NOT A MEMBER");
	}

	{
		int r = top + 5;
		char title[32];

		snprintf(title, sizeof(title), "Bookings (%d)", g_px_bk_n);
		draw_list_pane(r, 1, body_bottom() - r + 1, cols - 2, title, true, g_px_sel,
			       g_px_bk_n, px_bk_item);
	}
}

static void draw_booking_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int nw = CLAMP(cols - 51, 12, 27);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nb, "bookings")) {
		return;
	}
	g_bk_cur = MIN(g_bk_cur, g_nb - 1);
	if (g_bk_cur < g_bk_top) {
		g_bk_top = g_bk_cur;
	} else if (g_bk_cur >= g_bk_top + (uint32_t)h) {
		g_bk_top = g_bk_cur - (uint32_t)h + 1;
	}

	put(top, 0, cols, ST_TITLE, "  %-6s  %-7s %4s  %-*s  %-2s %-4s %s", "RECLOC", "FLIGHT",
	    "STD", nw, "NAME", "CL", "SEAT", "STATUS");
	for (int i = 0; i < h && g_bk_top + (uint32_t)i < g_nb; i++) {
		uint32_t index = g_bk_top + (uint32_t)i;
		bool sel = (index == g_bk_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct brow *r = brow_get(index);
		const struct flightdb_booking *b = &r->b;
		const struct row *fr;
		char name[40], t[5];
		int c;

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %06u  read failed: %d (%s)", index, r->rc, errstr(r->rc));
			continue;
		}
		fr = row_get(b->flight);
		c = put(top + 1 + i, 0, cols, st, "  %-6s  %-7s %4s  %-*.*s  %-2s %-4s ", b->locator,
			(fr->rc == 0) ? fr->f.desig : "?", (fr->rc == 0) ? hhmm(fr->f.dep, t) : "",
			nw, nw, pax_name_at(b->passenger, name, sizeof(name)),
			flightdb_cabin_code(b->cabin), b->seat);
		put(top + 1 + i, c, cols - c, sel ? ST_SEL : bk_status_style(b), "%s",
		    bk_status_str(b));
	}
	{
		char pos[32];
		int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", g_bk_cur + 1, g_nb);

		putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
	}
}

static void draw_booking(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "ENTER", "FLIGHT", "P", "PASSENGER",
		"G", "BAGS", "Q", "SIGN OFF",
	};
	static const char *const cabin[FLIGHTDB_CABINS] = { "F FIRST", "C BUSINESS", "Y ECONOMY" };
	const struct flightdb_booking *b = &g_bk;
	const struct prow *pr;
	const struct row *fr;
	int top = body_top();
	char name[40], t1[5], t2[5];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_bk_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Booking at position %u: %d (%s).", g_bk_index,
		    g_bk_rc, errstr(g_bk_rc));
		return;
	}
	put(top, 2, cols - 4, ST_BOLD, "1.%s  BOOKING RECORD", b->locator);
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_bk_index + 1, g_nb);

	pr = prow_get(b->passenger);
	if (pr->rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "PASSENGER   NBR %u: %d (%s)", b->passenger + 1,
		    pr->rc, errstr(pr->rc));
	} else {
		put(top + 1, 2, cols - 4, ST_NORMAL, "PASSENGER   %s  NBR %u",
		    pax_name(&pr->p, name, sizeof(name)), b->passenger + 1);
		put(top + 2, 2, cols - 4, ST_DIM, "            %s  BORN %u  %s %s", pr->p.nationality,
		    pr->p.birth_year, pr->p.ffp, flightdb_tier_name(pr->p.tier));
	}

	fr = row_get(b->flight);
	if (fr->rc != 0) {
		put(top + 4, 2, cols - 4, ST_BAD, "FLIGHT      position %u: %d (%s)", b->flight,
		    fr->rc, errstr(fr->rc));
	} else {
		const struct flightdb_flight *f = &fr->f;
		enum flightdb_status fs = flightdb_status(f, now_minute());

		put(top + 4, 2, cols - 4, ST_NORMAL, "FLIGHT      %s  %s", f->desig, f->airline);
		put(top + 5, 2, cols - 4, ST_NORMAL, "ROUTE       %s %s - %s %s", f->from,
		    apt_city(f->from), f->to, apt_city(f->to));
		put(top + 6, 2, cols - 4, ST_NORMAL, "SCHEDULE    STD %sZ  STA %sZ  GATE %s T%u",
		    hhmm(f->dep, t1), hhmm(f->dep + f->block, t2), f->gate, f->terminal);
		put(top + 7, 2, 12, ST_NORMAL, "FLT STATUS");
		put(top + 7, 14, cols, status_style(fs), "%s", flightdb_status_name(fs));
	}

	put(top + 9, 2, cols - 4, ST_NORMAL, "CABIN       %-11s  SEAT %s",
	    (b->cabin < FLIGHTDB_CABINS) ? cabin[b->cabin] : "?", b->seat[0] ? b->seat : "----");
	put(top + 10, 2, 12, ST_NORMAL, "STATUS");
	put(top + 10, 14, cols, bk_status_style(b), "%s",
	    flightdb_booking_status_name(b->status));
	put(top + 11, 2, cols - 4, ST_NORMAL, "CHECK-IN    %s",
	    b->checked_in ? "CHECKED IN" : "NOT CHECKED IN");
	{
		int c = put(top + 12, 2, cols - 4, ST_NORMAL, "BAGS        %u", b->bags) + 2;

		/* The tags the baggage store holds for it, as far as the line goes. */
		for (int i = 0; i < g_bk_bags_n && c < cols - 12; i++) {
			const struct grow *r = grow_get(g_bk_bags[i]);

			c += put(top + 12, c + 2, cols - c - 4, ST_DIM, "%s",
				 (r->rc == 0) ? r->g.tag : "?") + 2;
		}
	}
	put(top + 13, 2, cols - 4, ST_NORMAL, "FARE        %u.00", b->fare);
}

/* One flight as a line of a list: when, which, where, and how it stands. */
static const char *fl_line(uint32_t index, char *buf, size_t sz)
{
	const struct row *r = row_get(index);
	const struct flightdb_flight *f = &r->f;
	char t1[5], t2[5];

	if (r->rc != 0) {
		snprintf(buf, sz, "flight %u: %s", index, errstr(r->rc));
		return buf;
	}
	snprintf(buf, sz, "%s  %-7s %s-%s  %s  %s", hhmm(f->dep, t1), f->desig, f->from, f->to,
		 hhmm(f->dep + f->block, t2), flightdb_status_name(flightdb_status(f, now_minute())));
	return buf;
}

/* Keep a list's cursor on screen: the top row, given @p h rows. */
static void list_scroll(uint32_t cur, uint32_t *top, int h)
{
	if (cur < *top) {
		*top = cur;
	} else if (cur >= *top + (uint32_t)h) {
		*top = cur - (uint32_t)h + 1;
	}
}

static void draw_list_pos(uint32_t cur, uint32_t n)
{
	char pos[32];
	int len = snprintf(pos, sizeof(pos), " LINE %u OF %u ", cur + 1, n);

	putn(body_bottom() + 1, cols - len - 1, len, ST_DIM, pos);
}

static void draw_airline_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nl, "airlines")) {
		return;
	}
	g_al_cur = MIN(g_al_cur, g_nl - 1);
	put(top, 0, cols, ST_TITLE, "  %-2s  %-19s %-11s %-3s  %-12s %-9s %4s %5s", "CD", "NAME",
	    "CALLSIGN", "HUB", "COUNTRY", "ALLIANCE", "FNDD", "FLEET");
	for (int i = 0; i < h && (uint32_t)i < g_nl; i++) {
		const struct flightdb_airline *a = &g_als[i];
		uint8_t st = ((uint32_t)i == g_al_cur) ? ST_SEL : ST_NORMAL;

		fill(top + 1 + i, 0, cols, ' ', st);
		put(top + 1 + i, 0, cols, st, "  %-2s  %-19s %-11s %-3s  %-12.12s %-9s %4u %5d",
		    a->code, a->name, a->callsign, a->hub, a->country,
		    a->alliance[0] ? a->alliance : "-", a->founded, fleet_size(a->code));
	}
}

static const char *al_fleet_item(int i)
{
	static char buf[64];
	const struct flightdb_aircraft *k = &g_acs[g_al_fleet[i]];

	snprintf(buf, sizeof(buf), "%-6s  %-4s  %3u/%3u/%3u  BUILT %u  %u SECTORS", k->reg, k->type,
		 k->seats[0], k->seats[1], k->seats[2], k->built, k->n_sectors);
	return buf;
}

static void draw_airline(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "ENTER", "AIRCRAFT", "Q", "SIGN OFF",
	};
	const struct flightdb_airline *a = &g_als[g_al_cur];
	int top = body_top();
	char title[32];

	draw_footer(hints, ARRAY_SIZE(hints));
	put(top, 2, cols - 4, ST_BOLD, "1.%s  %s", a->code, a->name);
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_al_cur + 1, g_nl);
	put(top + 1, 2, cols - 4, ST_NORMAL, "CALLSIGN    %s", a->callsign);
	put(top + 2, 2, cols - 4, ST_NORMAL, "HUB         %s %s, %s", a->hub, apt_city(a->hub),
	    a->country);
	put(top + 3, 2, cols - 4, ST_NORMAL, "ALLIANCE    %s  FOUNDED %u",
	    a->alliance[0] ? a->alliance : "NONE", a->founded);
	snprintf(title, sizeof(title), "Fleet (%d)", g_al_fleet_n);
	draw_list_pane(top + 5, 1, body_bottom() - top - 4, cols - 2, title, true, g_al_sel,
		       g_al_fleet_n, al_fleet_item);
}

static void draw_fleet_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nk, "aircraft")) {
		return;
	}
	g_ac_cur = MIN(g_ac_cur, g_nk - 1);
	list_scroll(g_ac_cur, &g_ac_top, h);
	put(top, 0, cols, ST_TITLE, "  %-6s  %-4s  %-19s %11s  %5s  %s", "REG", "TYPE", "AIRLINE",
	    "SEATS F/C/Y", "BUILT", "SECTORS");
	for (int i = 0; i < h && g_ac_top + (uint32_t)i < g_nk; i++) {
		uint32_t idx = g_ac_top + (uint32_t)i;
		const struct flightdb_aircraft *k = &g_acs[idx];
		int al = airline_by_code(k->airline);
		uint8_t st = (idx == g_ac_cur) ? ST_SEL : ST_NORMAL;

		fill(top + 1 + i, 0, cols, ' ', st);
		put(top + 1 + i, 0, cols, st, "  %-6s  %-4s  %-19s %3u/%3u/%3u  %5u  %u", k->reg,
		    k->type, (al >= 0) ? g_als[al].name : k->airline, k->seats[0], k->seats[1],
		    k->seats[2], k->built, k->n_sectors);
	}
	draw_list_pos(g_ac_cur, g_nk);
}

static const char *ac_sector_item(int i)
{
	static char buf[64];

	return fl_line(g_acs[g_ac_cur].sectors[i], buf, sizeof(buf));
}

static void draw_aircraft(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "ENTER", "FLIGHT", "Q", "SIGN OFF",
	};
	const struct flightdb_aircraft *k = &g_acs[g_ac_cur];
	int al = airline_by_code(k->airline);
	int top = body_top();
	char title[32];

	draw_footer(hints, ARRAY_SIZE(hints));
	put(top, 2, cols - 4, ST_BOLD, "1.%s  %s", k->reg, k->type);
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_ac_cur + 1, g_nk);
	put(top + 1, 2, cols - 4, ST_NORMAL, "OPERATOR    %s %s", k->airline,
	    (al >= 0) ? g_als[al].name : "not in the store");
	put(top + 2, 2, cols - 4, ST_NORMAL, "SEATS       F %u  C %u  Y %u  TOTAL %u", k->seats[0],
	    k->seats[1], k->seats[2], k->seats[0] + k->seats[1] + k->seats[2]);
	put(top + 3, 2, cols - 4, ST_NORMAL, "BUILT       %u", k->built);
	snprintf(title, sizeof(title), "Rotation (%u)", k->n_sectors);
	draw_list_pane(top + 5, 1, body_bottom() - top - 4, cols - 2, title, true, g_ac_sel,
		       k->n_sectors, ac_sector_item);
}

static void draw_crew_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int nw = CLAMP(cols - 46, 14, 27);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_nc, "crew")) {
		return;
	}
	g_cr_cur = MIN(g_cr_cur, g_nc - 1);
	list_scroll(g_cr_cur, &g_cr_top, h);
	put(top, 0, cols, ST_TITLE, "  %-5s  %-*s %-4s %-2s  %-4s  %s", "STAFF", nw, "NAME", "RANK",
	    "AL", "BASE", "DUTIES");
	for (int i = 0; i < h && g_cr_top + (uint32_t)i < g_nc; i++) {
		uint32_t idx = g_cr_top + (uint32_t)i;
		bool sel = (idx == g_cr_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct crow *r = crow_get(idx);
		const struct flightdb_crew *m = &r->c;
		char name[40];

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %05u  read failed: %d (%s)", idx, r->rc, errstr(r->rc));
			continue;
		}
		snprintf(name, sizeof(name), "%s/%s", m->surname, m->given);
		put(top + 1 + i, 0, cols, st, "  %05u  %-*.*s %-4s %-2s  %-4s  %u", m->staff_no, nw,
		    nw, name, flightdb_rank_code(m->rank), m->airline, m->base, m->n_duties);
	}
	draw_list_pos(g_cr_cur, g_nc);
}

static const char *cr_duty_item(int i)
{
	static char buf[64];

	return fl_line(g_cr.duties[i], buf, sizeof(buf));
}

static void draw_crew(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "ENTER", "FLIGHT", "Q", "SIGN OFF",
	};
	const struct flightdb_crew *m = &g_cr;
	int al;
	int top = body_top();
	char title[32];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_cr_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Crew member at position %u: %d (%s).", g_cr_index,
		    g_cr_rc, errstr(g_cr_rc));
		return;
	}
	al = airline_by_code(m->airline);
	put(top, 2, cols - 4, ST_BOLD, "1.%s/%s", m->surname, m->given);
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_cr_index + 1, g_nc);
	put(top + 1, 2, cols - 4, ST_NORMAL, "STAFF NO    %05u", m->staff_no);
	put(top + 2, 2, cols - 4, ST_NORMAL, "RANK        %s", flightdb_rank_name(m->rank));
	put(top + 3, 2, cols - 4, ST_NORMAL, "AIRLINE     %s %s  BASE %s %s", m->airline,
	    (al >= 0) ? g_als[al].name : "", m->base, apt_city(m->base));
	snprintf(title, sizeof(title), "Duties (%u)", m->n_duties);
	draw_list_pane(top + 5, 1, body_bottom() - top - 4, cols - 2, title, true, g_cr_sel,
		       m->n_duties, cr_duty_item);
}

static uint8_t bag_style(const struct flightdb_bag *g)
{
	switch (g->status) {
	case FLIGHTDB_BAG_MISHANDLED: return ST_BAD;
	case FLIGHTDB_BAG_OFFLOADED:  return ST_WARN;
	case FLIGHTDB_BAG_LOADED:     return ST_OK;
	default:                      return ST_NORMAL;
	}
}

static void draw_bag_list(void)
{
	static const char *const hints[] = {
		"ENTER", "DISPLAY", "F7", "BKWD", "F8", "FWD", "/", "LOCATE",
		"F", "FILL", "S", "SYSTEM", "Q", "SIGN OFF",
	};
	int top = body_top();
	int h = body_bottom() - top;
	int nw = CLAMP(cols - 58, 12, 27);

	draw_footer(hints, ARRAY_SIZE(hints));
	if (draw_unavailable(top + 1, g_ng, "bags")) {
		return;
	}
	g_bg_cur = MIN(g_bg_cur, g_ng - 1);
	list_scroll(g_bg_cur, &g_bg_top, h);
	put(top, 0, cols, ST_TITLE, "  %-8s  %-6s  %-*s  %-7s %-3s %3s  %-4s %s", "TAG", "RECLOC",
	    nw, "NAME", "FLIGHT", "DST", "KG", "PRIO", "STATUS");
	for (int i = 0; i < h && g_bg_top + (uint32_t)i < g_ng; i++) {
		uint32_t idx = g_bg_top + (uint32_t)i;
		bool sel = (idx == g_bg_cur);
		uint8_t st = sel ? ST_SEL : ST_NORMAL;
		const struct grow *r = grow_get(idx);
		const struct flightdb_bag *g = &r->g;
		const struct brow *br;
		const struct row *fr = NULL;
		char name[40] = "";
		int c;

		fill(top + 1 + i, 0, cols, ' ', st);
		if (r->rc != 0) {
			put(top + 1 + i, 0, cols, sel ? ST_SEL : ST_BAD,
			    "  %06u  read failed: %d (%s)", idx, r->rc, errstr(r->rc));
			continue;
		}
		br = brow_get(g->booking);
		if (br->rc == 0) {
			pax_name_at(br->b.passenger, name, sizeof(name));
			fr = row_get(br->b.flight);
		}
		c = put(top + 1 + i, 0, cols, st, "  %-8s  %-6s  %-*.*s  %-7s %-3s %3u  %-4s ", g->tag,
			(br->rc == 0) ? br->b.locator : "?", nw, nw, name,
			(fr != NULL && fr->rc == 0) ? fr->f.desig : "?", g->dest, g->weight,
			g->priority ? "PRIO" : "");
		put(top + 1 + i, c, cols - c, sel ? ST_SEL : bag_style(g), "%s",
		    flightdb_bag_status_name(g->status));
	}
	draw_list_pos(g_bg_cur, g_ng);
}

static void draw_bag(void)
{
	static const char *const hints[] = {
		"F3", "RETURN", "F7/F8", "PREV/NEXT", "ENTER", "FLIGHT", "B", "BOOKING",
		"Q", "SIGN OFF",
	};
	const struct flightdb_bag *g = &g_bg;
	const struct brow *br;
	int top = body_top();
	char name[40], line[64];

	draw_footer(hints, ARRAY_SIZE(hints));
	if (g_bg_rc != 0) {
		put(top + 1, 2, cols - 4, ST_BAD, "Bag at position %u: %d (%s).", g_bg_index, g_bg_rc,
		    errstr(g_bg_rc));
		return;
	}
	put(top, 2, cols - 4, ST_BOLD, "1.%s  BAG TAG", g->tag);
	put(top, 46, cols, ST_NORMAL, "NBR %u OF %u", g_bg_index + 1, g_ng);
	put(top + 1, 2, cols - 4, ST_NORMAL, "DESTINATION %s %s", g->dest, apt_city(g->dest));
	put(top + 2, 2, cols - 4, ST_NORMAL, "WEIGHT      %u KG%s", g->weight,
	    g->priority ? "  PRIORITY" : "");
	put(top + 3, 2, 12, ST_NORMAL, "STATUS");
	put(top + 3, 14, cols, bag_style(g), "%s", flightdb_bag_status_name(g->status));

	br = brow_get(g->booking);
	if (br->rc != 0) {
		put(top + 5, 2, cols - 4, ST_BAD, "BOOKING     position %u: %d (%s)", g->booking,
		    br->rc, errstr(br->rc));
		return;
	}
	put(top + 5, 2, cols - 4, ST_NORMAL, "BOOKING     %s  %s", br->b.locator,
	    pax_name_at(br->b.passenger, name, sizeof(name)));
	put(top + 6, 2, cols - 4, ST_NORMAL, "FLIGHT      %s", fl_line(br->b.flight, line,
									   sizeof(line)));
}

static const char *const store_actions[] = {
	"Fill flights and bookings...",
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
		put(r++, 2, ac - 4, ST_NORMAL, "Passengers   %u of %u", g_st.passengers,
		    FLIGHTDB_PASSENGERS_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Bookings     %u of %u", g_st.bookings,
		    FLIGHTDB_BOOKINGS_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Airlines     %u of %u", g_st.airlines,
		    FLIGHTDB_AIRLINES_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Aircraft     %u of %u", g_st.aircraft,
		    FLIGHTDB_AIRCRAFT_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Crew         %u of %u", g_st.crew,
		    FLIGHTDB_CREW_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Bags         %u of %u", g_st.bags,
		    FLIGHTDB_BAGS_MAX);
		put(r++, 2, ac - 4, ST_NORMAL, "Partition    %zu B", g_st.partition_bytes);
		put(r++, 2, ac - 4, ST_DIM, "Buckets      %u/%u/%u/%u/%u/%u/%u/%u",
		    g_st.airport_buckets, g_st.flight_buckets, g_st.passenger_buckets,
		    g_st.booking_buckets, g_st.airline_buckets, g_st.aircraft_buckets,
		    g_st.crew_buckets, g_st.bag_buckets);
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
		if (g_als_rc != 0 || g_acs_rc != 0) {
			put(r++, 2, ac - 4, ST_BAD, "Airlines/acft read failed: %d",
			    g_als_rc ? g_als_rc : g_acs_rc);
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
	case TAB_AIRLINES:
		if (g_al_detail) {
			draw_airline();
		} else {
			draw_airline_list();
		}
		break;
	case TAB_FLEET:
		if (g_ac_detail) {
			draw_aircraft();
		} else {
			draw_fleet_list();
		}
		break;
	case TAB_CREW:
		if (g_cr_detail) {
			draw_crew();
		} else {
			draw_crew_list();
		}
		break;
	case TAB_BAGS:
		if (g_bg_detail) {
			draw_bag();
		} else {
			draw_bag_list();
		}
		break;
	case TAB_PASSENGERS:
		if (g_px_detail) {
			draw_passenger();
		} else {
			draw_passenger_list();
		}
		break;
	case TAB_BOOKINGS:
		if (g_bk_detail) {
			draw_booking();
		} else {
			draw_booking_list();
		}
		break;
	default:
		draw_store();
		break;
	}
}

/* -- actions ------------------------------------------------------------- */

/* Fill in batches, so progress shows and F3 can stop between them; each
 * batch is published by the scenario layer before the next starts. A flight
 * is a dozen writes with its bookings and passengers, so batches are small. */
#define FILL_BATCH 8u

static void store_reset(void);

static void act_fill(void)
{
	uint32_t complete = g_open ? flight_scenario_flights_complete(&g_s) : 0;
	uint32_t left = flight_scenario_flights_defined() - MIN(complete,
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
		/* The generator defines a fixed schedule; to fill again the store
		 * has to start over, which the operator says yes to explicitly. */
		snprintf(l1, sizeof(l1), "All %u flights are already stored.",
			 flight_scenario_flights_defined());
		if (!confirm("Fill", l1, "Erase the store and fill it again?")) {
			return;
		}
		store_reset();
		if (!g_open) {
			return;
		}
		left = flight_scenario_flights_defined();
		want = left;
	}
	snprintf(l1, sizeof(l1), "Flights to fill and book (1..%u):", left);
	if (!input_number("Fill", l1, &want, left)) {
		return;
	}
	t0 = k_uptime_get();    /* after the prompt: time the writing only */
	while (done < want && !stopped) {
		uint32_t w = 0;

		stopped = busy("Fill", "Writing flights, crew, bookings, bags...", done, want);
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

	snprintf(l1, sizeof(l1), "%u flights filled in %lld ms%s", done,
		 (long long)(k_uptime_get() - t0), stopped ? " (stopped)" : "");
	snprintf(l2, sizeof(l2), "store: %u flights, %u bookings, %u bags", g_nf, g_nb, g_ng);
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
	snprintf(l1, sizeof(l1), "%s: %u apt %u airl %u acft %u crew %u flt", pass ? "PASS" : "FAIL",
		 v.airports_checked, v.airlines_checked, v.aircraft_checked, v.crew_checked,
		 v.flights_checked);
	snprintf(l2, sizeof(l2), "%u pax %u bkg %u bags; %u bad; %lld ms", v.passengers_checked,
		 v.bookings_checked, v.bags_checked, v.bad, (long long)v.ms);
	log_line(pass ? ST_OK : ST_BAD, "verify %s; %s", l1, l2);
	store_refresh();
	message("Verify", pass ? ST_OK : ST_BAD, l1, l2);
}

static void store_reset(void)
{
	int rc;

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
	g_px_cur = 0;
	g_px_top = 0;
	g_px_detail = false;
	g_bk_cur = 0;
	g_bk_top = 0;
	g_bk_detail = false;
	g_al_cur = 0;
	g_al_detail = false;
	g_ac_cur = 0;
	g_ac_top = 0;
	g_ac_detail = false;
	g_cr_cur = 0;
	g_cr_top = 0;
	g_cr_detail = false;
	g_bg_cur = 0;
	g_bg_top = 0;
	g_bg_detail = false;
	store_refresh();
	log_line(rc != 0 ? ST_BAD : ST_NORMAL, "reset: %d (%s)", rc, errstr(rc));
	status(rc != 0 ? ST_BAD : ST_OK, "reset: %s", errstr(rc));
}

static void act_reset(void)
{
	if (confirm("Reset", "Erase every flight and booking?",
		    "Persons in the other store are kept.")) {
		store_reset();
	}
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

static void booking_open(uint32_t index)
{
	g_fl_detail = false;
	g_tab = TAB_BOOKINGS;
	g_bk_cur = index;
	g_bk_index = index;
	g_bk_rc = flightdb_booking_get(g_s.db, index, &g_bk);
	if (g_bk_rc != 0) {
		memset(&g_bk, 0, sizeof(g_bk));
	}
	/* Its bags, for the tag line and G. */
	g_bk_bags_n = 0;
	if (g_bk_rc == 0 && g_bk.bags > 0) {
		bag_index_load();
		for (uint32_t i = 0; i < g_ng && g_bk_bags_n < BOOKING_BAGS_MAX; i++) {
			if (g_bag_bk[i] == index) {
				g_bk_bags[g_bk_bags_n++] = (uint16_t)i;
			}
		}
	}
	g_bk_detail = true;
}

static void airline_open(uint32_t index)
{
	g_fl_detail = false;
	g_tab = TAB_AIRLINES;
	g_al_cur = index;
	g_al_fleet_n = 0;
	g_al_sel = 0;
	for (uint32_t i = 0; i < g_nk; i++) {
		if (strcmp(g_acs[i].airline, g_als[index].code) == 0) {
			g_al_fleet[g_al_fleet_n++] = (uint16_t)i;
		}
	}
	g_al_detail = true;
}

static void aircraft_open(uint32_t index)
{
	g_fl_detail = false;
	g_tab = TAB_FLEET;
	g_ac_cur = index;
	g_ac_sel = 0;
	g_ac_detail = true;
}

static void crew_open(uint32_t index)
{
	g_fl_detail = false;
	g_tab = TAB_CREW;
	g_cr_cur = index;
	g_cr_index = index;
	g_cr_rc = flightdb_crew_get(g_s.db, index, &g_cr);
	if (g_cr_rc != 0) {
		memset(&g_cr, 0, sizeof(g_cr));
	}
	g_cr_sel = 0;
	g_cr_detail = true;
}

static void bag_open(uint32_t index)
{
	g_fl_detail = false;
	g_tab = TAB_BAGS;
	g_bg_cur = index;
	g_bg_index = index;
	g_bg_rc = flightdb_bag_get(g_s.db, index, &g_bg);
	if (g_bg_rc != 0) {
		memset(&g_bg, 0, sizeof(g_bg));
	}
	g_bg_detail = true;
}

static const char *crew_pick_item(int i)
{
	static char buf[64];
	const struct crow *r = crow_get(g_pick[i]);

	if (r->rc != 0) {
		snprintf(buf, sizeof(buf), "crew %u: %s", g_pick[i], errstr(r->rc));
		return buf;
	}
	snprintf(buf, sizeof(buf), "%-3s  %05u  %s/%s", flightdb_rank_code(r->c.rank),
		 r->c.staff_no, r->c.surname, r->c.given);
	return buf;
}

/* The crew the store rosters on the flight on display, captain first. */
static void flight_crew(void)
{
	char title[40];
	int sel;

	crew_index_load();
	g_pick_n = 0;
	for (int rank = 0; rank < FLIGHTDB_RANK__COUNT; rank++) {
		for (uint32_t i = 0; i < g_nc; i++) {
			const struct crow *r;
			bool on = false;

			for (int k = 0; k < g_cr_n[i]; k++) {
				on |= (g_cr_duty[i][k] == g_fl_index);
			}
			if (!on) {
				continue;
			}
			r = crow_get(i);
			/* Unreadable ones once, with the captains. */
			if (((r->rc != 0) ? rank == 0 : r->c.rank == rank) &&
			    g_pick_n < FLIGHT_CREW_MAX) {
				g_pick[g_pick_n++] = (uint16_t)i;
			}
		}
	}
	if (g_pick_n == 0) {
		status(ST_WARN, "no crew rostered on %s in the store", g_fl.desig);
		return;
	}
	snprintf(title, sizeof(title), "Crew %s (%d)", g_fl.desig, g_pick_n);
	sel = pick(title, g_pick_n, crew_pick_item, 0);
	if (sel >= 0) {
		crew_open(g_pick[sel]);
	}
}

static const char *bag_pick_item(int i)
{
	static char buf[64];
	const struct grow *r = grow_get(g_bk_bags[i]);

	if (r->rc != 0) {
		snprintf(buf, sizeof(buf), "bag %u: %s", g_bk_bags[i], errstr(r->rc));
		return buf;
	}
	snprintf(buf, sizeof(buf), "%s  %-3s %2u KG  %s", r->g.tag, r->g.dest, r->g.weight,
		 flightdb_bag_status_name(r->g.status));
	return buf;
}

static void passenger_open(uint32_t index)
{
	g_fl_detail = false;
	g_tab = TAB_PASSENGERS;
	g_px_cur = index;
	g_px_index = index;
	g_px_rc = flightdb_passenger_get(g_s.db, index, &g_px);
	if (g_px_rc != 0) {
		memset(&g_px, 0, sizeof(g_px));
	}
	g_px_bk_n = 0;
	g_px_sel = 0;
	booking_index_load();
	for (uint32_t i = 0; i < g_nb && g_px_bk_n < PAX_BOOKINGS_MAX; i++) {
		if (g_bk_pax[i] == index) {
			g_px_bk[g_px_bk_n++] = (uint16_t)i;
		}
	}
	g_px_detail = true;
}

static const char *man_item(int i)
{
	static char buf[64];
	const struct brow *r = brow_get(g_man[i]);
	char name[40];

	if (r->rc != 0) {
		snprintf(buf, sizeof(buf), "booking %u: %s", g_man[i], errstr(r->rc));
		return buf;
	}
	snprintf(buf, sizeof(buf), "%s  %-24.24s %s %-3s  %s", r->b.locator,
		 pax_name_at(r->b.passenger, name, sizeof(name)), flightdb_cabin_code(r->b.cabin),
		 r->b.seat, bk_status_str(&r->b));
	return buf;
}

/* The bookings the store holds on the flight on display, to pick one from. */
static void manifest(void)
{
	char title[40];
	int sel;

	booking_index_load();
	g_man_n = 0;
	for (uint32_t i = 0; i < g_nb && g_man_n < MANIFEST_MAX; i++) {
		if (g_bk_flt[i] == g_fl_index) {
			g_man[g_man_n++] = (uint16_t)i;
		}
	}
	if (g_man_n == 0) {
		status(ST_WARN, "no bookings for %s in the store", g_fl.desig);
		return;
	}
	snprintf(title, sizeof(title), "Manifest %s (%d)", g_fl.desig, g_man_n);
	sel = pick(title, g_man_n, man_item, 0);
	if (sel >= 0) {
		booking_open(g_man[sel]);
	}
}

/* A typed search key: spaces dropped, upper case, as the store spells it. */
static void search_key(const char *in, char *out)
{
	size_t n = 0;

	for (const char *p = in; *p != '\0'; p++) {
		if (*p != ' ') {
			out[n++] = (char)toupper((unsigned char)*p);
		}
	}
	out[n] = '\0';
}

static void passenger_locate(void)
{
	char buf[FLIGHTDB_SURNAME_MAX + 1] = "";
	char want[FLIGHTDB_SURNAME_MAX + 1];
	uint32_t n = g_np;

	if (!input_text("Locate", "Surname, or its start:", buf, sizeof(buf), false, false)) {
		return;
	}
	search_key(buf, want);
	busy("Locate", "Searching the passengers...", 0, 0);
	/* From the one after the cursor, so repeating finds the next. */
	for (uint32_t k = 1; k <= n; k++) {
		uint32_t i = (g_px_cur + k) % n;
		const struct prow *r = prow_get(i);

		if (r->rc == 0 && strncmp(r->p.surname, want, strlen(want)) == 0) {
			g_px_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no passenger %s in the store", want);
}

static void booking_locate(void)
{
	char buf[FLIGHTDB_LOCATOR_LEN + 1] = "";
	char want[FLIGHTDB_LOCATOR_LEN + 1];

	if (!input_text("Locate", "Record locator:", buf, sizeof(buf), false, false)) {
		return;
	}
	search_key(buf, want);
	busy("Locate", "Searching the bookings...", 0, 0);
	for (uint32_t i = 0; i < g_nb; i++) {
		const struct brow *r = brow_get(i);

		if (r->rc == 0 && strcmp(r->b.locator, want) == 0) {
			g_bk_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no booking %s in the store", want);
}

static void aircraft_locate(void)
{
	char buf[FLIGHTDB_REG_MAX + 1] = "";
	char want[FLIGHTDB_REG_MAX + 1];

	if (!input_text("Locate", "Registration, e.g. VD-KRA:", buf, sizeof(buf), false, false)) {
		return;
	}
	search_key(buf, want);
	for (uint32_t i = 0; i < g_nk; i++) {
		if (strcmp(g_acs[i].reg, want) == 0) {
			g_ac_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no aircraft %s in the store", want);
}

static void crew_locate(void)
{
	char buf[FLIGHTDB_SURNAME_MAX + 1] = "";
	char want[FLIGHTDB_SURNAME_MAX + 1];
	uint32_t n = g_nc;

	if (!input_text("Locate", "Staff number or surname:", buf, sizeof(buf), false, false)) {
		return;
	}
	search_key(buf, want);
	busy("Locate", "Searching the crew...", 0, 0);
	for (uint32_t k = 1; k <= n; k++) {
		uint32_t i = (g_cr_cur + k) % n;
		const struct crow *r = crow_get(i);
		char staff[12];

		if (r->rc != 0) {
			continue;
		}
		snprintf(staff, sizeof(staff), "%05u", r->c.staff_no);
		if (strcmp(staff, want) == 0 ||
		    strncmp(r->c.surname, want, strlen(want)) == 0) {
			g_cr_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no crew member %s in the store", want);
}

static void bag_locate(void)
{
	char buf[FLIGHTDB_TAG_MAX + 1] = "";
	char want[FLIGHTDB_TAG_MAX + 1];

	if (!input_text("Locate", "Bag tag, e.g. XA482913:", buf, sizeof(buf), false, false)) {
		return;
	}
	search_key(buf, want);
	busy("Locate", "Searching the bags...", 0, 0);
	for (uint32_t i = 0; i < g_ng; i++) {
		const struct grow *r = grow_get(i);

		if (r->rc == 0 && strcmp(r->g.tag, want) == 0) {
			g_bg_cur = i;
			status(ST_NORMAL, "");
			return;
		}
	}
	status(ST_BAD, "no bag %s in the store", want);
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
	case 'm':
		if (g_fl_rc == 0) {
			manifest();
		}
		return true;
	case 'c':
		if (g_fl_rc == 0) {
			flight_crew();
		}
		return true;
	case 't': {
		int ac = aircraft_of(g_fl_index);

		if (ac >= 0) {
			aircraft_open((uint32_t)ac);
		} else {
			status(ST_WARN, "no aircraft for %s in the store", g_fl.desig);
		}
		return true;
	}
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

/* Up, down, page and home/end over a list of @p n, with ENTER, / and F. */
static bool keys_list(int k, uint32_t *cur, uint32_t n, void (*open)(uint32_t),
		      void (*locate)(void))
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
	case K_ENTER:
		if (g_open && n > 0) {
			open(*cur);
		}
		return true;
	case '/':
		if (n > 0) {
			locate();
		}
		return true;
	case 'f':
		act_fill();
		return true;
	default:
		return false;
	}
}

static bool keys_passenger_list(int k)
{
	return keys_list(k, &g_px_cur, g_np, passenger_open, passenger_locate);
}

static bool keys_passenger(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_px_detail = false;
		return true;
	case K_UP:
		g_px_sel = (g_px_sel > 0) ? g_px_sel - 1 : 0;
		return true;
	case K_DOWN:
		g_px_sel = (g_px_sel + 1 < g_px_bk_n) ? g_px_sel + 1 : g_px_sel;
		return true;
	case K_PGUP:
		if (g_px_index > 0) {
			passenger_open(g_px_index - 1);
		}
		return true;
	case K_PGDN:
		if (g_px_index + 1 < g_np) {
			passenger_open(g_px_index + 1);
		}
		return true;
	case K_ENTER:
		if (g_px_bk_n > 0) {
			booking_open(g_px_bk[g_px_sel]);
		}
		return true;
	default:
		return false;
	}
}

static bool keys_booking_list(int k)
{
	return keys_list(k, &g_bk_cur, g_nb, booking_open, booking_locate);
}

static bool keys_booking(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_bk_detail = false;
		return true;
	case K_PGUP:
		if (g_bk_index > 0) {
			booking_open(g_bk_index - 1);
		}
		return true;
	case K_PGDN:
		if (g_bk_index + 1 < g_nb) {
			booking_open(g_bk_index + 1);
		}
		return true;
	case K_ENTER:
		if (g_bk_rc == 0) {
			flight_open(g_bk.flight);
			g_fl_cur = g_bk.flight;
		}
		return true;
	case 'p':
		if (g_bk_rc == 0) {
			passenger_open(g_bk.passenger);
		}
		return true;
	case 'g':
		if (g_bk_bags_n == 0) {
			status(ST_WARN, "no bags for %s in the store", g_bk.locator);
		} else {
			int sel = pick("Bags", g_bk_bags_n, bag_pick_item, 0);

			if (sel >= 0) {
				bag_open(g_bk_bags[sel]);
			}
		}
		return true;
	default:
		return false;
	}
}

static void airline_open_cur(uint32_t index)
{
	airline_open(index);
}

static void airline_no_locate(void)
{
}

static bool keys_airline_list(int k)
{
	return keys_list(k, &g_al_cur, g_nl, airline_open_cur, airline_no_locate);
}

static bool keys_airline(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_al_detail = false;
		return true;
	case K_UP:
		g_al_sel = (g_al_sel > 0) ? g_al_sel - 1 : 0;
		return true;
	case K_DOWN:
		g_al_sel = (g_al_sel + 1 < g_al_fleet_n) ? g_al_sel + 1 : g_al_sel;
		return true;
	case K_PGUP:
		airline_open((g_al_cur + g_nl - 1) % g_nl);
		return true;
	case K_PGDN:
		airline_open((g_al_cur + 1) % g_nl);
		return true;
	case K_ENTER:
		if (g_al_fleet_n > 0) {
			aircraft_open(g_al_fleet[g_al_sel]);
		}
		return true;
	default:
		return false;
	}
}

static bool keys_fleet_list(int k)
{
	return keys_list(k, &g_ac_cur, g_nk, aircraft_open, aircraft_locate);
}

static bool keys_aircraft(int k)
{
	const struct flightdb_aircraft *a = &g_acs[g_ac_cur];

	switch (k) {
	case K_ESC:
	case K_BS:
		g_ac_detail = false;
		return true;
	case K_UP:
		g_ac_sel = (g_ac_sel > 0) ? g_ac_sel - 1 : 0;
		return true;
	case K_DOWN:
		g_ac_sel = (g_ac_sel + 1 < a->n_sectors) ? g_ac_sel + 1 : g_ac_sel;
		return true;
	case K_PGUP:
		aircraft_open((g_ac_cur + g_nk - 1) % g_nk);
		return true;
	case K_PGDN:
		aircraft_open((g_ac_cur + 1) % g_nk);
		return true;
	case K_ENTER:
		if (a->n_sectors > 0) {
			flight_open(a->sectors[g_ac_sel]);
			g_fl_cur = g_fl_index;
		}
		return true;
	default:
		return false;
	}
}

static bool keys_crew_list(int k)
{
	return keys_list(k, &g_cr_cur, g_nc, crew_open, crew_locate);
}

static bool keys_crew(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_cr_detail = false;
		return true;
	case K_UP:
		g_cr_sel = (g_cr_sel > 0) ? g_cr_sel - 1 : 0;
		return true;
	case K_DOWN:
		g_cr_sel = (g_cr_sel + 1 < g_cr.n_duties) ? g_cr_sel + 1 : g_cr_sel;
		return true;
	case K_PGUP:
		if (g_cr_index > 0) {
			crew_open(g_cr_index - 1);
		}
		return true;
	case K_PGDN:
		if (g_cr_index + 1 < g_nc) {
			crew_open(g_cr_index + 1);
		}
		return true;
	case K_ENTER:
		if (g_cr_rc == 0 && g_cr.n_duties > 0) {
			flight_open(g_cr.duties[g_cr_sel]);
			g_fl_cur = g_fl_index;
		}
		return true;
	default:
		return false;
	}
}

static bool keys_bag_list(int k)
{
	return keys_list(k, &g_bg_cur, g_ng, bag_open, bag_locate);
}

static bool keys_bag(int k)
{
	switch (k) {
	case K_ESC:
	case K_BS:
		g_bg_detail = false;
		return true;
	case K_PGUP:
		if (g_bg_index > 0) {
			bag_open(g_bg_index - 1);
		}
		return true;
	case K_PGDN:
		if (g_bg_index + 1 < g_ng) {
			bag_open(g_bg_index + 1);
		}
		return true;
	case K_ENTER: {
		const struct brow *br = brow_get(g_bg.booking);

		if (g_bg_rc == 0 && br->rc == 0) {
			flight_open(br->b.flight);
			g_fl_cur = g_fl_index;
		}
		return true;
	}
	case 'b':
		if (g_bg_rc == 0) {
			booking_open(g_bg.booking);
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
	struct flightdb_counts c;
	int rc = flight_scenario_open(&s);

	if (rc == 0) {
		flightdb_counts(s.db, &c);
		*airports = c.airports;
		*flights = c.flights;
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
	g_px_detail = false;
	g_bk_detail = false;
	g_al_detail = false;
	g_ac_detail = false;
	g_cr_detail = false;
	g_bg_detail = false;

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
		} else if (g_tab == TAB_AIRLINES) {
			used = g_al_detail ? keys_airline(k) : keys_airline_list(k);
		} else if (g_tab == TAB_FLEET) {
			used = g_ac_detail ? keys_aircraft(k) : keys_fleet_list(k);
		} else if (g_tab == TAB_CREW) {
			used = g_cr_detail ? keys_crew(k) : keys_crew_list(k);
		} else if (g_tab == TAB_BAGS) {
			used = g_bg_detail ? keys_bag(k) : keys_bag_list(k);
		} else if (g_tab == TAB_PASSENGERS) {
			used = g_px_detail ? keys_passenger(k) : keys_passenger_list(k);
		} else if (g_tab == TAB_BOOKINGS) {
			used = g_bk_detail ? keys_booking(k) : keys_booking_list(k);
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
		case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9':
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
