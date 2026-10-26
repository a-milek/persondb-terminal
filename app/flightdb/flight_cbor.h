/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight_cbor — the flight store's wire format, private to flightdb.
 *
 * Canonical CBOR maps with small integer keys, as in person_cbor.h:
 *
 *   airport    { 1: code, 2: name, 3: city, 4: country, 5: utc_offset,
 *                6: elevation, 7: runways, 8: gates }
 *   flight     { 1: designator, 2: airline, 3: from, 4: to, 5: dep,
 *                6: block, 7: delay, 8: cancelled, 9: distance,
 *               10: equipment, 11: gate, 12: terminal, 13: [seats],
 *               14: [booked] }
 *   superblock { 1: version, 2: airports root, 3: flights root,
 *                4: airports held, 5: flights held }
 */

#ifndef APP_CBOR_PERSONDB_FLIGHT_CBOR_H_
#define APP_CBOR_PERSONDB_FLIGHT_CBOR_H_

#include <stddef.h>
#include <stdint.h>

#include "flightdb.h"

/* Every field at its capacity, with room to spare. The shipped data encodes
 * to at most 58 B per airport and 81 B per flight. */
#define AIRPORT_CBOR_MAX 128
#define FLIGHT_CBOR_MAX  160
#define FLDB_SB_CBOR_MAX 64

/* Bump on any change to an encoding below. */
#define FLDB_SB_VERSION 1

struct fldb_superblock {
	uint32_t version;
	uint64_t airports_root;
	uint64_t flights_root;
	uint32_t n_airports;
	uint32_t n_flights;
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

int fldb_sb_cbor_encode(const struct fldb_superblock *sb, uint8_t *buf,
			size_t buf_sz, size_t *out_len);
/** @retval -ENOTSUP another version (out->version holds it) */
int fldb_sb_cbor_decode(const uint8_t *buf, size_t len,
			struct fldb_superblock *out);

#endif /* APP_CBOR_PERSONDB_FLIGHT_CBOR_H_ */
