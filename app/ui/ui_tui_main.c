/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_main — the terminal: take the console UART and open straight into
 * the flight database. Q signs off.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "tui.h"
#include "ui_tui_flights.h"

int ui_main(void)
{
	int rc = rx_start();

	if (rc != 0) {
		printk("flight-terminal: console UART has no RX interrupt: %d\n", rc);
		return rc;
	}

	screen_enter();
	tui_flights_run();
	return 0;
}
