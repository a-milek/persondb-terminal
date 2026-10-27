/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flightdb — the flight information API, on L2 map_ops + rootreg + blob_db.
 *
 * Layout. One registry key of its own, one superblock, eight kvhash
 * instances:
 *
 *   rootreg[ ROOTREG_KEY('FLDB', 0) ] -> superblock -> airports map
 *                                                   -> flights map
 *                                                   -> passengers map
 *                                                   -> bookings map
 *                                                   -> airlines map
 *                                                   -> aircraft map
 *                                                   -> crew map
 *                                                   -> bags map
 *
 * Keys are positions, fixed width,
 * after a letter per kind: "aNNNN" airports, "fNNNN" flights, "pNNNN"
 * passengers, "bNNNN" bookings, "lNNNN" airlines, "kNNNN" aircraft, "cNNNN"
 * crew and "gNNNN" bags.
 *
 * This is the only file of the flight database that names a key, a blob id or a
 * map operation (the flight half of DESIGN.md A7).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include <app/lib/blob_db.h>
#include <app/lib/blob_db_iostats.h>
#include <app/lib/containers/kvhash.h>
#include <app/lib/rootreg.h>

#include "flight_cbor.h"
#include "flightdb.h"

LOG_MODULE_REGISTER(flightdb, CONFIG_APP_CBOR_PERSONDB_LOG_LEVEL);

#define FLDB_MAGIC       0x464c4442u /* 'FLDB' */
#define FLDB_ROOTREG_KEY ROOTREG_KEY(FLDB_MAGIC, 0)

#define KEY_SZ 6

struct flightdb {
	bool                   open;
	uint64_t               sb_id;
	struct fldb_superblock sb;
	const struct map_ops  *ops;
	struct flightdb_stat   st;
};

/* One store per image. */
static struct flightdb g_db;

/* CONFIG_APP_CBOR_PERSONDB_FRESH_START has run this boot. */
static bool g_erased;

/* -- plumbing ----------------------------------------------------------- */

/* Callers bound @p index by FLIGHTDB_*_MAX, so it fits four hex digits; the
 * mask tells the compiler so. */
static void key(char *buf, char kind, uint32_t index)
{
	snprintf(buf, KEY_SZ, "%c%04X", kind, (unsigned)(index & 0xFFFFu));
}

static int map_get(struct flightdb *db, uint64_t root, const char *k, void *out,
		   size_t out_sz, size_t *out_len)
{
	db->st.map_gets++;
	return db->ops->get(root, k, strlen(k), out, out_sz, out_len);
}

static int map_set(struct flightdb *db, uint64_t root, const char *k,
		   const void *val, size_t len)
{
	int rc;

	db->st.map_sets++;
	rc = db->ops->set(root, k, strlen(k), val, len);
	if (rc == -ENOSPC) {
		/* Bucket overflow or the directory blob unplaceable — the same
		 * two walls of kvhash (FINDINGS.md K2, K13). */
		db->st.enospc_hits++;
		LOG_ERR("map full on key '%s'", k);
	}
	return rc;
}

static int map_put(struct flightdb *db, uint64_t root, char kind, uint32_t index,
		   const void *val, size_t len)
{
	char k[KEY_SZ];

	key(k, kind, index);
	return map_set(db, root, k, val, len);
}

/* A missing or oversized value is a corrupt record to the caller. */
static int map_read(struct flightdb *db, uint64_t root, char kind, uint32_t index,
		    void *out, size_t out_sz, size_t *out_len)
{
	char k[KEY_SZ];
	int rc;

	key(k, kind, index);
	rc = map_get(db, root, k, out, out_sz, out_len);
	return (rc == -ENOMEM) ? -EILSEQ : rc;
}

