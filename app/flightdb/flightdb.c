/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flightdb — the flight information API, on L2 map_ops + rootreg + blob_db.
 *
 * Layout. One registry key of its own, one superblock, two kvhash instances:
 *
 *   rootreg[ ROOTREG_KEY('FLDB', 0) ] -> superblock -> airports map
 *                                                   -> flights map
 *
 * The person database sits beside it under ('PADB', 0) on the same blob_db,
 * so both survive flashing either firmware. Keys are positions, fixed width:
 * "aNNNN" for airports, "fNNNN" for flights.
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

/* One store per image, as in persondb.c. */
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
		 * two walls persondb meets (FINDINGS.md K2, K13). */
		db->st.enospc_hits++;
		LOG_ERR("map full on key '%s'", k);
	}
	return rc;
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

/*
 * Allocate and create both maps, then bind the superblock last: that one
 * write publishes the store. A crash before it leaks the two map roots until
 * a format, as for persondb (FINDINGS.md B8).
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

	db->sb.airports_root = blob_db_alloc_id();
	if (db->sb.airports_root == 0) {
		return -EIO;
	}
	rc = db->ops->create(db->sb.airports_root, &airports_cfg);
	if (rc != 0) {
		return rc;
	}
	db->sb.flights_root = blob_db_alloc_id();
	if (db->sb.flights_root == 0) {
		return -EIO;
	}
	rc = db->ops->create(db->sb.flights_root, &flights_cfg);
	if (rc != 0) {
		return rc;
	}
	LOG_INF("created flight store");
	return sb_commit(db);   /* the commit point */
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
	/* Once per boot, as in persondb.c. Whichever store opens second
	 * erases the first one's new, empty store; the first one recreates it
	 * on its next open, so both still start over. */
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
		if (rc == -ENOTSUP) {
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
 * blob_db_erase_all() would be one write instead, and would take the person
 * database with it.
 */
int flightdb_erase(struct flightdb *db)
{
	uint64_t roots[2] = { db->sb.airports_root, db->sb.flights_root };

	for (int i = 0; i < 2; i++) {
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
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_AIRPORTS_MAX) {
		return -EINVAL;
	}
	rc = airport_cbor_encode(a, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	key(k, 'a', index);
	return map_set(db, db->sb.airports_root, k, buf, len);
}

int flightdb_flight_put(struct flightdb *db, uint32_t index,
			const struct flightdb_flight *f)
{
	uint8_t buf[FLIGHT_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= FLIGHTDB_FLIGHTS_MAX) {
		return -EINVAL;
	}
	rc = flight_cbor_encode(f, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	key(k, 'f', index);
	return map_set(db, db->sb.flights_root, k, buf, len);
}

int flightdb_airport_get(struct flightdb *db, uint32_t index,
			 struct flightdb_airport *out)
{
	uint8_t buf[AIRPORT_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= db->sb.n_airports) {
		return -ENOENT;
	}
	key(k, 'a', index);
	rc = map_get(db, db->sb.airports_root, k, buf, sizeof(buf), &len);
	if (rc != 0) {
		return (rc == -ENOMEM) ? -EILSEQ : rc;
	}
	return airport_cbor_decode(buf, len, out);
}

int flightdb_flight_get(struct flightdb *db, uint32_t index,
			struct flightdb_flight *out)
{
	uint8_t buf[FLIGHT_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= db->sb.n_flights) {
		return -ENOENT;
	}
	key(k, 'f', index);
	rc = map_get(db, db->sb.flights_root, k, buf, sizeof(buf), &len);
	if (rc != 0) {
		return (rc == -ENOMEM) ? -EILSEQ : rc;
	}
	return flight_cbor_decode(buf, len, out);
}

void flightdb_counts(struct flightdb *db, uint32_t *airports, uint32_t *flights)
{
	if (airports) {
		*airports = db->sb.n_airports;
	}
	if (flights) {
		*flights = db->sb.n_flights;
	}
}

int flightdb_counts_set(struct flightdb *db, uint32_t airports, uint32_t flights)
{
	if (airports > FLIGHTDB_AIRPORTS_MAX || flights > FLIGHTDB_FLIGHTS_MAX) {
		return -EINVAL;
	}
	db->sb.n_airports = airports;
	db->sb.n_flights = flights;
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
	if (db->ops->stat && db->ops->stat(db->sb.airports_root, &info) == 0) {
		out->airport_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.flights_root, &info) == 0) {
		out->flight_buckets = info.buckets;
	}
	return 0;
}
