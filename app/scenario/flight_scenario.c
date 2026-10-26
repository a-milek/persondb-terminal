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
	return MIN(MIN(FLIGHTS_PER_DAY, (uint32_t)FLIGHTDB_FLIGHTS_MAX),
		   (uint32_t)FLIGHTDB_BOOKINGS_MAX / BOOKINGS_PER_FLIGHT);
}

uint32_t flight_scenario_flights_complete(struct flight_scenario *s)
{
	struct flightdb_counts c;
	uint32_t n, bags = 0;

	flightdb_counts(s->db, &c);
	n = MIN(c.flights, c.bookings / BOOKINGS_PER_FLIGHT);
	/* Bags are written in booking order with their flight, so the flights
	 * whose bags all fit in the count are the complete ones. */
	for (uint32_t i = 0; i < n; i++) {
		for (uint32_t k = i * BOOKINGS_PER_FLIGHT; k < (i + 1) * BOOKINGS_PER_FLIGHT; k++) {
			bags += flights_booking_bags(k);
		}
		if (bags > c.bags) {
			return i;
		}
	}
	return n;
}

/* Flight @p i's bookings, first the passengers they name that @p c does not
 * count yet, and after each its bags. Bookings name passengers in order on
 * the first lap (flights.h), so the passengers are always the next ones; bags
 * start where the flights before left off, @p bag. */
static int book_flight(struct flight_scenario *s, uint32_t i, struct flightdb_counts *c,
		       uint32_t bag)
{
	struct flightdb_passenger p;
	struct flightdb_booking b;
	struct flightdb_bag g;
	int rc;

	for (uint32_t k = i * BOOKINGS_PER_FLIGHT; k < (i + 1) * BOOKINGS_PER_FLIGHT; k++) {
		flights_booking(k, &b);
		while (c->passengers <= b.passenger) {
			flights_passenger(c->passengers, &p);
			rc = flightdb_passenger_put(s->db, c->passengers, &p);
			if (rc != 0) {
				return rc;
			}
			c->passengers++;
		}
		rc = flightdb_booking_put(s->db, k, &b);
		if (rc != 0) {
			return rc;
		}
		for (uint32_t j = 0; j < flights_booking_bags(k); j++) {
			flights_bag(k, j, &g);
			rc = flightdb_bag_put(s->db, bag, &g);
			if (rc != 0) {
				return rc;
			}
			bag++;
		}
	}
	c->bookings = (i + 1) * BOOKINGS_PER_FLIGHT;
	c->bags = bag;
	return 0;
}

/* The bags of every booking before flight @p first: where its bags go. */
static uint32_t bags_before(uint32_t first)
{
	uint32_t n = 0;

	for (uint32_t k = 0; k < first * BOOKINGS_PER_FLIGHT; k++) {
		n += flights_booking_bags(k);
	}
	return n;
}

/*
 * Airports, airlines, aircraft and crew: the records the flights and
 * bookings stand on, few enough to write in one go, each kind published as
 * soon as it is down. Crew and aircraft name flights a later fill writes;
 * until then the screens show those as not in the store.
 */
static int fill_reference(struct flight_scenario *s, struct flightdb_counts *c)
{
	struct flightdb_airport a;
	struct flightdb_airline l;
	struct flightdb_aircraft k;
	struct flightdb_crew m;
	uint32_t want;
	int rc = 0;

	want = MIN((uint32_t)flights_airport_count(), (uint32_t)FLIGHTDB_AIRPORTS_MAX);
	if (c->airports < want) {
		for (uint32_t i = c->airports; i < want && rc == 0; i++) {
			flights_airport((int)i, &a);
			rc = flightdb_airport_put(s->db, i, &a);
		}
		if (rc != 0) {
			return rc;
		}
		c->airports = want;
		rc = flightdb_counts_set(s->db, c);
		if (rc != 0) {
			return rc;
		}
	}
	want = MIN((uint32_t)flights_airline_count(), (uint32_t)FLIGHTDB_AIRLINES_MAX);
	if (c->airlines < want) {
		for (uint32_t i = c->airlines; i < want && rc == 0; i++) {
			flights_airline((int)i, &l);
			rc = flightdb_airline_put(s->db, i, &l);
		}
		if (rc != 0) {
			return rc;
		}
		c->airlines = want;
		rc = flightdb_counts_set(s->db, c);
		if (rc != 0) {
			return rc;
		}
	}
	want = MIN(flights_aircraft_count(), (uint32_t)FLIGHTDB_AIRCRAFT_MAX);
	if (c->aircraft < want) {
		for (uint32_t i = c->aircraft; i < want && rc == 0; i++) {
			flights_aircraft(i, &k);
			rc = flightdb_aircraft_put(s->db, i, &k);
		}
		if (rc != 0) {
			return rc;
		}
		c->aircraft = want;
		rc = flightdb_counts_set(s->db, c);
		if (rc != 0) {
			return rc;
		}
	}
	want = MIN(flights_crew_count(), (uint32_t)FLIGHTDB_CREW_MAX);
	if (c->crew < want) {
		for (uint32_t i = c->crew; i < want && rc == 0; i++) {
			flights_crew(i, &m);
			rc = flightdb_crew_put(s->db, i, &m);
		}
		if (rc != 0) {
			return rc;
		}
		c->crew = want;
		rc = flightdb_counts_set(s->db, c);
	}
	return rc;
}

