/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flightdb — the flight information API: airports, airlines, their aircraft
 * and crew, a daily schedule of flights, the passengers, their bookings on
 * those flights and the bags checked on them, in one store under its own
 * registry entry.
 *
 * Domain vocabulary only: no keys, no blob ids, no CBOR.
 * Records are addressed by position — flight 0..n-1 in departure order, and
 * every other kind 0..n-1 in the order it was written — and the store keeps
 * how many of each it holds, so a reader shows exactly what was written,
 * nothing more. References are those positions, held by the record that
 * points: a booking names its passenger and flight, a bag its booking, and an
 * aircraft or a crew member the flights it works.
 *
 * Concurrency: single-threaded (FINDINGS.md K7).
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
#define FLIGHTDB_SURNAME_MAX  15
#define FLIGHTDB_GIVEN_MAX    11
#define FLIGHTDB_TITLE_MAX    4    /* "MR", "MRS", "MSTR" */
#define FLIGHTDB_FFP_MAX      9    /* "XA1234567"; empty: not a member */
#define FLIGHTDB_LOCATOR_LEN  6    /* record locator, "K7QZ2X" */
#define FLIGHTDB_SEAT_MAX     3    /* "32A"; empty: none assigned */
#define FLIGHTDB_AIRLINE_CODE 2    /* "XA" */
#define FLIGHTDB_CALLSIGN_MAX 11
#define FLIGHTDB_ALLIANCE_MAX 11
#define FLIGHTDB_REG_MAX      6    /* registration, "VD-KRA" */
#define FLIGHTDB_SECTORS_MAX  8    /* flights one aircraft flies in the day */
#define FLIGHTDB_DUTIES_MAX   3    /* flights one crew member works */
#define FLIGHTDB_TAG_MAX      8    /* bag tag, "XA482913" */

/* Store capacity, declared to the maps when the store is created. */
#define FLIGHTDB_AIRPORTS_MAX   64
#define FLIGHTDB_FLIGHTS_MAX    1024
#define FLIGHTDB_PASSENGERS_MAX 1024
#define FLIGHTDB_BOOKINGS_MAX   4096
#define FLIGHTDB_AIRLINES_MAX   16
#define FLIGHTDB_AIRCRAFT_MAX   256
#define FLIGHTDB_CREW_MAX       1024
#define FLIGHTDB_BAGS_MAX       4096

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

/** Frequent flyer tier. */
enum flightdb_tier {
	FLIGHTDB_TIER_NONE,
	FLIGHTDB_TIER_BLUE,
	FLIGHTDB_TIER_SILVER,
	FLIGHTDB_TIER_GOLD,
	FLIGHTDB_TIER__COUNT,
};

struct flightdb_passenger {
	char     surname[FLIGHTDB_SURNAME_MAX + 1];
	char     given[FLIGHTDB_GIVEN_MAX + 1];
	char     title[FLIGHTDB_TITLE_MAX + 1];
	char     nationality[FLIGHTDB_COUNTRY_MAX + 1];
	uint16_t birth_year;
	char     ffp[FLIGHTDB_FFP_MAX + 1];
	uint8_t  tier;           /* enum flightdb_tier; NONE without an ffp */
};

/** Where a booking stands, as stored; check-in is kept beside it. */
enum flightdb_booking_status {
	FLIGHTDB_BK_CONFIRMED,
	FLIGHTDB_BK_WAITLISTED,
	FLIGHTDB_BK_CANCELLED,
	FLIGHTDB_BK__COUNT,
};

struct flightdb_booking {
	char     locator[FLIGHTDB_LOCATOR_LEN + 1];
	uint16_t passenger;      /* position in the passenger store */
	uint16_t flight;         /* position in the flight store */
	uint8_t  cabin;          /* 0..FLIGHTDB_CABINS-1, as in seats[] */
	char     seat[FLIGHTDB_SEAT_MAX + 1];
	uint8_t  status;         /* enum flightdb_booking_status */
	bool     checked_in;
	uint8_t  bags;
	uint32_t fare;           /* whole currency units */
};

