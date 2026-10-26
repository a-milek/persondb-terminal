/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * library_scenario — the operations on the library store's population: fill
 * it from the generator (library.h), check it, reset it. The library
 * database's counterpart of flight_scenario.h; returns structs, never prints.
 */

#ifndef APP_CBOR_PERSONDB_LIBRARY_SCENARIO_H_
#define APP_CBOR_PERSONDB_LIBRARY_SCENARIO_H_

#include <stdint.h>

#include "librarydb.h"

/* The terminal's clock starts here, as FLIGHT_SCENARIO_NOW does, so every
 * application shows the same date. */
#define LIBRARY_SCENARIO_NOW 1830000000u

struct library_scenario {
	struct librarydb *db;
};

struct library_verify_report {
	uint32_t books_checked;
	uint32_t loans_checked;
	uint32_t bad;             /* unreadable, differing from the generator, or
				   * a book and a loan that disagree */
	int64_t  ms;
};

int library_scenario_open(struct library_scenario *s);
int library_scenario_close(struct library_scenario *s);

/** Today, in days since 1970, by the terminal's clock. */
uint16_t library_scenario_today(void);

/** How many books the generator defines: the most a fill can add. */
uint32_t library_scenario_books_defined(void);

/**
 * @brief Write the next @p n books of the catalogue, with the loans the
 *        generator gives their copies, and publish them.
 *
 * Resumable: each call continues from the store's own counts. Copies whose
 * loans no longer fit in the loan store go on the shelf instead.
 *
 * @param written  (out) books written and published
 */
int library_scenario_fill(struct library_scenario *s, uint32_t n, uint32_t *written);

/**
 * Read every stored record back: books against the generator (their loans
 * aside, which the terminal changes), and every loan against the book it
 * names, both ways.
 */
int library_scenario_verify(struct library_scenario *s, struct library_verify_report *r);

/** Empty the library store; the other stores are not touched. */
int library_scenario_erase(struct library_scenario *s);

#endif /* APP_CBOR_PERSONDB_LIBRARY_SCENARIO_H_ */
