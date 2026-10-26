/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * library — the made-up books and loans the library store is filled with, as
 * a pure function of an index: the library database's counterpart of
 * flights.h. Fictional titles by fictional authors, shelved by subject.
 *
 * Nothing here reads or writes storage. A book's loans are decided by the
 * book index and copy alone, so a fill writes the same catalogue every time;
 * the loan positions a book names depend on the order loans were written, and
 * are filled in by the scenario layer.
 */

#ifndef APP_CBOR_PERSONDB_LIBRARY_H_
#define APP_CBOR_PERSONDB_LIBRARY_H_

#include <stdbool.h>
#include <stdint.h>

#include "librarydb.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Books the generator defines: the most a fill can add. */
#define LIBRARY_BOOKS LIBRARYDB_BOOKS_MAX

/** Fill @p out with book @p index, every copy on the shelf. */
void library_book(uint32_t index, struct librarydb_book *out);

/**
 * @brief The loan copy @p copy of book @p index is out on, on day @p today.
 *
 * @retval true  the copy is out; @p out holds the loan
 * @retval false the copy is on the shelf
 */
bool library_loan(uint32_t index, uint8_t copy, uint16_t today,
		  struct librarydb_loan *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_CBOR_PERSONDB_LIBRARY_H_ */
