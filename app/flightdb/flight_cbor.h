/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight_cbor — the flight store's wire format, private to flightdb.
 *
 * Canonical CBOR maps with small integer keys:
 *
 *   airport    { 1: code, 2: name, 3: city, 4: country, 5: utc_offset,
 *                6: elevation, 7: runways, 8: gates }
 *   flight     { 1: designator, 2: airline, 3: from, 4: to, 5: dep,
 *                6: block, 7: delay, 8: cancelled, 9: distance,
 *               10: equipment, 11: gate, 12: terminal, 13: [seats],
 *               14: [booked] }
 *   passenger  { 1: surname, 2: given name, 3: title, 4: nationality,
 *                5: birth year, 6: ffp number, 7: tier }
 *   booking    { 1: locator, 2: passenger, 3: flight, 4: cabin, 5: seat,
 *                6: status, 7: checked in, 8: bags, 9: fare }
 *   airline    { 1: code, 2: name, 3: callsign, 4: hub, 5: country,
 *                6: alliance, 7: founded }
 *   aircraft   { 1: registration, 2: type, 3: airline, 4: [seats],
 *                5: built, 6: [sectors] }
 *   crew       { 1: staff number, 2: surname, 3: given name, 4: rank,
 *                5: airline, 6: base, 7: [duties] }
 *   bag        { 1: tag, 2: booking, 3: weight, 4: destination, 5: status,
 *                6: priority }
 *   superblock { 1: version, 2: airports root, 3: flights root,
 *                4: airports held, 5: flights held,
 *                6: passengers root, 7: bookings root,
 *                8: passengers held, 9: bookings held,          (version 2+)
 *               10: airlines root, 11: aircraft root, 12: crew root,
 *               13: bags root, 14: airlines held, 15: aircraft held,
 *               16: crew held, 17: bags held }                  (version 3)
 */

#ifndef APP_CBOR_PERSONDB_FLIGHT_CBOR_H_
#define APP_CBOR_PERSONDB_FLIGHT_CBOR_H_

#include <stddef.h>
#include <stdint.h>

#include "flightdb.h"

/* Every field at its capacity, with room to spare. The shipped data encodes
 * to at most 58 B per airport, 81 B per flight, 64 B per passenger, 37 B per
 * booking, 76 B per airline, 56 B per aircraft, 49 B per crew member and
 * 26 B per bag. */
#define AIRPORT_CBOR_MAX   128
#define FLIGHT_CBOR_MAX    160
#define PASSENGER_CBOR_MAX 112
#define BOOKING_CBOR_MAX   64
#define AIRLINE_CBOR_MAX   128
#define AIRCRAFT_CBOR_MAX  96
#define CREW_CBOR_MAX      80
#define BAG_CBOR_MAX       48
#define FLDB_SB_CBOR_MAX   160

/* Bump on any change to an encoding below. Earlier versions still decode,
 * with the roots they lack zero, so flightdb can add those tables: version 1
 * had airports and flights, version 2 added passengers and bookings. */
#define FLDB_SB_VERSION    3
#define FLDB_SB_VERSION_V2 2
#define FLDB_SB_VERSION_V1 1

struct fldb_superblock {
	uint32_t version;
	uint64_t airports_root;
	uint64_t flights_root;
	uint32_t n_airports;
	uint32_t n_flights;
	uint64_t passengers_root;
	uint64_t bookings_root;
	uint32_t n_passengers;
	uint32_t n_bookings;
	uint64_t airlines_root;
	uint64_t aircraft_root;
	uint64_t crew_root;
	uint64_t bags_root;
	uint32_t n_airlines;
	uint32_t n_aircraft;
	uint32_t n_crew;
	uint32_t n_bags;
};

/** @retval -ENOMEM buf too small  @retval -EINVAL a field out of bounds */
int airport_cbor_encode(const struct flightdb_airport *a, uint8_t *buf,
			size_t buf_sz, size_t *out_len);
/** @retval -EILSEQ not a well-formed airport */
int airport_cbor_decode(const uint8_t *buf, size_t len,
			struct flightdb_airport *out);

int flight_cbor_encode(const struct flightdb_flight *f, uint8_t *buf,
		       size_t buf_sz, size_t *out_len);
int flight_cbor_decode(const uint8_t *buf, size_t len,
		       struct flightdb_flight *out);

int passenger_cbor_encode(const struct flightdb_passenger *p, uint8_t *buf,
			  size_t buf_sz, size_t *out_len);
int passenger_cbor_decode(const uint8_t *buf, size_t len,
			  struct flightdb_passenger *out);

int booking_cbor_encode(const struct flightdb_booking *b, uint8_t *buf,
			size_t buf_sz, size_t *out_len);
int booking_cbor_decode(const uint8_t *buf, size_t len,
			struct flightdb_booking *out);

int airline_cbor_encode(const struct flightdb_airline *a, uint8_t *buf,
			size_t buf_sz, size_t *out_len);
int airline_cbor_decode(const uint8_t *buf, size_t len,
			struct flightdb_airline *out);

int aircraft_cbor_encode(const struct flightdb_aircraft *a, uint8_t *buf,
			 size_t buf_sz, size_t *out_len);
int aircraft_cbor_decode(const uint8_t *buf, size_t len,
			 struct flightdb_aircraft *out);

int crew_cbor_encode(const struct flightdb_crew *c, uint8_t *buf,
		     size_t buf_sz, size_t *out_len);
int crew_cbor_decode(const uint8_t *buf, size_t len, struct flightdb_crew *out);

int bag_cbor_encode(const struct flightdb_bag *b, uint8_t *buf,
		    size_t buf_sz, size_t *out_len);
int bag_cbor_decode(const uint8_t *buf, size_t len, struct flightdb_bag *out);

int fldb_sb_cbor_encode(const struct fldb_superblock *sb, uint8_t *buf,
			size_t buf_sz, size_t *out_len);
/** Versions 1 to 3; always encodes version 3.
 *  @retval -ENOTSUP another version (out->version holds it) */
int fldb_sb_cbor_decode(const uint8_t *buf, size_t len,
			struct fldb_superblock *out);

#endif /* APP_CBOR_PERSONDB_FLIGHT_CBOR_H_ */