struct flightdb_airline {
	char     code[FLIGHTDB_AIRLINE_CODE + 1];
	char     name[FLIGHTDB_AIRLINE_MAX + 1];
	char     callsign[FLIGHTDB_CALLSIGN_MAX + 1];
	char     hub[FLIGHTDB_CODE_LEN + 1];       /* airport code */
	char     country[FLIGHTDB_COUNTRY_MAX + 1];
	char     alliance[FLIGHTDB_ALLIANCE_MAX + 1];
	uint16_t founded;
};

struct flightdb_aircraft {
	char     reg[FLIGHTDB_REG_MAX + 1];
	char     type[FLIGHTDB_EQUIP_MAX + 1];      /* as in a flight's equipment */
	char     airline[FLIGHTDB_AIRLINE_CODE + 1];
	uint16_t seats[FLIGHTDB_CABINS];
	uint16_t built;          /* year */
	uint8_t  n_sectors;
	uint16_t sectors[FLIGHTDB_SECTORS_MAX];   /* flights, in departure order */
};

enum flightdb_rank {
	FLIGHTDB_RANK_CAPTAIN,
	FLIGHTDB_RANK_FIRST_OFFICER,
	FLIGHTDB_RANK_PURSER,
	FLIGHTDB_RANK_ATTENDANT,
	FLIGHTDB_RANK__COUNT,
};

struct flightdb_crew {
	uint32_t staff_no;
	char     surname[FLIGHTDB_SURNAME_MAX + 1];
	char     given[FLIGHTDB_GIVEN_MAX + 1];
	uint8_t  rank;           /* enum flightdb_rank */
	char     airline[FLIGHTDB_AIRLINE_CODE + 1];
	char     base[FLIGHTDB_CODE_LEN + 1];      /* airport code */
	uint8_t  n_duties;
	uint16_t duties[FLIGHTDB_DUTIES_MAX];     /* flights, in departure order */
};

enum flightdb_bag_status {
	FLIGHTDB_BAG_CHECKED,
	FLIGHTDB_BAG_LOADED,
	FLIGHTDB_BAG_OFFLOADED,
	FLIGHTDB_BAG_MISHANDLED,
	FLIGHTDB_BAG__COUNT,
};

struct flightdb_bag {
	char     tag[FLIGHTDB_TAG_MAX + 1];
	uint16_t booking;        /* position in the booking store */
	uint8_t  weight;         /* kg */
	char     dest[FLIGHTDB_CODE_LEN + 1];      /* airport code */
	uint8_t  status;         /* enum flightdb_bag_status */
	bool     priority;
};