static int sb_commit(struct flightdb *db)
{
	uint8_t buf[FLDB_SB_CBOR_MAX];
	size_t len;
	int rc = fldb_sb_cbor_encode(&db->sb, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	return blob_db_update(db->sb_id, buf, len);
}

/* -- lifecycle ---------------------------------------------------------- */

static int create_map(struct flightdb *db, uint64_t *root, const struct map_config *cfg)
{
	*root = blob_db_alloc_id();
	if (*root == 0) {
		return -EIO;
	}
	return db->ops->create(*root, cfg);
}

/* The two tables version 2 added. Measured with the shipped data:
 * passengers encode to at most 64 B, bookings to 37 B. */
static int create_people_maps(struct flightdb *db)
{
	const struct map_config passengers_cfg = {
		.expected_entries = FLIGHTDB_PASSENGERS_MAX,
		.typical_entry_bytes = 60,
		.max_entry_bytes = KEY_SZ + PASSENGER_CBOR_MAX,
	};
	const struct map_config bookings_cfg = {
		.expected_entries = FLIGHTDB_BOOKINGS_MAX,
		.typical_entry_bytes = 40,
		.max_entry_bytes = KEY_SZ + BOOKING_CBOR_MAX,
	};
	int rc = create_map(db, &db->sb.passengers_root, &passengers_cfg);

	if (rc != 0) {
		return rc;
	}
	db->sb.n_passengers = 0;
	db->sb.n_bookings = 0;
	return create_map(db, &db->sb.bookings_root, &bookings_cfg);
}

/* The four tables version 3 added. Measured with the shipped data: airlines
 * encode to at most 76 B, aircraft to 56 B, crew to 49 B, bags to 26 B. */
static int create_ops_maps(struct flightdb *db)
{
	const struct map_config airlines_cfg = {
		.expected_entries = FLIGHTDB_AIRLINES_MAX,
		.typical_entry_bytes = 75,
		.max_entry_bytes = KEY_SZ + AIRLINE_CBOR_MAX,
	};
	const struct map_config aircraft_cfg = {
		.expected_entries = FLIGHTDB_AIRCRAFT_MAX,
		.typical_entry_bytes = 55,
		.max_entry_bytes = KEY_SZ + AIRCRAFT_CBOR_MAX,
	};
	const struct map_config crew_cfg = {
		.expected_entries = FLIGHTDB_CREW_MAX,
		.typical_entry_bytes = 50,
		.max_entry_bytes = KEY_SZ + CREW_CBOR_MAX,
	};
	const struct map_config bags_cfg = {
		.expected_entries = FLIGHTDB_BAGS_MAX,
		.typical_entry_bytes = 30,
		.max_entry_bytes = KEY_SZ + BAG_CBOR_MAX,
	};
	int rc = create_map(db, &db->sb.airlines_root, &airlines_cfg);

	if (rc == 0) {
		rc = create_map(db, &db->sb.aircraft_root, &aircraft_cfg);
	}
	if (rc == 0) {
		rc = create_map(db, &db->sb.crew_root, &crew_cfg);
	}
	if (rc == 0) {
		rc = create_map(db, &db->sb.bags_root, &bags_cfg);
	}
	db->sb.n_airlines = 0;
	db->sb.n_aircraft = 0;
	db->sb.n_crew = 0;
	db->sb.n_bags = 0;
	return rc;
}

/*
 * Allocate and create every map, then bind the superblock last: that one
 * write publishes the store. A crash before it leaks the map roots until a
 * format (FINDINGS.md B8).
 */
static int create_store(struct flightdb *db)
{
	/* Measured with the shipped data: airports encode to at most 58 B,
	 * flights to 81 B, each under a 5-byte key. */
	const struct map_config airports_cfg = {
		.expected_entries = FLIGHTDB_AIRPORTS_MAX,
		.typical_entry_bytes = 60,
		.max_entry_bytes = KEY_SZ + AIRPORT_CBOR_MAX,
	};
	const struct map_config flights_cfg = {
		.expected_entries = FLIGHTDB_FLIGHTS_MAX,
		.typical_entry_bytes = 85,
		.max_entry_bytes = KEY_SZ + FLIGHT_CBOR_MAX,
	};
	int rc;

	memset(&db->sb, 0, sizeof(db->sb));
	db->sb.version = FLDB_SB_VERSION;

	rc = create_map(db, &db->sb.airports_root, &airports_cfg);
	if (rc != 0) {
		return rc;
	}
	rc = create_map(db, &db->sb.flights_root, &flights_cfg);
	if (rc != 0) {
		return rc;
	}
	rc = create_people_maps(db);
	if (rc != 0) {
		return rc;
	}
	rc = create_ops_maps(db);
	if (rc != 0) {
		return rc;
	}
	LOG_INF("created flight store");
	return sb_commit(db);   /* the commit point */
}

/*
 * An older store lacks the tables later versions added: version 1 has
 * airports and flights only, version 2 also passengers and bookings. Add the
 * missing maps and rewrite the superblock as the current version, keeping
 * every count it had: the same commit point as a create, and a crash before
 * it leaves the old version, which the next open upgrades again.
 */
static int upgrade_store(struct flightdb *db)
{
	uint32_t from = db->sb.version;
	int rc = 0;

	if (from < FLDB_SB_VERSION_V2) {
		rc = create_people_maps(db);
	}
	if (rc == 0) {
		rc = create_ops_maps(db);
	}
	if (rc != 0) {
		return rc;
	}
	db->sb.version = FLDB_SB_VERSION;
	LOG_INF("flight store upgraded from version %u to %u: %u flights, %u bookings kept",
		from, FLDB_SB_VERSION, db->sb.n_flights, db->sb.n_bookings);
	return sb_commit(db);
}

static int geometry(size_t *partition_bytes)
{
	const struct flash_area *fa;
	int rc = flash_area_open(PARTITION_ID(storage_partition), &fa);

	if (rc < 0) {
		return rc;
	}
	*partition_bytes = fa->fa_size;
	flash_area_close(fa);
	return 0;
}

int flightdb_open(struct flightdb **out)
{
	struct flightdb *db = &g_db;
	uint8_t buf[FLDB_SB_CBOR_MAX];
	size_t got = 0;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}
	memset(&db->st, 0, sizeof(db->st));
	db->ops = &kvhash_map_ops;

	rc = blob_db_mount();
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("blob_db_mount: %d", rc);
		return rc;
	}
	/* Once per boot: later opens keep what this run writes. */
	if (IS_ENABLED(CONFIG_APP_CBOR_PERSONDB_FRESH_START) && !g_erased) {
		g_erased = true;
		rc = blob_db_erase_all();
		if (rc != 0) {
			return rc;
		}
	}
	rc = rootreg_init();
	if (rc != 0) {
		LOG_ERR("rootreg_init: %d", rc);
		return rc;
	}
	rc = geometry(&db->st.partition_bytes);
	if (rc != 0) {
		return rc;
	}
	rc = rootreg_get_or_create(FLDB_ROOTREG_KEY, &db->sb_id);
	if (rc != 0) {
		LOG_ERR("rootreg: %d", rc);
		return rc;
	}

	rc = blob_db_get(db->sb_id, buf, sizeof(buf), &got);
	if (rc == -ENOENT || (rc == 0 && got == 0)) {
		/* Virgin, or a crash before a previous create's commit. */
		rc = create_store(db);
	} else if (rc == 0) {
		rc = fldb_sb_cbor_decode(buf, got, &db->sb);
		if (rc == 0 && db->sb.version < FLDB_SB_VERSION) {
			rc = upgrade_store(db);
		} else if (rc == -ENOTSUP) {
			LOG_ERR("flight store version %u, this build reads %u",
				db->sb.version, FLDB_SB_VERSION);
		} else if (rc != 0) {
			LOG_ERR("flight superblock unreadable: %d", rc);
			rc = -EIO;
		}
	}
	if (rc != 0) {
		return rc;
	}
	db->open = true;
	*out = db;
	return 0;
}

