/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ui_tui_apps — the applications the sign-on screen (ui_tui_main.c) offers.
 * Each owns its database: it opens the store on entry and closes it before
 * handing the terminal back, so only one store is open at a time (closing
 * one unmounts the medium they share).
 */

#ifndef APP_CBOR_PERSONDB_UI_TUI_APPS_H_
#define APP_CBOR_PERSONDB_UI_TUI_APPS_H_

#include <stdint.h>

/** Run the person database until the operator presses S. */
void tui_persons_run(void);
/** Open the person store just long enough to count what it holds. */
int tui_persons_count(uint32_t *persons);

/** Run the flight database until the operator presses S. */
void tui_flights_run(void);
/** Open the flight store just long enough to count what it holds. */
int tui_flights_count(uint32_t *airports, uint32_t *flights);

/** Run the library catalogue until the operator presses S. */
void tui_library_run(void);
/** Open the library store just long enough to count what it holds. */
int tui_library_count(uint32_t *books, uint32_t *on_loan);

#endif /* APP_CBOR_PERSONDB_UI_TUI_APPS_H_ */
