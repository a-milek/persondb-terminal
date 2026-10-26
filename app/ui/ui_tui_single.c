/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_single — the terminal with one application and no sign-on screen
 * (CONFIG_APP_CBOR_PERSONDB_TUI_PERSONS, _FLIGHTS or _LIBRARY). The build
 * compiles only that database, so its flash holds nothing else; S does
 * nothing (TUI_STANDALONE) and Q signs off.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "tui.h"
#include "ui_tui_apps.h"

int ui_main(void)
{
	int rc = rx_start();

	if (rc != 0) {
		printk("persondb: console UART has no RX interrupt: %d\n", rc);
		return rc;
	}

	screen_enter();
	for (;;) {
		/* Returns only on S, which a standalone build ignores. */
#if defined(CONFIG_APP_CBOR_PERSONDB_TUI_PERSONS)
		tui_persons_run();
#elif defined(CONFIG_APP_CBOR_PERSONDB_TUI_FLIGHTS)
		tui_flights_run();
#else
		tui_library_run();
#endif
	}
	return 0;
}