int flightdb_close(struct flightdb *db)
{
	if (db) {
		db->open = false;
	}
	blob_db_unmount();
	return 0;
}

/*
 * Destroy the maps first, then build new ones and commit. A crash in between
 * leaves a superblock naming destroyed maps, which the next erase destroys
 * again (-ENOENT, taken as done) — the order DESIGN.md §12.3 uses for drops.
 * blob_db_erase_all() would be one write instead, and would take anything
 * else on the flash with it.
 */
int flightdb_erase(struct flightdb *db)
{
	uint64_t roots[] = { db->sb.airports_root, db->sb.flights_root,
			     db->sb.passengers_root, db->sb.bookings_root,
			     db->sb.airlines_root, db->sb.aircraft_root,
			     db->sb.crew_root, db->sb.bags_root };

	for (int i = 0; i < (int)ARRAY_SIZE(roots); i++) {
		int rc = db->ops->destroy(roots[i]);

		if (rc != 0 && rc != -ENOENT) {
			LOG_ERR("destroying map: %d", rc);
			return rc;
		}
	}
	return create_store(db);
}

/* -- records ------------------------------------------------------------ */

int flightdb_airport_put(struct flightdb *db, uint32_t index,
			 const struct flightdb_airport *a)
{
	uint8_t buf[AIRPORT_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_AIRPORTS_MAX) {
		return -EINVAL;
	}
	rc = airport_cbor_encode(a, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.airports_root, 'a', index, buf, len);
}

