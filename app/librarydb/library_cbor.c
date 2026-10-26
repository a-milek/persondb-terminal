/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * library_cbor — zcbor encode/decode for the library store. See library_cbor.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include "library_cbor.h"

#define NBACKUPS 2

/* Map keys. Never renumber: the store outlives the build that wrote it. */
enum {
	B_ISBN = 1, B_TITLE, B_AUTHOR, B_SUBJECT, B_CALL, B_YEAR, B_PAGES, B_LOANS,
};
#define BOOK_PAIRS 8

enum {
	L_BOOK = 1, L_COPY, L_CARD, L_BORROWER, L_OUT, L_DUE, L_RENEWALS,
	L_RETURNED, L_RETURNED_ON,
};
#define LOAN_PAIRS 9

enum {
	SB_VERSION = 1, SB_BOOKS_ROOT, SB_LOANS_ROOT, SB_N_BOOKS, SB_N_LOANS,
	SB_N_ACTIVE,
};
#define SB_PAIRS 6

/* Copy a decoded text slice into a fixed field; too long is corrupt, not
 * truncated, so a bad record surfaces here rather than as a wrong screen. */
static bool get_tstr(zcbor_state_t *zs, uint32_t key, char *dst, size_t cap)
{
	struct zcbor_string s;

	if (!zcbor_uint32_expect(zs, key) || !zcbor_tstr_decode(zs, &s) ||
	    s.len >= cap) {
		return false;
	}
	memcpy(dst, s.value, s.len);
	dst[s.len] = '\0';
	return true;
}

static bool get_u16(zcbor_state_t *zs, uint32_t key, uint16_t *out)
{
	uint32_t v;

	if (!zcbor_uint32_expect(zs, key) || !zcbor_uint32_decode(zs, &v) ||
	    v > UINT16_MAX) {
		return false;
	}
	*out = (uint16_t)v;
	return true;
}

static bool get_u8(zcbor_state_t *zs, uint32_t key, uint8_t *out)
{
	uint32_t v;

	if (!zcbor_uint32_expect(zs, key) || !zcbor_uint32_decode(zs, &v) ||
	    v > UINT8_MAX) {
		return false;
	}
	*out = (uint8_t)v;
	return true;
}

static bool put_loans(zcbor_state_t *zs, const struct librarydb_book *b)
{
	bool ok = zcbor_uint32_put(zs, B_LOANS) &&
		  zcbor_list_start_encode(zs, LIBRARYDB_COPIES_MAX);

	for (int i = 0; ok && i < b->copies; i++) {
		ok = zcbor_uint32_put(zs, b->loan[i]);
	}
	return ok && zcbor_list_end_encode(zs, LIBRARYDB_COPIES_MAX);
}

static bool get_loans(zcbor_state_t *zs, struct librarydb_book *b)
{
	uint32_t x;
	int n = 0;

	if (!zcbor_uint32_expect(zs, B_LOANS) || !zcbor_list_start_decode(zs)) {
		return false;
	}
	while (!zcbor_array_at_end(zs)) {
		if (n == LIBRARYDB_COPIES_MAX || !zcbor_uint32_decode(zs, &x) ||
		    x > UINT16_MAX) {
			return false;
		}
		b->loan[n++] = (uint16_t)x;
	}
	b->copies = (uint8_t)n;
	for (int i = n; i < LIBRARYDB_COPIES_MAX; i++) {
		b->loan[i] = LIBRARYDB_NO_LOAN;
	}
	return n > 0 && zcbor_list_end_decode(zs);
}

static bool fits(const char *s, size_t max)
{
	return strnlen(s, max + 1) <= max;
}

