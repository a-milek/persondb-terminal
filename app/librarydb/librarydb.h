/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * librarydb — the library catalogue API: books and the loans of their copies,
 * stored on the same stack as the person and flight databases but as a
 * separate store with its own registry entry. None of the stores can see the
 * others' records, and erasing one leaves the others intact.
 *
 * Domain vocabulary only, as in flightdb.h: no keys, no blob ids, no CBOR.
 * Records are addressed by position — book 0..n-1, loan 0..n-1 in the order
 * they were made — and the store keeps how many of each it holds.
 *
 * A loan is never deleted: returning a copy marks its loan returned, so the
 * loan positions a book names stay valid. Each book names the active loan of
 * each of its copies, so the book alone says what is on the shelf.
 *
 * Concurrency: single-threaded, like persondb (FINDINGS.md K7).
 */

#ifndef APP_CBOR_PERSONDB_LIBRARYDB_H_
#define APP_CBOR_PERSONDB_LIBRARYDB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Field capacities: fixed, so a record is a plain value type. */
#define LIBRARYDB_ISBN_LEN    13
#define LIBRARYDB_TITLE_MAX   31
#define LIBRARYDB_AUTHOR_MAX  23   /* "KOVAC, E." */
#define LIBRARYDB_SUBJECT_MAX 15
#define LIBRARYDB_CALL_MAX    11   /* "823.91 KOV" */
#define LIBRARYDB_CARD_MAX    6    /* "B10423" */
#define LIBRARYDB_NAME_MAX    23
#define LIBRARYDB_COPIES_MAX  4

/* Store capacity, declared to the maps when the store is created. */
#define LIBRARYDB_BOOKS_MAX   2048
#define LIBRARYDB_LOANS_MAX   4096

/* A copy on the shelf. */
#define LIBRARYDB_NO_LOAN     0xFFFFu

/* Loan period and renewals, in days. */
#define LIBRARYDB_LOAN_DAYS   21u
#define LIBRARYDB_RENEWALS_MAX 3u

struct librarydb_book {
	char     isbn[LIBRARYDB_ISBN_LEN + 1];
	char     title[LIBRARYDB_TITLE_MAX + 1];
	char     author[LIBRARYDB_AUTHOR_MAX + 1];
	char     subject[LIBRARYDB_SUBJECT_MAX + 1];
	char     call[LIBRARYDB_CALL_MAX + 1];
	uint16_t year;
	uint16_t pages;
	uint8_t  copies;
	uint16_t loan[LIBRARYDB_COPIES_MAX];   /* per copy: active loan, or NO_LOAN */
};

/* Dates are days since 1970-01-01. */
struct librarydb_loan {
	uint16_t book;
	uint8_t  copy;           /* 0..copies-1 */
	char     card[LIBRARYDB_CARD_MAX + 1];
	char     borrower[LIBRARYDB_NAME_MAX + 1];
	uint16_t out;
	uint16_t due;
	uint8_t  renewals;
	bool     returned;
	uint16_t returned_on;    /* 0 while out */
};

/** Where a loan is on a given day. */
enum librarydb_status {
	LIBRARYDB_ON_LOAN,
	LIBRARYDB_DUE_SOON,      /* due within three days */
	LIBRARYDB_DUE_TODAY,
	LIBRARYDB_OVERDUE,
	LIBRARYDB_RETURNED,
};

struct librarydb_stat {
	uint32_t books;              /* records held */
	uint32_t loans;
	uint32_t active;             /* loans not returned */
	uint32_t book_buckets;       /* geometry the maps chose, read back */
	uint32_t loan_buckets;
	size_t   partition_bytes;
	uint32_t enospc_hits;
	uint64_t map_gets;
	uint64_t map_sets;
	uint64_t flash_reads;        /* zero unless CONFIG_BLOB_DB_IOSTATS */
	uint64_t flash_writes;
	uint64_t flash_erases;
	bool     io_measured;
};

struct librarydb;

/* -- lifecycle ---------------------------------------------------------- */

/**
 * @brief Mount the stack and attach to the library store, creating an empty
 *        one on first use.
 *
 * @retval 0        ready
 * @retval -ENOTSUP the store was written by a build with another format
 * @retval -EIO     flash error or a corrupt superblock
 */
int librarydb_open(struct librarydb **db);

/** Detach and unmount. The store's contents are untouched. */
int librarydb_close(struct librarydb *db);

/**
 * @brief Drop every book and loan and start an empty store.
 *
 * Destroys this store's maps only: the other databases on the flash are left
 * as they were.
 */
int librarydb_erase(struct librarydb *db);

/* -- records ------------------------------------------------------------ */

/**
 * @brief Write the record at @p index. Writing at or past the count does not
 *        make it visible: librarydb_counts_set() does, once a batch is down.
 *
 * @retval -EINVAL index past capacity, or a field violating its bounds
 * @retval -ENOSPC a map overflowed (counted in the stat block)
 */
int librarydb_book_put(struct librarydb *db, uint32_t index,
		       const struct librarydb_book *b);
int librarydb_loan_put(struct librarydb *db, uint32_t index,
		       const struct librarydb_loan *l);

/**
 * @retval 0       found
 * @retval -ENOENT @p index is at or past the count
 * @retval -EILSEQ the stored record is corrupt
 */
int librarydb_book_get(struct librarydb *db, uint32_t index,
		       struct librarydb_book *out);
int librarydb_loan_get(struct librarydb *db, uint32_t index,
		       struct librarydb_loan *out);

/** How many records the store holds, and how many loans are still out. */
void librarydb_counts(struct librarydb *db, uint32_t *books, uint32_t *loans,
		      uint32_t *active);

/** Publish the counts: one superblock write. */
int librarydb_counts_set(struct librarydb *db, uint32_t books, uint32_t loans,
			 uint32_t active);

/* -- circulation -------------------------------------------------------- */

/**
 * @brief Lend copy @p copy of book @p book from day @p today: a new loan,
 *        then the book naming it, then the counts.
 *
 * @param index  (out, optional) the new loan's position
 * @retval -EBUSY  the copy is already out
 * @retval -ENOSPC the loan store is full
 */
int librarydb_lend(struct librarydb *db, uint32_t book, uint8_t copy,
		   const char *card, const char *borrower, uint16_t today,
		   uint32_t *index);

/** @brief Mark the copy's loan returned on @p today and put it on the shelf.
 *  @retval -ENOENT the copy is not out */
int librarydb_return(struct librarydb *db, uint32_t book, uint8_t copy,
		     uint16_t today);

/** @brief Extend the copy's loan by a loan period from @p today.
 *  @retval -ENOENT the copy is not out  @retval -EDQUOT renewed too often */
int librarydb_renew(struct librarydb *db, uint32_t book, uint8_t copy,
		    uint16_t today);

/* -- helpers ------------------------------------------------------------ */

/** True if both records encode identically, i.e. are the same stored record. */
bool librarydb_book_equal(const struct librarydb_book *a,
			  const struct librarydb_book *b);

/** Copies of @p b on the shelf. */
int librarydb_available(const struct librarydb_book *b);

enum librarydb_status librarydb_status(const struct librarydb_loan *l, uint16_t today);
const char *librarydb_status_name(enum librarydb_status s);

int librarydb_stat(struct librarydb *db, struct librarydb_stat *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_LIBRARYDB_H_ */