int flightdb_flight_put(struct flightdb *db, uint32_t index,
			const struct flightdb_flight *f)
{
	uint8_t buf[FLIGHT_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_FLIGHTS_MAX) {
		return -EINVAL;
	}
	rc = flight_cbor_encode(f, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.flights_root, 'f', index, buf, len);
}

int flightdb_passenger_put(struct flightdb *db, uint32_t index,
			   const struct flightdb_passenger *p)
{
	uint8_t buf[PASSENGER_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_PASSENGERS_MAX) {
		return -EINVAL;
	}
	rc = passenger_cbor_encode(p, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.passengers_root, 'p', index, buf, len);
}

int flightdb_booking_put(struct flightdb *db, uint32_t index,
			 const struct flightdb_booking *b)
{
	uint8_t buf[BOOKING_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_BOOKINGS_MAX) {
		return -EINVAL;
	}
	rc = booking_cbor_encode(b, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.bookings_root, 'b', index, buf, len);
}

int flightdb_airline_put(struct flightdb *db, uint32_t index,
			 const struct flightdb_airline *a)
{
	uint8_t buf[AIRLINE_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_AIRLINES_MAX) {
		return -EINVAL;
	}
	rc = airline_cbor_encode(a, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.airlines_root, 'l', index, buf, len);
}

int flightdb_aircraft_put(struct flightdb *db, uint32_t index,
			  const struct flightdb_aircraft *a)
{
	uint8_t buf[AIRCRAFT_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_AIRCRAFT_MAX) {
		return -EINVAL;
	}
	rc = aircraft_cbor_encode(a, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.aircraft_root, 'k', index, buf, len);
}

int flightdb_crew_put(struct flightdb *db, uint32_t index,
		      const struct flightdb_crew *c)
{
	uint8_t buf[CREW_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_CREW_MAX) {
		return -EINVAL;
	}
	rc = crew_cbor_encode(c, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.crew_root, 'c', index, buf, len);
}