int book_cbor_encode(const struct librarydb_book *b, uint8_t *buf,
		     size_t buf_sz, size_t *out_len)
{
	if (!fits(b->isbn, LIBRARYDB_ISBN_LEN) || !fits(b->title, LIBRARYDB_TITLE_MAX) ||
	    !fits(b->author, LIBRARYDB_AUTHOR_MAX) ||
	    !fits(b->subject, LIBRARYDB_SUBJECT_MAX) ||
	    !fits(b->call, LIBRARYDB_CALL_MAX) || b->copies == 0 ||
	    b->copies > LIBRARYDB_COPIES_MAX) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, BOOK_PAIRS) &&
		  zcbor_uint32_put(zs, B_ISBN) &&
		  zcbor_tstr_put_term(zs, b->isbn, LIBRARYDB_ISBN_LEN) &&
		  zcbor_uint32_put(zs, B_TITLE) &&
		  zcbor_tstr_put_term(zs, b->title, LIBRARYDB_TITLE_MAX) &&
		  zcbor_uint32_put(zs, B_AUTHOR) &&
		  zcbor_tstr_put_term(zs, b->author, LIBRARYDB_AUTHOR_MAX) &&
		  zcbor_uint32_put(zs, B_SUBJECT) &&
		  zcbor_tstr_put_term(zs, b->subject, LIBRARYDB_SUBJECT_MAX) &&
		  zcbor_uint32_put(zs, B_CALL) &&
		  zcbor_tstr_put_term(zs, b->call, LIBRARYDB_CALL_MAX) &&
		  zcbor_uint32_put(zs, B_YEAR) && zcbor_uint32_put(zs, b->year) &&
		  zcbor_uint32_put(zs, B_PAGES) && zcbor_uint32_put(zs, b->pages) &&
		  put_loans(zs, b) && zcbor_map_end_encode(zs, BOOK_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int book_cbor_decode(const uint8_t *buf, size_t len, struct librarydb_book *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, B_ISBN, out->isbn, sizeof(out->isbn)) &&
		  get_tstr(zs, B_TITLE, out->title, sizeof(out->title)) &&
		  get_tstr(zs, B_AUTHOR, out->author, sizeof(out->author)) &&
		  get_tstr(zs, B_SUBJECT, out->subject, sizeof(out->subject)) &&
		  get_tstr(zs, B_CALL, out->call, sizeof(out->call)) &&
		  get_u16(zs, B_YEAR, &out->year) && get_u16(zs, B_PAGES, &out->pages) &&
		  get_loans(zs, out) && zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int loan_cbor_encode(const struct librarydb_loan *l, uint8_t *buf,
		     size_t buf_sz, size_t *out_len)
{
	if (!fits(l->card, LIBRARYDB_CARD_MAX) || !fits(l->borrower, LIBRARYDB_NAME_MAX) ||
	    l->copy >= LIBRARYDB_COPIES_MAX) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, LOAN_PAIRS) &&
		  zcbor_uint32_put(zs, L_BOOK) && zcbor_uint32_put(zs, l->book) &&
		  zcbor_uint32_put(zs, L_COPY) && zcbor_uint32_put(zs, l->copy) &&
		  zcbor_uint32_put(zs, L_CARD) &&
		  zcbor_tstr_put_term(zs, l->card, LIBRARYDB_CARD_MAX) &&
		  zcbor_uint32_put(zs, L_BORROWER) &&
		  zcbor_tstr_put_term(zs, l->borrower, LIBRARYDB_NAME_MAX) &&
		  zcbor_uint32_put(zs, L_OUT) && zcbor_uint32_put(zs, l->out) &&
		  zcbor_uint32_put(zs, L_DUE) && zcbor_uint32_put(zs, l->due) &&
		  zcbor_uint32_put(zs, L_RENEWALS) && zcbor_uint32_put(zs, l->renewals) &&
		  zcbor_uint32_put(zs, L_RETURNED) && zcbor_bool_put(zs, l->returned) &&
		  zcbor_uint32_put(zs, L_RETURNED_ON) &&
		  zcbor_uint32_put(zs, l->returned_on) &&
		  zcbor_map_end_encode(zs, LOAN_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int loan_cbor_decode(const uint8_t *buf, size_t len, struct librarydb_loan *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_u16(zs, L_BOOK, &out->book) && get_u8(zs, L_COPY, &out->copy) &&
		  out->copy < LIBRARYDB_COPIES_MAX &&
		  get_tstr(zs, L_CARD, out->card, sizeof(out->card)) &&
		  get_tstr(zs, L_BORROWER, out->borrower, sizeof(out->borrower)) &&
		  get_u16(zs, L_OUT, &out->out) && get_u16(zs, L_DUE, &out->due) &&
		  get_u8(zs, L_RENEWALS, &out->renewals) &&
		  zcbor_uint32_expect(zs, L_RETURNED) &&
		  zcbor_bool_decode(zs, &out->returned) &&
		  get_u16(zs, L_RETURNED_ON, &out->returned_on) &&
		  zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int lbdb_sb_cbor_encode(const struct lbdb_superblock *sb, uint8_t *buf,
			size_t buf_sz, size_t *out_len)
{
	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, SB_PAIRS) &&
		  zcbor_uint32_put(zs, SB_VERSION) &&
		  zcbor_uint32_put(zs, LBDB_SB_VERSION) &&
		  zcbor_uint32_put(zs, SB_BOOKS_ROOT) &&
		  zcbor_uint64_put(zs, sb->books_root) &&
		  zcbor_uint32_put(zs, SB_LOANS_ROOT) &&
		  zcbor_uint64_put(zs, sb->loans_root) &&
		  zcbor_uint32_put(zs, SB_N_BOOKS) && zcbor_uint32_put(zs, sb->n_books) &&
		  zcbor_uint32_put(zs, SB_N_LOANS) && zcbor_uint32_put(zs, sb->n_loans) &&
		  zcbor_uint32_put(zs, SB_N_ACTIVE) &&
		  zcbor_uint32_put(zs, sb->n_active) &&
		  zcbor_map_end_encode(zs, SB_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int lbdb_sb_cbor_decode(const uint8_t *buf, size_t len,
			struct lbdb_superblock *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	if (!zcbor_map_start_decode(zs) || !zcbor_uint32_expect(zs, SB_VERSION) ||
	    !zcbor_uint32_decode(zs, &out->version)) {
		return -EILSEQ;
	}
	if (out->version != LBDB_SB_VERSION) {
		return -ENOTSUP;
	}

	bool ok = zcbor_uint32_expect(zs, SB_BOOKS_ROOT) &&
		  zcbor_uint64_decode(zs, &out->books_root) &&
		  zcbor_uint32_expect(zs, SB_LOANS_ROOT) &&
		  zcbor_uint64_decode(zs, &out->loans_root) &&
		  zcbor_uint32_expect(zs, SB_N_BOOKS) &&
		  zcbor_uint32_decode(zs, &out->n_books) &&
		  zcbor_uint32_expect(zs, SB_N_LOANS) &&
		  zcbor_uint32_decode(zs, &out->n_loans) &&
		  zcbor_uint32_expect(zs, SB_N_ACTIVE) &&
		  zcbor_uint32_decode(zs, &out->n_active) && zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}
