/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flightdb — the flight information API: airports and a daily schedule of
 * flights, stored on the same stack as the person database but as a separate
 * store with its own registry entry. Neither store can see the other's
 * records, and erasing one leaves the other intact.
 *
 * Domain vocabulary only, as in persondb.h: no keys, no blob ids, no CBOR.
 * Records are addressed by position — airport 0..n-1, flight 0..n-1 in
 * departure order — and the store keeps how many of each it holds, so a
 * reader shows exactly what was written, nothing more.
 *
 * Concurrency: single-threaded, like persondb (FINDINGS.md K7). The two are
 * never built into the same image.
 */

#ifndef APP_CBOR_PERSONDB_FLIGHTDB_H_
#define APP_CBOR_PERSONDB_FLIGHTDB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field capacities: fixed, so a record is a plain value type. */
#define FLIGHTDB_CODE_LEN     3    /* IATA airport code */
#define FLIGHTDB_NAME_MAX     23
#define FLIGHTDB_CITY_MAX     15
#define FLIGHTDB_COUNTRY_MAX  15
#define FLIGHTDB_DESIG_MAX    7    /* "XA0421" */
#define FLIGHTDB_AIRLINE_MAX  19
#define FLIGHTDB_EQUIP_MAX    4    /* ICAO type, "A320" */
#define FLIGHTDB_GATE_MAX     4
#define FLIGHTDB_CABINS       3    /* first, business, economy */

/* Store capacity, declared to the maps when the store is created. */
#define FLIGHTDB_AIRPORTS_MAX 64
#define FLIGHTDB_FLIGHTS_MAX  1024

#define FLIGHTDB_MIN_PER_DAY  1440u

struct flightdb_airport {
	char     code[FLIGHTDB_CODE_LEN + 1];
	char     name[FLIGHTDB_NAME_MAX + 1];
	char     city[FLIGHTDB_CITY_MAX + 1];
	char     country[FLIGHTDB_COUNTRY_MAX + 1];
	int8_t   utc_offset;     /* hours */
	uint16_t elevation;      /* feet */
	uint8_t  runways;
	uint8_t  gates;
};

struct flightdb_flight {
	char     desig[FLIGHTDB_DESIG_MAX + 1];
	char     airline[FLIGHTDB_AIRLINE_MAX + 1];
	char     from[FLIGHTDB_CODE_LEN + 1];
	char     to[FLIGHTDB_CODE_LEN + 1];
	uint16_t dep;            /* scheduled departure, minute of the day, UTC */
	uint16_t block;          /* scheduled gate-to-gate time, minutes */
	uint16_t delay;          /* minutes */
	bool     cancelled;
	uint16_t distance;       /* nautical miles */
	char     equipment[FLIGHTDB_EQUIP_MAX + 1];
	char     gate[FLIGHTDB_GATE_MAX + 1];
	uint8_t  terminal;
	uint16_t seats[FLIGHTDB_CABINS];
	uint16_t booked[FLIGHTDB_CABINS];
};

/** Where a flight is at a given minute of the day. */
enum flightdb_status {
	FLIGHTDB_SCHEDULED,
	FLIGHTDB_DELAYED,
	FLIGHTDB_BOARDING,
	FLIGHTDB_FINAL_CALL,
	FLIGHTDB_DEPARTED,
	FLIGHTDB_LANDED,
	FLIGHTDB_CANCELLED,
};

struct flightdb_stat {
	uint32_t airports;           /* records held */
	uint32_t flights;
	uint32_t airport_buckets;    /* geometry the maps chose, read back */
	uint32_t flight_buckets;
	size_t   partition_bytes;
	uint32_t enospc_hits;
	uint64_t map_gets;
	uint64_t map_sets;
	uint64_t flash_reads;        /* zero unless CONFIG_BLOB_DB_IOSTATS */
	uint64_t flash_writes;
	uint64_t flash_erases;
	bool     io_measured;
};

struct flightdb;

/* -- lifecycle ---------------------------------------------------------- */

/**
 * @brief Mount the stack and attach to the flight store, creating an empty
 *        one on first use.
 *
 * @retval 0        ready
 * @retval -ENOTSUP the store was written by a build with another format
 * @retval -EIO     flash error or a corrupt superblock
 */
int flightdb_open(struct flightdb **db);

/** Detach and unmount. The store's contents are untouched. */
int flightdb_close(struct flightdb *db);

/**
 * @brief Drop every airport and flight and start an empty store.
 *
 * Destroys this store's maps only: the person database, which shares the
 * flash, is left as it was.
 */
int flightdb_erase(struct flightdb *db);

/* -- records ------------------------------------------------------------ */

/**
 * @brief Write the record at @p index. Writing at or past the count does not
 *        make it visible: flightdb_counts_set() does, once a batch is down.
 *
 * @retval -EINVAL index past capacity, or a field violating its bounds
 * @retval -ENOSPC a map overflowed (counted in the stat block)
 */
int flightdb_airport_put(struct flightdb *db, uint32_t index,
			 const struct flightdb_airport *a);
int flightdb_flight_put(struct flightdb *db, uint32_t index,
			const struct flightdb_flight *f);

/**
 * @retval 0       found
 * @retval -ENOENT @p index is at or past the count
 * @retval -EILSEQ the stored record is corrupt
 */
int flightdb_airport_get(struct flightdb *db, uint32_t index,
			 struct flightdb_airport *out);
int flightdb_flight_get(struct flightdb *db, uint32_t index,
			struct flightdb_flight *out);

/** How many records the store holds. */
void flightdb_counts(struct flightdb *db, uint32_t *airports, uint32_t *flights);

/** Publish @p airports and @p flights records: one superblock write. */
int flightdb_counts_set(struct flightdb *db, uint32_t airports, uint32_t flights);

/* -- helpers ------------------------------------------------------------ */

/** True if both records encode identically, i.e. are the same stored record. */
bool flightdb_flight_equal(const struct flightdb_flight *a,
			   const struct flightdb_flight *b);
bool flightdb_airport_equal(const struct flightdb_airport *a,
			    const struct flightdb_airport *b);

/**
 * @brief Status at minute @p now of the day, UTC, from the record alone.
 *
 * Within today's schedule: before the estimated departure a flight is on time
 * or delayed, boarding from 40 minutes out and on final call for the last 10,
 * then departed until its block time has passed, then landed. Past midnight
 * every flight is scheduled again.
 */
enum flightdb_status flightdb_status(const struct flightdb_flight *f, uint32_t now);

const char *flightdb_status_name(enum flightdb_status s);

int flightdb_stat(struct flightdb *db, struct flightdb_stat *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_FLIGHTDB_H_ */