int flight_scenario_fill(struct flight_scenario *s, uint32_t n, uint32_t *written)
{
	struct flightdb_flight f;
	struct flightdb_counts c, done;
	uint32_t first, end, bag;
	int rc;

	*written = 0;
	flightdb_counts(s->db, &c);
	rc = fill_reference(s, &c);
	if (rc != 0) {
		return rc;
	}

	first = flight_scenario_flights_complete(s);
	end = MIN(first + n, flight_scenario_flights_defined());
	bag = bags_before(first);
	done = c;
	for (uint32_t i = first; i < end && rc == 0; i++) {
		if (i >= c.flights) {
			flights_flight(i, &f);
			rc = flightdb_flight_put(s->db, i, &f);
			if (rc != 0) {
				break;
			}
			c.flights = i + 1;
		}
		rc = book_flight(s, i, &c, bag);
		if (rc == 0) {
			bag = c.bags;
			done = c;
			(*written)++;
		}
	}
	/* Publish the flights that were completed, even after a failure part-way;
	 * a half-booked flight stays unpublished and the next fill redoes it. */
	if (*written > 0) {
		int rc2 = flightdb_counts_set(s->db, &done);

		rc = (rc != 0) ? rc : rc2;
	}
	return rc;
}

int flight_scenario_verify(struct flight_scenario *s, struct flight_verify_report *r)
{
	struct flightdb_airport a, ga;
	struct flightdb_flight f, gf;
	struct flightdb_passenger p, gp;
	struct flightdb_booking b, gb;
	struct flightdb_airline l, gl;
	struct flightdb_aircraft k, gk;
	struct flightdb_crew m, gm;
	struct flightdb_bag g, gg;
	struct flightdb_counts c;
	uint32_t bag = 0;
	int64_t t0 = k_uptime_get();

	*r = (struct flight_verify_report){ 0 };
	flightdb_counts(s->db, &c);
	for (uint32_t i = 0; i < c.airports; i++) {
		flights_airport((int)i, &ga);
		if (flightdb_airport_get(s->db, i, &a) != 0 || !flightdb_airport_equal(&a, &ga)) {
			r->bad++;
		}
		r->airports_checked++;
	}
	for (uint32_t i = 0; i < c.flights; i++) {
		flights_flight(i, &gf);
		if (flightdb_flight_get(s->db, i, &f) != 0 || !flightdb_flight_equal(&f, &gf)) {
			r->bad++;
		}
		r->flights_checked++;
	}
	for (uint32_t i = 0; i < c.passengers; i++) {
		flights_passenger(i, &gp);
		if (flightdb_passenger_get(s->db, i, &p) != 0 ||
		    !flightdb_passenger_equal(&p, &gp)) {
			r->bad++;
		}
		r->passengers_checked++;
	}
	for (uint32_t i = 0; i < c.bookings; i++) {
		flights_booking(i, &gb);
		if (flightdb_booking_get(s->db, i, &b) != 0 || !flightdb_booking_equal(&b, &gb)) {
			r->bad++;
		}
		r->bookings_checked++;
	}
	for (uint32_t i = 0; i < c.airlines; i++) {
		flights_airline((int)i, &gl);
		if (flightdb_airline_get(s->db, i, &l) != 0 || !flightdb_airline_equal(&l, &gl)) {
			r->bad++;
		}
		r->airlines_checked++;
	}
	for (uint32_t i = 0; i < c.aircraft; i++) {
		flights_aircraft(i, &gk);
		if (flightdb_aircraft_get(s->db, i, &k) != 0 ||
		    !flightdb_aircraft_equal(&k, &gk)) {
			r->bad++;
		}
		r->aircraft_checked++;
	}
	for (uint32_t i = 0; i < c.crew; i++) {
		flights_crew(i, &gm);
		if (flightdb_crew_get(s->db, i, &m) != 0 || !flightdb_crew_equal(&m, &gm)) {
			r->bad++;
		}
		r->crew_checked++;
	}
	/* Bags in booking order, as a fill writes them. */
	for (uint32_t i = 0; i < c.bookings && bag < c.bags; i++) {
		for (uint32_t j = 0; j < flights_booking_bags(i) && bag < c.bags; j++) {
			flights_bag(i, j, &gg);
			if (flightdb_bag_get(s->db, bag, &g) != 0 || !flightdb_bag_equal(&g, &gg)) {
				r->bad++;
			}
			r->bags_checked++;
			bag++;
		}
	}
	if (bag < c.bags) {
		r->bad += c.bags - bag;  /* bags past every booking's own */
	}
	r->ms = k_uptime_get() - t0;
	return 0;
}

int flight_scenario_erase(struct flight_scenario *s)
{
	return flightdb_erase(s->db);
}
