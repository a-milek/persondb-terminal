/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight_scenario — the operations on the flight store's population: fill it
 * from the generator (flights.h), check it against the generator, reset it.
 * The flight database's counterpart of scenario.h; returns structs, never prints.
 */

#ifndef APP_CBOR_PERSONDB_FLIGHT_SCENARIO_H_
#define APP_CBOR_PERSONDB_FLIGHT_SCENARIO_H_

#include <stdint.h>

#include "flightdb.h"

/* The terminal's clock starts here, the instant the person build decides
 * access checks at (SCENARIO_BENCH_NOW), so both builds show the same time. */
#define FLIGHT_SCENARIO_NOW 1830000000u

struct flight_scenario {
	struct flightdb *db;
};

struct flight_verify_report {
	uint32_t airports_checked;
	uint32_t flights_checked;
	uint32_t bad;             /* unreadable, or differing from the generator */
	int64_t  ms;
};

int flight_scenario_open(struct flight_scenario *s);
int flight_scenario_close(struct flight_scenario *s);

/** How many flights the generator defines: the most a fill can add. */
uint32_t flight_scenario_flights_defined(void);

/**
 * @brief Write the next @p n flights of the schedule, after every airport the
 *        store does not hold yet, and publish them.
 *
 * Resumable: each call continues from the store's own counts, so a fill
 * interrupted between calls loses nothing it published.
 *
 * @param written  (out) flights written and published
 */
int flight_scenario_fill(struct flight_scenario *s, uint32_t n, uint32_t *written);

/** Read every stored record back and compare it with the generator. */
int flight_scenario_verify(struct flight_scenario *s, struct flight_verify_report *r);

/** Empty the flight store; the person store is not touched. */
int flight_scenario_erase(struct flight_scenario *s);

#endif /* APP_CBOR_PERSONDB_FLIGHT_SCENARIO_H_ */
