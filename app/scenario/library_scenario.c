/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * library_scenario — fill, verify and reset the library store. See the header.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "library.h"
#include "library_scenario.h"

int library_scenario_open(struct library_scenario *s)
{
	return librarydb_open(&s->db);
}

int library_scenario_close(struct library_scenario *s)
{
	return librarydb_close(s->db);
}

uint16_t library_scenario_today(void)
{
	return (uint16_t)((LIBRARY_SCENARIO_NOW + (uint32_t)(k_uptime_get() / 1000)) / 86400u);
}

uint32_t library_scenario_books_defined(void)
{
	return MIN((uint32_t)LIBRARY_BOOKS, (uint32_t)LIBRARYDB_BOOKS_MAX);
}

int library_scenario_fill(struct library_scenario *s, uint32_t n, uint32_t *written)
{
	struct librarydb_book b;
	struct librarydb_loan l;
	uint16_t today = library_scenario_today();
	uint32_t nb, nl, na, end;
	int rc = 0;

	*written = 0;
	librarydb_counts(s->db, &nb, &nl, &na);

	end = MIN(nb + n, library_scenario_books_defined());
	for (uint32_t i = nb; i < end && rc == 0; i++) {
		uint32_t loans = 0;

		library_book(i, &b);
		/* The loans first, then the book naming them. */
		for (uint8_t c = 0; c < b.copies && rc == 0; c++) {
			if (nl + loans >= LIBRARYDB_LOANS_MAX || !library_loan(i, c, today, &l)) {
				continue;
			}
			rc = librarydb_loan_put(s->db, nl + loans, &l);
			if (rc == 0) {
				b.loan[c] = (uint16_t)(nl + loans);
				loans++;
			}
		}
		if (rc == 0) {
			rc = librarydb_book_put(s->db, i, &b);
		}
		if (rc == 0) {
			(*written)++;
			nl += loans;
			na += loans;
		}
	}
	/* Publish what made it, even after a failure part-way. */
	if (*written > 0) {
		int rc2 = librarydb_counts_set(s->db, nb + *written, nl, na);

		rc = (rc != 0) ? rc : rc2;
	}
	return rc;
}

int library_scenario_verify(struct library_scenario *s, struct library_verify_report *r)
{
	struct librarydb_book b, gb;
	struct librarydb_loan l;
	int64_t t0 = k_uptime_get();
	uint32_t nb, nl, na, active = 0;

	*r = (struct library_verify_report){ 0 };
	librarydb_counts(s->db, &nb, &nl, &na);

	for (uint32_t i = 0; i < nb; i++) {
		r->books_checked++;
		if (librarydb_book_get(s->db, i, &b) != 0) {
			r->bad++;
			continue;
		}
		library_book(i, &gb);
		memcpy(gb.loan, b.loan, sizeof(gb.loan));
		if (!librarydb_book_equal(&b, &gb)) {
			r->bad++;
			continue;
		}
		/* Each copy out names a loan of this book and copy, still out. */
		for (uint8_t c = 0; c < b.copies; c++) {
			if (b.loan[c] == LIBRARYDB_NO_LOAN) {
				continue;
			}
			if (librarydb_loan_get(s->db, b.loan[c], &l) != 0 || l.book != i ||
			    l.copy != c || l.returned) {
				r->bad++;
			}
		}
	}

	/* And each loan still out is the one its book names. */
	for (uint32_t j = 0; j < nl; j++) {
		r->loans_checked++;
		if (librarydb_loan_get(s->db, j, &l) != 0 || l.book >= nb) {
			r->bad++;
			continue;
		}
		if (l.returned) {
			continue;
		}
		active++;
		if (librarydb_book_get(s->db, l.book, &b) != 0 || l.copy >= b.copies ||
		    b.loan[l.copy] != j) {
			r->bad++;
		}
	}
	if (active != na) {
		r->bad++;
	}
	r->ms = k_uptime_get() - t0;
	return 0;
}

int library_scenario_erase(struct library_scenario *s)
{
	return librarydb_erase(s->db);
}