int flightdb_bag_put(struct flightdb *db, uint32_t index,
		     const struct flightdb_bag *b)
{
	uint8_t buf[BAG_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_BAGS_MAX) {
		return -EINVAL;
	}
	rc = bag_cbor_encode(b, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	return map_put(db, db->sb.bags_root, 'g', index, buf, len);
}

int flightdb_airport_get(struct flightdb *db, uint32_t index,
			 struct flightdb_airport *out)
{
	uint8_t buf[AIRPORT_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_airports) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.airports_root, 'a', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : airport_cbor_decode(buf, len, out);
}

int flightdb_flight_get(struct flightdb *db, uint32_t index,
			struct flightdb_flight *out)
{
	uint8_t buf[FLIGHT_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_flights) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.flights_root, 'f', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : flight_cbor_decode(buf, len, out);
}

int flightdb_passenger_get(struct flightdb *db, uint32_t index,
			   struct flightdb_passenger *out)
{
	uint8_t buf[PASSENGER_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_passengers) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.passengers_root, 'p', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : passenger_cbor_decode(buf, len, out);
}

int flightdb_booking_get(struct flightdb *db, uint32_t index,
			 struct flightdb_booking *out)
{
	uint8_t buf[BOOKING_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_bookings) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.bookings_root, 'b', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : booking_cbor_decode(buf, len, out);
}

int flightdb_airline_get(struct flightdb *db, uint32_t index,
			 struct flightdb_airline *out)
{
	uint8_t buf[AIRLINE_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_airlines) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.airlines_root, 'l', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : airline_cbor_decode(buf, len, out);
}

int flightdb_aircraft_get(struct flightdb *db, uint32_t index,
			  struct flightdb_aircraft *out)
{
	uint8_t buf[AIRCRAFT_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_aircraft) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.aircraft_root, 'k', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : aircraft_cbor_decode(buf, len, out);
}

int flightdb_crew_get(struct flightdb *db, uint32_t index,
		      struct flightdb_crew *out)
{
	uint8_t buf[CREW_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_crew) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.crew_root, 'c', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : crew_cbor_decode(buf, len, out);
}

int flightdb_bag_get(struct flightdb *db, uint32_t index,
		     struct flightdb_bag *out)
{
	uint8_t buf[BAG_CBOR_MAX];
	size_t len;
	int rc;

	if (index >= db->sb.n_bags) {
		return -ENOENT;
	}
	rc = map_read(db, db->sb.bags_root, 'g', index, buf, sizeof(buf), &len);
	return (rc != 0) ? rc : bag_cbor_decode(buf, len, out);
}

void flightdb_counts(struct flightdb *db, struct flightdb_counts *out)
{
	out->airports = db->sb.n_airports;
	out->flights = db->sb.n_flights;
	out->passengers = db->sb.n_passengers;
	out->bookings = db->sb.n_bookings;
	out->airlines = db->sb.n_airlines;
	out->aircraft = db->sb.n_aircraft;
	out->crew = db->sb.n_crew;
	out->bags = db->sb.n_bags;
}

int flightdb_counts_set(struct flightdb *db, const struct flightdb_counts *c)
{
	if (c->airports > FLIGHTDB_AIRPORTS_MAX || c->flights > FLIGHTDB_FLIGHTS_MAX ||
	    c->passengers > FLIGHTDB_PASSENGERS_MAX || c->bookings > FLIGHTDB_BOOKINGS_MAX ||
	    c->airlines > FLIGHTDB_AIRLINES_MAX || c->aircraft > FLIGHTDB_AIRCRAFT_MAX ||
	    c->crew > FLIGHTDB_CREW_MAX || c->bags > FLIGHTDB_BAGS_MAX) {
		return -EINVAL;
	}
	db->sb.n_airports = c->airports;
	db->sb.n_flights = c->flights;
	db->sb.n_passengers = c->passengers;
	db->sb.n_bookings = c->bookings;
	db->sb.n_airlines = c->airlines;
	db->sb.n_aircraft = c->aircraft;
	db->sb.n_crew = c->crew;
	db->sb.n_bags = c->bags;
	return sb_commit(db);
}

/* -- helpers ------------------------------------------------------------ */

bool flightdb_flight_equal(const struct flightdb_flight *a,
			   const struct flightdb_flight *b)
{
	uint8_t ba[FLIGHT_CBOR_MAX], bb[FLIGHT_CBOR_MAX];
	size_t la, lb;

	if (flight_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    flight_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

bool flightdb_airport_equal(const struct flightdb_airport *a,
			    const struct flightdb_airport *b)
{
	uint8_t ba[AIRPORT_CBOR_MAX], bb[AIRPORT_CBOR_MAX];
	size_t la, lb;

	if (airport_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    airport_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

bool flightdb_passenger_equal(const struct flightdb_passenger *a,
			      const struct flightdb_passenger *b)
{
	uint8_t ba[PASSENGER_CBOR_MAX], bb[PASSENGER_CBOR_MAX];
	size_t la, lb;

	if (passenger_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    passenger_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

bool flightdb_booking_equal(const struct flightdb_booking *a,
			    const struct flightdb_booking *b)
{
	uint8_t ba[BOOKING_CBOR_MAX], bb[BOOKING_CBOR_MAX];
	size_t la, lb;

	if (booking_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    booking_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

enum flightdb_status flightdb_status(const struct flightdb_flight *f, uint32_t now)
{
	int d = (int)(now % FLIGHTDB_MIN_PER_DAY) - (int)f->dep - (int)f->delay;

	if (f->cancelled) {
		return FLIGHTDB_CANCELLED;
	}
	if (d < -40) {
		return (f->delay > 0) ? FLIGHTDB_DELAYED : FLIGHTDB_SCHEDULED;
	}
	if (d < -10) {
		return FLIGHTDB_BOARDING;
	}
	if (d < 0) {
		return FLIGHTDB_FINAL_CALL;
	}
	return (d < (int)f->block) ? FLIGHTDB_DEPARTED : FLIGHTDB_LANDED;
}

const char *flightdb_status_name(enum flightdb_status s)
{
	switch (s) {
	case FLIGHTDB_SCHEDULED:  return "ON TIME";
	case FLIGHTDB_DELAYED:    return "DELAYED";
	case FLIGHTDB_BOARDING:   return "BOARDING";
	case FLIGHTDB_FINAL_CALL: return "FINAL CALL";
	case FLIGHTDB_DEPARTED:   return "DEPARTED";
	case FLIGHTDB_LANDED:     return "LANDED";
	default:                  return "CANCELLED";
	}
}

bool flightdb_airline_equal(const struct flightdb_airline *a,
			    const struct flightdb_airline *b)
{
	uint8_t ba[AIRLINE_CBOR_MAX], bb[AIRLINE_CBOR_MAX];
	size_t la, lb;

	if (airline_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    airline_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

bool flightdb_aircraft_equal(const struct flightdb_aircraft *a,
			     const struct flightdb_aircraft *b)
{
	uint8_t ba[AIRCRAFT_CBOR_MAX], bb[AIRCRAFT_CBOR_MAX];
	size_t la, lb;

	if (aircraft_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    aircraft_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

bool flightdb_crew_equal(const struct flightdb_crew *a,
			 const struct flightdb_crew *b)
{
	uint8_t ba[CREW_CBOR_MAX], bb[CREW_CBOR_MAX];
	size_t la, lb;

	if (crew_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    crew_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

bool flightdb_bag_equal(const struct flightdb_bag *a,
			const struct flightdb_bag *b)
{
	uint8_t ba[BAG_CBOR_MAX], bb[BAG_CBOR_MAX];
	size_t la, lb;

	if (bag_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    bag_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

const char *flightdb_tier_name(enum flightdb_tier t)
{
	switch (t) {
	case FLIGHTDB_TIER_BLUE:   return "BLUE";
	case FLIGHTDB_TIER_SILVER: return "SILVER";
	case FLIGHTDB_TIER_GOLD:   return "GOLD";
	default:                   return "";
	}
}

const char *flightdb_booking_status_name(enum flightdb_booking_status s)
{
	switch (s) {
	case FLIGHTDB_BK_CONFIRMED:  return "CONFIRMED";
	case FLIGHTDB_BK_WAITLISTED: return "WAITLISTED";
	default:                     return "CANCELLED";
	}
}

const char *flightdb_cabin_code(uint8_t cabin)
{
	static const char *const code[FLIGHTDB_CABINS] = { "F", "C", "Y" };

	return (cabin < FLIGHTDB_CABINS) ? code[cabin] : "?";
}

const char *flightdb_rank_code(enum flightdb_rank r)
{
	static const char *const code[FLIGHTDB_RANK__COUNT] = { "CPT", "FO", "PUR", "FA" };

	return (r < FLIGHTDB_RANK__COUNT) ? code[r] : "?";
}

const char *flightdb_rank_name(enum flightdb_rank r)
{
	static const char *const name[FLIGHTDB_RANK__COUNT] = {
		"CAPTAIN", "FIRST OFFICER", "PURSER", "FLIGHT ATTENDANT",
	};

	return (r < FLIGHTDB_RANK__COUNT) ? name[r] : "?";
}

const char *flightdb_bag_status_name(enum flightdb_bag_status s)
{
	switch (s) {
	case FLIGHTDB_BAG_CHECKED:   return "CHECKED";
	case FLIGHTDB_BAG_LOADED:    return "LOADED";
	case FLIGHTDB_BAG_OFFLOADED: return "OFFLOADED";
	default:                     return "MISHANDLED";
	}
}

int flightdb_stat(struct flightdb *db, struct flightdb_stat *out)
{
	struct map_info info;

	*out = db->st;
#ifdef CONFIG_BLOB_DB_IOSTATS
	{
		struct blob_db_iostats io;

		blob_db_iostats_get(&io);
		out->flash_reads = io.reads;
		out->flash_writes = io.writes;
		out->flash_erases = io.erases;
		out->io_measured = true;
	}
#endif
	out->airports = db->sb.n_airports;
	out->flights = db->sb.n_flights;
	out->passengers = db->sb.n_passengers;
	out->bookings = db->sb.n_bookings;
	out->airlines = db->sb.n_airlines;
	out->aircraft = db->sb.n_aircraft;
	out->crew = db->sb.n_crew;
	out->bags = db->sb.n_bags;
	if (db->ops->stat && db->ops->stat(db->sb.airports_root, &info) == 0) {
		out->airport_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.flights_root, &info) == 0) {
		out->flight_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.passengers_root, &info) == 0) {
		out->passenger_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.bookings_root, &info) == 0) {
		out->booking_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.airlines_root, &info) == 0) {
		out->airline_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.aircraft_root, &info) == 0) {
		out->aircraft_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.crew_root, &info) == 0) {
		out->crew_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.bags_root, &info) == 0) {
		out->bag_buckets = info.buckets;
	}
	return 0;
}
