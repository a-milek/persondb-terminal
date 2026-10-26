/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * library_cbor — the library store's wire format, private to librarydb.
 *
 * Canonical CBOR maps with small integer keys, as in flight_cbor.h:
 *
 *   book       { 1: isbn, 2: title, 3: author, 4: subject, 5: call number,
 *                6: year, 7: pages, 8: [loan per copy] }
 *   loan       { 1: book, 2: copy, 3: card, 4: borrower, 5: out, 6: due,
 *                7: renewals, 8: returned, 9: returned on }
 *   superblock { 1: version, 2: books root, 3: loans root, 4: books held,
 *                5: loans held, 6: loans out }
 *
 * A book's copy count is the length of its loan list.
 */

#ifndef APP_CBOR_PERSONDB_LIBRARY_CBOR_H_
#define APP_CBOR_PERSONDB_LIBRARY_CBOR_H_

#include <stddef.h>
#include <stdint.h>

#include "librarydb.h"

/* Every field at its capacity, with room to spare. */
#define BOOK_CBOR_MAX    192
#define LOAN_CBOR_MAX    96
#define LBDB_SB_CBOR_MAX 64

/* Bump on any change to an encoding below. */
#define LBDB_SB_VERSION 1

struct lbdb_superblock {
	uint32_t version;
	uint64_t books_root;
	uint64_t loans_root;
	uint32_t n_books;
	uint32_t n_loans;
	uint32_t n_active;
};

/** @retval -ENOMEM buf too small  @retval -EINVAL a field out of bounds */
int book_cbor_encode(const struct librarydb_book *b, uint8_t *buf,
		     size_t buf_sz, size_t *out_len);
/** @retval -EILSEQ not a well-formed book */
int book_cbor_decode(const uint8_t *buf, size_t len, struct librarydb_book *out);

int loan_cbor_encode(const struct librarydb_loan *l, uint8_t *buf,
		     size_t buf_sz, size_t *out_len);
int loan_cbor_decode(const uint8_t *buf, size_t len, struct librarydb_loan *out);

int lbdb_sb_cbor_encode(const struct lbdb_superblock *sb, uint8_t *buf,
			size_t buf_sz, size_t *out_len);
/** @retval -ENOTSUP another version (out->version holds it) */
int lbdb_sb_cbor_decode(const uint8_t *buf, size_t len,
			struct lbdb_superblock *out);

#endif /* APP_CBOR_PERSONDB_LIBRARY_CBOR_H_ */
