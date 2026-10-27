/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight_scenario — the operations on the flight store's population: fill it
 * from the generator (flights.h) — airports, airlines, aircraft and crew,
 * then flights, each with its bookings, the passengers they need and the bags
 * checked on them — check it against the generator, reset it.
 * Returns structs, never prints.
 */

#ifndef APP_CBOR_PERSONDB_FLIGHT_SCENARIO_H_
#define APP_CBOR_PERSONDB_FLIGHT_SCENARIO_H_

#include <stdint.h>

#include "flightdb.h"

/* The terminal's clock starts here. */
#define FLIGHT_SCENARIO_NOW 1830000000u

struct flight_scenario {
	struct flightdb *db;
};

struct flight_verify_report {
	uint32_t airports_checked;
	uint32_t flights_checked;
	uint32_t passengers_checked;
	uint32_t bookings_checked;
	uint32_t airlines_checked;
	uint32_t aircraft_checked;
	uint32_t crew_checked;
	uint32_t bags_checked;
	uint32_t bad;             /* unreadable, or differing from the generator */
	int64_t  ms;
};

int flight_scenario_open(struct flight_scenario *s);
int flight_scenario_close(struct flight_scenario *s);

/** How many flights the generator defines: the most a fill can add. */
uint32_t flight_scenario_flights_defined(void);

/**
 * How many flights the store holds together with all their bookings and
 * bags: where the next fill starts. A store from an earlier build holds
 * flights without them, which a fill then completes.
 */
uint32_t flight_scenario_flights_complete(struct flight_scenario *s);

/**
 * @brief Complete the next @p n flights of the schedule — the flight record
 *        if the store lacks it, its bookings, every passenger those bookings
 *        name that the store lacks, and their bags — after every airport,
 *        airline, aircraft and crew member the store does not hold yet, and
 *        publish them.
 *
 * Resumable: each call continues from the store's own counts, so a fill
 * interrupted between calls loses nothing it published.
 *
 * @param written  (out) flights completed and published
 */
int flight_scenario_fill(struct flight_scenario *s, uint32_t n, uint32_t *written);

/** Read every stored record back and compare it with the generator. */
int flight_scenario_verify(struct flight_scenario *s, struct flight_verify_report *r);

/** Empty the flight store. */
int flight_scenario_erase(struct flight_scenario *s);

#endif /* APP_CBOR_PERSONDB_FLIGHT_SCENARIO_H_ */
