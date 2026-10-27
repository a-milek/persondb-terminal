/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight-terminal — entry point.
 *
 * Deliberately empty of logic: everything goes through the terminal in
 * ui_tui_main.c, which goes through flight_scenario.h and flightdb.h.
 */

#include <zephyr/kernel.h>

/** Provided by ui_tui_main.c. */
int ui_main(void);

int main(void)
{
	return ui_main();
}