/** How many records of each kind the store holds. */
struct flightdb_counts {
	uint32_t airports;
	uint32_t flights;
	uint32_t passengers;
	uint32_t bookings;
	uint32_t airlines;
	uint32_t aircraft;
	uint32_t crew;
	uint32_t bags;
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
	uint32_t passengers;
	uint32_t bookings;
	uint32_t airlines;
	uint32_t aircraft;
	uint32_t crew;
	uint32_t bags;
	uint32_t airport_buckets;    /* geometry the maps chose, read back */
	uint32_t flight_buckets;
	uint32_t passenger_buckets;
	uint32_t booking_buckets;
	uint32_t airline_buckets;
	uint32_t aircraft_buckets;
	uint32_t crew_buckets;
	uint32_t bag_buckets;
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
 * A store from an earlier build is upgraded in place: version 1 (airports
 * and flights) and version 2 (with passengers and bookings) keep every record
 * they hold, and the tables they lack are added beside them, empty.
 *
 * @retval 0        ready
 * @retval -ENOTSUP the store was written by a build with another format
 * @retval -EIO     flash error or a corrupt superblock
 */
int flightdb_open(struct flightdb **db);

/** Detach and unmount. The store's contents are untouched. */
int flightdb_close(struct flightdb *db);

/**
 * @brief Drop every record of every kind and start an empty store.
 *
 * Destroys this store's maps only, leaving anything else under other registry
 * keys on the flash as it was.
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
int flightdb_passenger_put(struct flightdb *db, uint32_t index,
			   const struct flightdb_passenger *p);
int flightdb_booking_put(struct flightdb *db, uint32_t index,
			 const struct flightdb_booking *b);
int flightdb_airline_put(struct flightdb *db, uint32_t index,
			 const struct flightdb_airline *a);
int flightdb_aircraft_put(struct flightdb *db, uint32_t index,
			  const struct flightdb_aircraft *a);
int flightdb_crew_put(struct flightdb *db, uint32_t index,
		      const struct flightdb_crew *c);
int flightdb_bag_put(struct flightdb *db, uint32_t index,
		     const struct flightdb_bag *b);

/**
 * @retval 0       found
 * @retval -ENOENT @p index is at or past the count
 * @retval -EILSEQ the stored record is corrupt
 */
int flightdb_airport_get(struct flightdb *db, uint32_t index,
			 struct flightdb_airport *out);
int flightdb_flight_get(struct flightdb *db, uint32_t index,
			struct flightdb_flight *out);
int flightdb_passenger_get(struct flightdb *db, uint32_t index,
			   struct flightdb_passenger *out);
int flightdb_booking_get(struct flightdb *db, uint32_t index,
			 struct flightdb_booking *out);
int flightdb_airline_get(struct flightdb *db, uint32_t index,
			 struct flightdb_airline *out);
int flightdb_aircraft_get(struct flightdb *db, uint32_t index,
			  struct flightdb_aircraft *out);
int flightdb_crew_get(struct flightdb *db, uint32_t index,
		      struct flightdb_crew *out);
int flightdb_bag_get(struct flightdb *db, uint32_t index,
		     struct flightdb_bag *out);

/** How many records the store holds. */
void flightdb_counts(struct flightdb *db, struct flightdb_counts *out);

/** Publish the records @p c counts, of every kind: one superblock write. */
int flightdb_counts_set(struct flightdb *db, const struct flightdb_counts *c);

/* -- helpers ------------------------------------------------------------ */

/** True if both records encode identically, i.e. are the same stored record. */
bool flightdb_flight_equal(const struct flightdb_flight *a,
			   const struct flightdb_flight *b);
bool flightdb_airport_equal(const struct flightdb_airport *a,
			    const struct flightdb_airport *b);
bool flightdb_passenger_equal(const struct flightdb_passenger *a,
			      const struct flightdb_passenger *b);
bool flightdb_booking_equal(const struct flightdb_booking *a,
			    const struct flightdb_booking *b);
bool flightdb_airline_equal(const struct flightdb_airline *a,
			    const struct flightdb_airline *b);
bool flightdb_aircraft_equal(const struct flightdb_aircraft *a,
			     const struct flightdb_aircraft *b);
bool flightdb_crew_equal(const struct flightdb_crew *a,
			 const struct flightdb_crew *b);
bool flightdb_bag_equal(const struct flightdb_bag *a,
			const struct flightdb_bag *b);

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

const char *flightdb_tier_name(enum flightdb_tier t);
const char *flightdb_booking_status_name(enum flightdb_booking_status s);
/** "F", "C" or "Y". */
const char *flightdb_cabin_code(uint8_t cabin);
/** "CPT", "FO", "PUR" or "FA". */
const char *flightdb_rank_code(enum flightdb_rank r);
const char *flightdb_rank_name(enum flightdb_rank r);
const char *flightdb_bag_status_name(enum flightdb_bag_status s);

int flightdb_stat(struct flightdb *db, struct flightdb_stat *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_FLIGHTDB_H_ */
