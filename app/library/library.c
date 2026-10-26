/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * library — the made-up books and loans, as a pure function of an index.
 * See library.h.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "library.h"

/* Shelved by Dewey class; the class decides the call number and the range of
 * years and lengths a book of that subject has. */
static const struct {
	const char *name;
	uint16_t    dewey;
	uint16_t    year_from;
	uint16_t    pages_min, pages_span;
} subjects[] = {
	{ "Fiction",     823, 1950, 180, 420 },
	{ "Poetry",      821, 1920,  60, 140 },
	{ "History",     909, 1960, 240, 560 },
	{ "Science",     500, 1975, 200, 380 },
	{ "Computing",     5, 1990, 280, 620 },
	{ "Philosophy",  100, 1940, 160, 300 },
	{ "Art",         700, 1965, 120, 260 },
	{ "Music",       780, 1970, 140, 300 },
	{ "Travel",      910, 1985, 160, 260 },
	{ "Cookery",     641, 1980, 120, 280 },
};

#define N_SUBJECTS ((int)ARRAY_SIZE(subjects))

static const char *const surnames[] = {
	"Aalto", "Bercu", "Cheng", "Duarte", "Eriksen", "Farhi", "Gruber",
	"Haddad", "Ivanov", "Jarvis", "Kovac", "Lindqvist", "Moreau", "Nagy",
	"Okafor", "Pandit", "Quist", "Rasku", "Sorensen", "Tanaka", "Ulloa",
	"Varga", "Whitlow", "Zeller",
};

static const char *const given[] = {
	"Ada", "Bo", "Cira", "Dov", "Eve", "Finn", "Gita", "Hal",
	"Iris", "Juno", "Kai", "Lena", "Milo", "Nils", "Oona", "Pax",
};

static const char *const adjectives[] = {
	"Silent", "Northern", "Hollow", "Golden", "Last", "Hidden", "Winter",
	"Iron", "Distant", "Paper", "Salt", "Quiet", "Burning", "Glass",
	"Lost", "Second",
};

static const char *const nouns[] = {
	"Harbour", "Garden", "Archive", "River", "Lantern", "Orchard", "Tower",
	"Map", "Signal", "Valley", "Engine", "Mirror", "Island", "Bridge",
	"Forest", "Compass",
};

/* Per subject, so a cookery book does not read like a novel. */
static const char *const topics[N_SUBJECTS][4] = {
	{ "", "", "", "" },
	{ "Songs", "Elegies", "Verses", "Sonnets" },
	{ "Empires", "Trade Routes", "Revolutions", "Ports" },
	{ "Light", "Tides", "Crystals", "Weather" },
	{ "Compilers", "Networks", "Databases", "Embedded C" },
	{ "Doubt", "Time", "Virtue", "Language" },
	{ "Colour", "Fresco", "Print", "Sculpture" },
	{ "Harmony", "Rhythm", "Organs", "Folk Song" },
	{ "Fjords", "Deserts", "Islands", "Railways" },
	{ "Bread", "Soups", "Spices", "Preserves" },
};

#define PICK(arr, h) ((arr)[(h) % ARRAY_SIZE(arr)])

