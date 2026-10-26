/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * librarydb — the library catalogue API, on L2 map_ops + rootreg + blob_db.
 *
 * Layout. One registry key of its own, one superblock, two kvhash instances:
 *
 *   rootreg[ ROOTREG_KEY('LBDB', 0) ] -> superblock -> books map
 *                                                   -> loans map
 *
 * The person and flight databases sit beside it under ('PADB', 0) and
 * ('FLDB', 0) on the same blob_db. Keys are positions, fixed width: "bNNNN"
 * for books, "lNNNN" for loans.
 *
 * This is the only file of the library database that names a key, a blob id
 * or a map operation.
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

#include "library_cbor.h"
#include "librarydb.h"

LOG_MODULE_REGISTER(librarydb, CONFIG_APP_CBOR_PERSONDB_LOG_LEVEL);

#define LBDB_MAGIC       0x4c424442u /* 'LBDB' */
#define LBDB_ROOTREG_KEY ROOTREG_KEY(LBDB_MAGIC, 0)

#define KEY_SZ 6

struct librarydb {
	bool                   open;
	uint64_t               sb_id;
	struct lbdb_superblock sb;
	const struct map_ops  *ops;
	struct librarydb_stat  st;
};

/* One store per image, as in persondb.c. */
static struct librarydb g_db;

/* CONFIG_APP_CBOR_PERSONDB_FRESH_START has run this boot. */
static bool g_erased;

/* -- plumbing ----------------------------------------------------------- */

/* Callers bound @p index by LIBRARYDB_*_MAX, so it fits four hex digits; the
 * mask tells the compiler so. */
static void key(char *buf, char kind, uint32_t index)
{
	snprintf(buf, KEY_SZ, "%c%04X", kind, (unsigned)(index & 0xFFFFu));
}

static int map_get(struct librarydb *db, uint64_t root, const char *k, void *out,
		   size_t out_sz, size_t *out_len)
{
	db->st.map_gets++;
	return db->ops->get(root, k, strlen(k), out, out_sz, out_len);
}

static int map_set(struct librarydb *db, uint64_t root, const char *k,
		   const void *val, size_t len)
{
	int rc;

	db->st.map_sets++;
	rc = db->ops->set(root, k, strlen(k), val, len);
	if (rc == -ENOSPC) {
		/* The same two walls persondb meets (FINDINGS.md K2, K13). */
		db->st.enospc_hits++;
		LOG_ERR("map full on key '%s'", k);
	}
	return rc;
}

static int sb_commit(struct librarydb *db)
{
	uint8_t buf[LBDB_SB_CBOR_MAX];
	size_t len;
	int rc = lbdb_sb_cbor_encode(&db->sb, buf, sizeof(buf), &len);

	if (rc != 0) {
		return rc;
	}
	return blob_db_update(db->sb_id, buf, len);
}

/* -- lifecycle ---------------------------------------------------------- */

/*
 * Allocate and create both maps, then bind the superblock last: that one
 * write publishes the store, as in flightdb.c.
 */
