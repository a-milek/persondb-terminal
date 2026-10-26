/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight_scenario — fill, verify and reset the flight store. See the header.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "flight_scenario.h"
#include "flights.h"

int flight_scenario_open(struct flight_scenario *s)
{
	return flightdb_open(&s->db);
}

int flight_scenario_close(struct flight_scenario *s)
{
	return flightdb_close(s->db);
}

uint32_t flight_scenario_flights_defined(void)
{
	return MIN(FLIGHTS_PER_DAY, (uint32_t)FLIGHTDB_FLIGHTS_MAX);
}

int flight_scenario_fill(struct flight_scenario *s, uint32_t n, uint32_t *written)
{
	struct flightdb_airport a;
	struct flightdb_flight f;
	uint32_t na, nf, want_a, end;
	int rc = 0;

	*written = 0;
	flightdb_counts(s->db, &na, &nf);

	want_a = MIN((uint32_t)flights_airport_count(), (uint32_t)FLIGHTDB_AIRPORTS_MAX);
	if (na < want_a) {
		for (uint32_t i = na; i < want_a; i++) {
			flights_airport((int)i, &a);
			rc = flightdb_airport_put(s->db, i, &a);
			if (rc != 0) {
				return rc;
			}
		}
		na = want_a;
		rc = flightdb_counts_set(s->db, na, nf);
		if (rc != 0) {
			return rc;
		}
	}

	end = MIN(nf + n, flight_scenario_flights_defined());
	for (uint32_t i = nf; i < end && rc == 0; i++) {
		flights_flight(i, &f);
		rc = flightdb_flight_put(s->db, i, &f);
		if (rc == 0) {
			(*written)++;
		}
	}
	/* Publish what made it, even after a failure part-way. */
	if (*written > 0) {
		int rc2 = flightdb_counts_set(s->db, na, nf + *written);

		rc = (rc != 0) ? rc : rc2;
	}
	return rc;
}

int flight_scenario_verify(struct flight_scenario *s, struct flight_verify_report *r)
{
	struct flightdb_airport a, ga;
	struct flightdb_flight f, gf;
	int64_t t0 = k_uptime_get();
	uint32_t na, nf;

	*r = (struct flight_verify_report){ 0 };
	flightdb_counts(s->db, &na, &nf);
	for (uint32_t i = 0; i < na; i++) {
		flights_airport((int)i, &ga);
		if (flightdb_airport_get(s->db, i, &a) != 0 || !flightdb_airport_equal(&a, &ga)) {
			r->bad++;
		}
		r->airports_checked++;
	}
	for (uint32_t i = 0; i < nf; i++) {
		flights_flight(i, &gf);
		if (flightdb_flight_get(s->db, i, &f) != 0 || !flightdb_flight_equal(&f, &gf)) {
			r->bad++;
		}
		r->flights_checked++;
	}
	r->ms = k_uptime_get() - t0;
	return 0;
}

int flight_scenario_erase(struct flight_scenario *s)
{
	return flightdb_erase(s->db);
}