/* The same mixer as dataset.c and flights.c. */
static uint32_t mix(uint32_t x, uint32_t salt)
{
	x += salt * 0x9e3779b9u;
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static void isbn13(uint32_t index, char *out)
{
	uint32_t body = 100000000u + mix(index, 30) % 900000000u;
	int sum = 0;

	snprintf(out, LIBRARYDB_ISBN_LEN + 1, "978%09u", (unsigned)body);
	for (int i = 0; i < 12; i++) {
		sum += (out[i] - '0') * ((i & 1) ? 3 : 1);
	}
	out[12] = (char)('0' + (10 - sum % 10) % 10);
	out[13] = '\0';
}

static void title(uint32_t index, int subject, char *out, size_t sz)
{
	const char *adj = PICK(adjectives, mix(index, 21));
	const char *noun = PICK(nouns, mix(index, 22));
	const char *noun2 = PICK(nouns, mix(index, 23) + 1);
	uint32_t form = mix(index, 24) % 4u;

	if (subject != 0) {
		const char *topic = topics[subject][mix(index, 25) % 4u];

		switch (mix(index, 33) % 6u) {
		case 0:  snprintf(out, sz, "A Short Guide to %s", topic); break;
		case 1:  snprintf(out, sz, "%s: A History", topic); break;
		case 2:  snprintf(out, sz, "Notes on %s", topic); break;
		case 3:  snprintf(out, sz, "The Book of %s", topic); break;
		case 4:  snprintf(out, sz, "%s and the %s", topic, noun); break;
		default: snprintf(out, sz, "%s %s", adj, topic); break;
		}
		return;
	}
	switch (form) {
	case 0:  snprintf(out, sz, "The %s %s", adj, noun); break;
	case 1:  snprintf(out, sz, "The %s of the %s", noun, noun2); break;
	case 2:  snprintf(out, sz, "%s %ss", adj, noun); break;
	default: snprintf(out, sz, "Beyond the %s", noun); break;
	}
}

void library_book(uint32_t index, struct librarydb_book *out)
{
	int s = (int)(mix(index, 20) % N_SUBJECTS);
	const char *surname = PICK(surnames, mix(index, 26));
	uint32_t span;

	memset(out, 0, sizeof(*out));
	isbn13(index, out->isbn);
	title(index, s, out->title, sizeof(out->title));
	snprintf(out->author, sizeof(out->author), "%s, %c.", surname,
		 'A' + (char)(mix(index, 27) % 26u));
	strcpy(out->subject, subjects[s].name);
	snprintf(out->call, sizeof(out->call), "%03u.%02u %.3s", subjects[s].dewey % 1000u,
		 (unsigned)(mix(index, 28) % 100u), surname);
	for (char *p = out->call; *p != '\0'; p++) {
		*p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
	}
	span = 2027u - subjects[s].year_from;
	out->year = (uint16_t)(subjects[s].year_from + mix(index, 29) % (span + 1u));
	out->pages = (uint16_t)(subjects[s].pages_min +
				mix(index, 31) % (subjects[s].pages_span + 1u));
	/* Most books have one copy; popular ones up to four. */
	{
		uint32_t h = mix(index, 32) % 100u;

		out->copies = (h < 55u) ? 1 : (h < 80u) ? 2 : (h < 93u) ? 3 : 4;
	}
	for (int c = 0; c < LIBRARYDB_COPIES_MAX; c++) {
		out->loan[c] = LIBRARYDB_NO_LOAN;
	}
}

bool library_loan(uint32_t index, uint8_t copy, uint16_t today,
		  struct librarydb_loan *out)
{
	uint32_t k = index * LIBRARYDB_COPIES_MAX + copy;
	uint32_t ago;

	/* About a third of all copies are out. */
	if (mix(k, 40) % 100u >= 35u) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->book = (uint16_t)index;
	out->copy = copy;
	snprintf(out->card, sizeof(out->card), "B%05u", (unsigned)(10000u + mix(k, 41) % 90000u));
	snprintf(out->borrower, sizeof(out->borrower), "%s/%s", PICK(surnames, mix(k, 42)),
		 PICK(given, mix(k, 43)));
	for (char *p = out->borrower; *p != '\0'; p++) {
		*p = (*p >= 'a' && *p <= 'z') ? (char)(*p - 'a' + 'A') : *p;
	}
	/* Lent up to 50 days ago, renewed up to twice: some are overdue. */
	ago = 1u + mix(k, 44) % 50u;
	out->renewals = (uint8_t)(mix(k, 45) % 100u < 70u ? 0 : 1 + mix(k, 46) % 2u);
	out->out = (uint16_t)(today - ago);
	out->due = (uint16_t)(out->out + LIBRARYDB_LOAN_DAYS * (1u + out->renewals));
	return true;
}
