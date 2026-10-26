/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flights — the made-up airports and flights the flight store is filled with,
 * as a pure function of an index: the flight database's counterpart of
 * dataset.h. Fictional places on a fictional map, five fictional airlines and
 * one daily schedule of FLIGHTS_PER_DAY departures.
 *
 * Nothing here reads or writes storage. The terminal never shows this data
 * directly: it is written to flightdb by a fill, and the screens show what
 * the store holds. Flight i is the i-th departure of the day, so index order
 * is time order.
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

int flights_airport_count(void);

/** Fill @p out with airport @p i (< flights_airport_count()). */
void flights_airport(int i, struct flightdb_airport *out);

/** Fill @p out with flight @p index (< FLIGHTS_PER_DAY). */
void flights_flight(uint32_t index, struct flightdb_flight *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_FLIGHTS_H_ */
