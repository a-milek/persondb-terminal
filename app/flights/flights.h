/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flights — the made-up airports, airlines, aircraft, crew, flights,
 * passengers, bookings and bags the flight store is filled with, as a pure
 * function of an index: the flight database's counterpart of dataset.h.
 * Fictional places on a fictional map, five fictional airlines, one daily
 * schedule of FLIGHTS_PER_DAY departures and FLIGHTS_PASSENGERS travellers
 * holding BOOKINGS_PER_FLIGHT bookings on each, with their checked bags.
 *
 * Aircraft and crew come from the schedule: each flight is given an aircraft
 * of its carrier and type that has turned round since its last sector, and
 * crew of its carrier that have rested since their last duty — so how many
 * of each there are is whatever the schedule needs.
 *
 * Nothing here reads or writes storage. The terminal never shows this data
 * directly: it is written to flightdb by a fill, and the screens show what
 * the store holds. Flight i is the i-th departure of the day, so index order
 * is time order; bookings follow their flights, BOOKINGS_PER_FLIGHT each.
 *
 * Booking i is held by passenger i while i < FLIGHTS_PASSENGERS, so a fill
 * that writes bookings in order needs passengers only up to the booking it
 * is at; later bookings go to the same travellers again, in another order.
 */

#ifndef APP_CBOR_PERSONDB_FLIGHTS_H_
#define APP_CBOR_PERSONDB_FLIGHTS_H_

#include <stdint.h>

#include "flightdb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Departures in the daily schedule: one every six minutes. */
#define FLIGHTS_PER_DAY 240u

/** Bookings held on every flight: a sample of its load, not all of it. */
#define BOOKINGS_PER_FLIGHT 5u

/** Travellers the bookings belong to; each holds two. */
#define FLIGHTS_PASSENGERS 600u

int flights_airport_count(void);

/** Fill @p out with airport @p i (< flights_airport_count()). */
void flights_airport(int i, struct flightdb_airport *out);

/** Fill @p out with flight @p index (< FLIGHTS_PER_DAY). */
void flights_flight(uint32_t index, struct flightdb_flight *out);

/** Fill @p out with passenger @p index (< FLIGHTS_PASSENGERS). */
void flights_passenger(uint32_t index, struct flightdb_passenger *out);

/** Fill @p out with booking @p index (< FLIGHTS_PER_DAY * BOOKINGS_PER_FLIGHT). */
void flights_booking(uint32_t index, struct flightdb_booking *out);

int flights_airline_count(void);

/** Fill @p out with airline @p i (< flights_airline_count()). */
void flights_airline(int i, struct flightdb_airline *out);

uint32_t flights_aircraft_count(void);

/** Fill @p out with aircraft @p i (< flights_aircraft_count()). */
void flights_aircraft(uint32_t i, struct flightdb_aircraft *out);

uint32_t flights_crew_count(void);

/** Fill @p out with crew member @p i (< flights_crew_count()). */
void flights_crew(uint32_t i, struct flightdb_crew *out);

/** How many bags booking @p booking has checked: none unless checked in. */
uint32_t flights_booking_bags(uint32_t booking);

/** Fill @p out with bag @p k (< flights_booking_bags()) of booking @p booking. */
void flights_bag(uint32_t booking, uint32_t k, struct flightdb_bag *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_FLIGHTS_H_ */