static int create_store(struct librarydb *db)
{
	/* Measured with the shipped data: books encode to at most 104 B, loans
	 * to 52 B, each under a 5-byte key. */
	const struct map_config books_cfg = {
		.expected_entries = LIBRARYDB_BOOKS_MAX,
		.typical_entry_bytes = 100,
		.max_entry_bytes = KEY_SZ + BOOK_CBOR_MAX,
	};
	const struct map_config loans_cfg = {
		.expected_entries = LIBRARYDB_LOANS_MAX,
		.typical_entry_bytes = 56,
		.max_entry_bytes = KEY_SZ + LOAN_CBOR_MAX,
	};
	int rc;

	memset(&db->sb, 0, sizeof(db->sb));
	db->sb.version = LBDB_SB_VERSION;

	db->sb.books_root = blob_db_alloc_id();
	if (db->sb.books_root == 0) {
		return -EIO;
	}
	rc = db->ops->create(db->sb.books_root, &books_cfg);
	if (rc != 0) {
		return rc;
	}
	db->sb.loans_root = blob_db_alloc_id();
	if (db->sb.loans_root == 0) {
		return -EIO;
	}
	rc = db->ops->create(db->sb.loans_root, &loans_cfg);
	if (rc != 0) {
		return rc;
	}
	LOG_INF("created library store");
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

int librarydb_open(struct librarydb **out)
{
	struct librarydb *db = &g_db;
	uint8_t buf[LBDB_SB_CBOR_MAX];
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
	/* Once per boot, as in flightdb.c. */
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
	rc = rootreg_get_or_create(LBDB_ROOTREG_KEY, &db->sb_id);
	if (rc != 0) {
		LOG_ERR("rootreg: %d", rc);
		return rc;
	}

	rc = blob_db_get(db->sb_id, buf, sizeof(buf), &got);
	if (rc == -ENOENT || (rc == 0 && got == 0)) {
		/* Virgin, or a crash before a previous create's commit. */
		rc = create_store(db);
	} else if (rc == 0) {
		rc = lbdb_sb_cbor_decode(buf, got, &db->sb);
		if (rc == -ENOTSUP) {
			LOG_ERR("library store version %u, this build reads %u",
				db->sb.version, LBDB_SB_VERSION);
		} else if (rc != 0) {
			LOG_ERR("library superblock unreadable: %d", rc);
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

int librarydb_close(struct librarydb *db)
{
	if (db) {
		db->open = false;
	}
	blob_db_unmount();
	return 0;
}

/* Destroy the maps first, then build new ones and commit, as flightdb_erase(). */
int librarydb_erase(struct librarydb *db)
{
	uint64_t roots[2] = { db->sb.books_root, db->sb.loans_root };

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

int librarydb_book_put(struct librarydb *db, uint32_t index,
		       const struct librarydb_book *b)
{
	uint8_t buf[BOOK_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= LIBRARYDB_BOOKS_MAX) {
		return -EINVAL;
	}
	rc = book_cbor_encode(b, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	key(k, 'b', index);
	return map_set(db, db->sb.books_root, k, buf, len);
}

int librarydb_loan_put(struct librarydb *db, uint32_t index,
		       const struct librarydb_loan *l)
{
	uint8_t buf[LOAN_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= LIBRARYDB_LOANS_MAX) {
		return -EINVAL;
	}
	rc = loan_cbor_encode(l, buf, sizeof(buf), &len);
	if (rc != 0) {
		return rc;
	}
	key(k, 'l', index);
	return map_set(db, db->sb.loans_root, k, buf, len);
}

int librarydb_book_get(struct librarydb *db, uint32_t index,
		       struct librarydb_book *out)
{
	uint8_t buf[BOOK_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= db->sb.n_books) {
		return -ENOENT;
	}
	key(k, 'b', index);
	rc = map_get(db, db->sb.books_root, k, buf, sizeof(buf), &len);
	if (rc != 0) {
		return (rc == -ENOMEM) ? -EILSEQ : rc;
	}
	return book_cbor_decode(buf, len, out);
}

int librarydb_loan_get(struct librarydb *db, uint32_t index,
		       struct librarydb_loan *out)
{
	uint8_t buf[LOAN_CBOR_MAX];
	char k[KEY_SZ];
	size_t len;
	int rc;

	if (index >= db->sb.n_loans) {
		return -ENOENT;
	}
	key(k, 'l', index);
	rc = map_get(db, db->sb.loans_root, k, buf, sizeof(buf), &len);
	if (rc != 0) {
		return (rc == -ENOMEM) ? -EILSEQ : rc;
	}
	return loan_cbor_decode(buf, len, out);
}

void librarydb_counts(struct librarydb *db, uint32_t *books, uint32_t *loans,
		      uint32_t *active)
{
	if (books) {
		*books = db->sb.n_books;
	}
	if (loans) {
		*loans = db->sb.n_loans;
	}
	if (active) {
		*active = db->sb.n_active;
	}
}

int librarydb_counts_set(struct librarydb *db, uint32_t books, uint32_t loans,
			 uint32_t active)
{
	if (books > LIBRARYDB_BOOKS_MAX || loans > LIBRARYDB_LOANS_MAX || active > loans) {
		return -EINVAL;
	}
	db->sb.n_books = books;
	db->sb.n_loans = loans;
	db->sb.n_active = active;
	return sb_commit(db);
}

/* -- circulation -------------------------------------------------------- */

/*
 * Each change is up to three writes: the loan, the book, the counts. They go
 * in that order, so a crash part-way leaves at worst a loan no book names
 * (lend) or a book naming a returned loan (return) — both of which verify
 * reports, and neither of which loses a book.
 */

static int copy_loan(struct librarydb *db, uint32_t book, uint8_t copy,
		     struct librarydb_book *b, struct librarydb_loan *l)
{
	int rc = librarydb_book_get(db, book, b);

	if (rc != 0) {
		return rc;
	}
	if (copy >= b->copies || b->loan[copy] == LIBRARYDB_NO_LOAN) {
		return -ENOENT;
	}
	return librarydb_loan_get(db, b->loan[copy], l);
}

int librarydb_lend(struct librarydb *db, uint32_t book, uint8_t copy,
		   const char *card, const char *borrower, uint16_t today,
		   uint32_t *index)
{
	struct librarydb_book b;
	struct librarydb_loan l = { 0 };
	uint32_t n = db->sb.n_loans;
	int rc = librarydb_book_get(db, book, &b);

	if (rc != 0) {
		return rc;
	}
	if (copy >= b.copies) {
		return -EINVAL;
	}
	if (b.loan[copy] != LIBRARYDB_NO_LOAN) {
		return -EBUSY;
	}
	if (n >= LIBRARYDB_LOANS_MAX) {
		return -ENOSPC;
	}
	l.book = (uint16_t)book;
	l.copy = copy;
	strncpy(l.card, card, LIBRARYDB_CARD_MAX);
	strncpy(l.borrower, borrower, LIBRARYDB_NAME_MAX);
	l.out = today;
	l.due = (uint16_t)(today + LIBRARYDB_LOAN_DAYS);

	rc = librarydb_loan_put(db, n, &l);
	if (rc != 0) {
		return rc;
	}
	b.loan[copy] = (uint16_t)n;
	rc = librarydb_book_put(db, book, &b);
	if (rc != 0) {
		return rc;
	}
	if (index) {
		*index = n;
	}
	return librarydb_counts_set(db, db->sb.n_books, n + 1, db->sb.n_active + 1);
}

int librarydb_return(struct librarydb *db, uint32_t book, uint8_t copy,
		     uint16_t today)
{
	struct librarydb_book b;
	struct librarydb_loan l;
	uint16_t at;
	int rc = copy_loan(db, book, copy, &b, &l);

	if (rc != 0) {
		return rc;
	}
	at = b.loan[copy];
	l.returned = true;
	l.returned_on = today;
	rc = librarydb_loan_put(db, at, &l);
	if (rc != 0) {
		return rc;
	}
	b.loan[copy] = LIBRARYDB_NO_LOAN;
	rc = librarydb_book_put(db, book, &b);
	if (rc != 0) {
		return rc;
	}
	return librarydb_counts_set(db, db->sb.n_books, db->sb.n_loans,
				    db->sb.n_active > 0 ? db->sb.n_active - 1 : 0);
}

int librarydb_renew(struct librarydb *db, uint32_t book, uint8_t copy,
		    uint16_t today)
{
	struct librarydb_book b;
	struct librarydb_loan l;
	int rc = copy_loan(db, book, copy, &b, &l);

	if (rc != 0) {
		return rc;
	}
	if (l.renewals >= LIBRARYDB_RENEWALS_MAX) {
		return -EDQUOT;
	}
	l.renewals++;
	l.due = (uint16_t)(MAX(today, l.due) + LIBRARYDB_LOAN_DAYS);
	return librarydb_loan_put(db, b.loan[copy], &l);
}

/* -- helpers ------------------------------------------------------------ */

bool librarydb_book_equal(const struct librarydb_book *a,
			  const struct librarydb_book *b)
{
	uint8_t ba[BOOK_CBOR_MAX], bb[BOOK_CBOR_MAX];
	size_t la, lb;

	if (book_cbor_encode(a, ba, sizeof(ba), &la) != 0 ||
	    book_cbor_encode(b, bb, sizeof(bb), &lb) != 0) {
		return false;
	}
	return la == lb && memcmp(ba, bb, la) == 0;
}

int librarydb_available(const struct librarydb_book *b)
{
	int n = 0;

	for (int i = 0; i < b->copies; i++) {
		n += (b->loan[i] == LIBRARYDB_NO_LOAN) ? 1 : 0;
	}
	return n;
}

enum librarydb_status librarydb_status(const struct librarydb_loan *l, uint16_t today)
{
	if (l->returned) {
		return LIBRARYDB_RETURNED;
	}
	if (today > l->due) {
		return LIBRARYDB_OVERDUE;
	}
	if (today == l->due) {
		return LIBRARYDB_DUE_TODAY;
	}
	return (l->due - today <= 3) ? LIBRARYDB_DUE_SOON : LIBRARYDB_ON_LOAN;
}

const char *librarydb_status_name(enum librarydb_status s)
{
	switch (s) {
	case LIBRARYDB_ON_LOAN:   return "ON LOAN";
	case LIBRARYDB_DUE_SOON:  return "DUE SOON";
	case LIBRARYDB_DUE_TODAY: return "DUE TODAY";
	case LIBRARYDB_OVERDUE:   return "OVERDUE";
	default:                  return "RETURNED";
	}
}

int librarydb_stat(struct librarydb *db, struct librarydb_stat *out)
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
	out->books = db->sb.n_books;
	out->loans = db->sb.n_loans;
	out->active = db->sb.n_active;
	if (db->ops->stat && db->ops->stat(db->sb.books_root, &info) == 0) {
		out->book_buckets = info.buckets;
	}
	if (db->ops->stat && db->ops->stat(db->sb.loans_root, &info) == 0) {
		out->loan_buckets = info.buckets;
	}
	return 0;
}
