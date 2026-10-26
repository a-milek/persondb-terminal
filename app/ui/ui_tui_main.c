/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_main — the sign-on screen: one firmware, every database.
 *
 * The person, flight and library stores share the flash under their own
 * registry keys. This screen opens each in turn, shows what it holds, and
 * runs the application the operator picks (ui_tui_apps.h); S in either one
 * comes back here. Nothing is erased by switching.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "scenario.h"
#include "tui.h"
#include "ui_tui_apps.h"

enum app {
	APP_PERSONS,
	APP_FLIGHTS,
	APP_LIBRARY,
	APP__COUNT,
};

static const char *const app_name[APP__COUNT] = {
	[APP_PERSONS] = "PERSONDB ACCESS CONTROL",
	[APP_FLIGHTS] = "FLIGHT INFORMATION",
	[APP_LIBRARY] = "LIBRARY CATALOGUE",
};

/* What each store held when this screen last looked. */
static char g_contents[APP__COUNT][48];
static bool g_has_data[APP__COUNT];
static int g_sel;

static void survey(void)
{
	uint32_t persons = 0, airports = 0, flights = 0, books = 0, on_loan = 0;
	int rc;

	rc = tui_persons_count(&persons);
	g_has_data[APP_PERSONS] = (rc == 0 && persons > 0);
	if (rc != 0) {
		snprintf(g_contents[APP_PERSONS], sizeof(g_contents[0]), "open failed: %d (%s)",
			 rc, errstr(rc));
	} else if (persons == 0) {
		strcpy(g_contents[APP_PERSONS], "empty");
	} else {
		snprintf(g_contents[APP_PERSONS], sizeof(g_contents[0]), "%u persons", persons);
	}

	rc = tui_flights_count(&airports, &flights);
	g_has_data[APP_FLIGHTS] = (rc == 0 && (airports > 0 || flights > 0));
	if (rc != 0) {
		snprintf(g_contents[APP_FLIGHTS], sizeof(g_contents[0]), "open failed: %d (%s)",
			 rc, errstr(rc));
	} else if (!g_has_data[APP_FLIGHTS]) {
		strcpy(g_contents[APP_FLIGHTS], "empty");
	} else {
		snprintf(g_contents[APP_FLIGHTS], sizeof(g_contents[0]),
			 "%u airports, %u flights", airports, flights);
	}

	rc = tui_library_count(&books, &on_loan);
	g_has_data[APP_LIBRARY] = (rc == 0 && books > 0);
	if (rc != 0) {
		snprintf(g_contents[APP_LIBRARY], sizeof(g_contents[0]), "open failed: %d (%s)",
			 rc, errstr(rc));
	} else if (!g_has_data[APP_LIBRARY]) {
		strcpy(g_contents[APP_LIBRARY], "empty");
	} else {
		snprintf(g_contents[APP_LIBRARY], sizeof(g_contents[0]), "%u books, %u on loan",
			 books, on_loan);
	}
}

static void draw_signon(void)
{
	static const char *const hints[] = {
		"ENTER", "SIGN IN", "1-3", "SELECT", "Q", "SIGN OFF",
	};
	static const char sys[] = "PERSONDB TERMINAL";
	uint32_t now = SCENARIO_BENCH_NOW + (uint32_t)(k_uptime_get() / 1000);
	int top = body_top();
	char right[24], date[16];
	int n;

	grid_clear();
	airdate(now, date, sizeof(date));
	n = snprintf(right, sizeof(right), "%s %02u%02uZ", date, (now / 3600u) % 24u,
		     (now / 60u) % 60u);
	put(0, 1, cols, ST_TITLE, "SON100  SIGN-ON");
	putn(0, (cols - (int)strlen(sys)) / 2 + 8, cols, ST_NORMAL, sys);
	putn(0, cols - n - 1, n, ST_NORMAL, right);
	fill(1, 0, cols, '-', ST_NORMAL);

	put(top, 2, cols - 4, ST_BOLD, "Select an application");
	put(top + 2, 2, cols - 4, ST_DIM, "%-4s %-26s %s", "NBR", "APPLICATION",
	    "STORE CONTENTS");
	for (int i = 0; i < APP__COUNT; i++) {
		uint8_t st = (i == g_sel) ? ST_SEL : ST_NORMAL;

		fill(top + 3 + i, 1, cols - 2, ' ', st);
		put(top + 3 + i, 2, cols - 4, st, "%-4d %-26s %s", i + 1, app_name[i],
		    g_contents[i]);
	}
	put(top + 4 + APP__COUNT, 2, cols - 4, ST_DIM,
	    "All stores share the flash; switching keeps their data.");
	put(top + 5 + APP__COUNT, 2, cols - 4, ST_DIM,
	    "Press S in an application to return here.");

	tui_footer(hints, ARRAY_SIZE(hints), " 1A  ONLINE", "LU SON01 ");
}

static void no_store(void)
{
}

static void run(int app)
{
	status(ST_NORMAL, "%s", "");
	g_log_n = 0;
	switch (app) {
	case APP_PERSONS:
		tui_persons_run();
		break;
	case APP_FLIGHTS:
		tui_flights_run();
		break;
	default:
		tui_library_run();
		break;
	}
	status(ST_NORMAL, "%s", "");
	draw = draw_signon;
	survey();
}

int ui_main(void)
{
	int rc = rx_start();

	if (rc != 0) {
		printk("persondb: console UART has no RX interrupt: %d\n", rc);
		return rc;
	}

	draw = draw_signon;
	survey();
	/* Start on the first store that holds something; the person database
	 * if none does. */
	g_sel = APP_PERSONS;
	for (int i = 0; i < APP__COUNT; i++) {
		if (g_has_data[i]) {
			g_sel = i;
			break;
		}
	}
	screen_enter();
	for (;;) {
		int k;

		draw();
		present();
		k = getkey(1000);       /* a second without a key: redraw the clock */
		switch (k) {
		case K_DOWN:
		case K_TAB:
			g_sel = (g_sel + 1) % APP__COUNT;
			break;
		case K_UP:
		case K_BTAB:
			g_sel = (g_sel + APP__COUNT - 1) % APP__COUNT;
			break;
		case '1': case '2': case '3':
			g_sel = k - '1';
			break;
		case K_ENTER:
			run(g_sel);
			break;
		case K_REDRAW:
			detect_size();
			break;
		case 'q':
		case 'Q':
		case K_INTR:
			sign_off(no_store);
			break;
		default:
			break;
		}
	}
	return 0;
}
