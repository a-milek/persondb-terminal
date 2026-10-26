/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flights — the made-up airports and flights, as a pure function of an index.
 * See flights.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flights.h"

/* Fictional places on a fictional map, roughly 4 000 NM across. The
 * coordinates decide the distances, and the distances decide block times and
 * aircraft, so a route always gets the same aircraft and duration. */
struct place {
	const char *code, *name, *city, *country;
	int8_t      utc_offset;
	uint16_t    elevation;
	uint8_t     runways;
	uint8_t     gates;
	int16_t     x, y;       /* position on the map, nautical miles */
};

static const struct place airports[] = {
	{ "NVK", "Novikar Intl",        "Novikar",    "Valdria",    1,   312, 3, 48,     0,     0 },
	{ "PLM", "Pellamar Harbour",    "Pellamar",   "Valdria",    1,    18, 2, 22,   210,  -340 },
	{ "ORS", "Orskald Fjordvik",    "Orskald",    "Norrheim",   1,   145, 2, 18,   280,   910 },
	{ "HGV", "Hegvangen",           "Hegvangen",  "Norrheim",   2,   960, 1,  8,   720,  1260 },
	{ "TSQ", "Tessaquin Central",   "Tessaquin",  "Lusmora",    0,   402, 2, 30,  -880,   150 },
	{ "MRB", "Marbeau St-Clair",    "Marbeau",    "Lusmora",    0,   230, 2, 16, -1120,  -260 },
	{ "KZD", "Kazdhan Alminar",     "Kazdhan",    "Sarhaddin",  4,  3870, 3, 40,  2650,  -420 },
	{ "QRT", "Qirat Oasis",         "Qirat",      "Sarhaddin",  4,  1540, 1, 10,  2310, -1080 },
	{ "BLZ", "Belzora Ridge",       "Belzora",    "Montecaia",  1,  2180, 2, 14,   540,  -870 },
	{ "CVN", "Cavenna Marittima",   "Cavenna",    "Montecaia",  1,    12, 2, 20,   830, -1240 },
	{ "DRW", "Dunraven Moor",       "Dunraven",   "Ashmere",   -1,   590, 2, 26, -1560,   720 },
	{ "LYF", "Lyford Bay",          "Lyford",     "Ashmere",   -1,    40, 1,  9, -1840,   330 },
	{ "YKT", "Yaketsk Polar",       "Yaketsk",    "Ostrava Nord", 5, 290, 1,  6,  2890,  1680 },
	{ "VRN", "Varenko Gagarin",     "Varenko",    "Ostrava Nord", 3, 670, 3, 36,  1640,   820 },
	{ "SLT", "Saltrin Flats",       "Saltrin",    "Varethi",    2,  1210, 2, 12,  1180,  -210 },
	{ "AMB", "Ambrecht Kaiserfeld", "Ambrecht",   "Grauland",   1,   480, 4, 60,   190,   420 },
	{ "WLD", "Waldhafen",           "Waldhafen",  "Grauland",   1,  1020, 1, 11,   470,   180 },
	{ "IZM", "Izmara Coast",        "Izmara",     "Qalendar",   3,    65, 2, 24,  1720, -1420 },
	{ "TRG", "Torgalla Highlands",  "Torgalla",   "Qalendar",   3,  5320, 1,  7,  2060, -1760 },
	{ "EMR", "Emberline Sound",     "Emberline",  "Ashmere",   -1,    22, 2, 15, -1390,    40 },
};

#define N_AIRPORTS ((int)(sizeof(airports) / sizeof(airports[0])))

static const struct {
	const char *code;
	const char *name;
} carriers[] = {
	{ "XA", "Aurora Airways" },
	{ "QV", "Quillvale Air" },
	{ "KR", "Kestrel Regional" },
	{ "VL", "Vellum Airlines" },
	{ "ZN", "Zenith Nordic" },
};

#define N_CARRIERS ((int)(sizeof(carriers) / sizeof(carriers[0])))

struct equipment {
	uint16_t    max_nm;
	const char *type[2];
	uint16_t    seats[2][FLIGHTDB_CABINS];
};

/* Two types per band, picked per flight. */
static const struct equipment fleet[] = {
	{  500, { "AT76", "E190" }, { { 0,  0,  70 }, { 0, 12,  88 } } },
	{ 1800, { "A320", "B738" }, { { 0, 12, 150 }, { 0, 16, 146 } } },
	{ UINT16_MAX, { "B789", "A359" }, { { 8, 48, 234 }, { 12, 42, 261 } } },
};

/* The same mixer as dataset.c, so fields of one flight are independent. */
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

static uint32_t isqrt(uint32_t v)
{
	uint32_t r = 0;

	for (uint32_t bit = 1u << 30; bit != 0; bit >>= 2) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
	}
	return r;
}

int flights_airport_count(void)
{
	return N_AIRPORTS;
}

void flights_airport(int i, struct flightdb_airport *out)
{
	const struct place *p = &airports[i % N_AIRPORTS];

	memset(out, 0, sizeof(*out));
	strcpy(out->code, p->code);
	strcpy(out->name, p->name);
	strcpy(out->city, p->city);
	strcpy(out->country, p->country);
	out->utc_offset = p->utc_offset;
	out->elevation = p->elevation;
	out->runways = p->runways;
	out->gates = p->gates;
}

void flights_flight(uint32_t index, struct flightdb_flight *out)
{
	const struct place *a, *b;
	const struct equipment *eq;
	uint32_t dx, dy, h, pct;
	int from, to, carrier, sub;

	index %= FLIGHTS_PER_DAY;
	memset(out, 0, sizeof(*out));

	carrier = (int)(mix(index, 1) % N_CARRIERS);
	/* 389 is coprime with 900, so every flight of the day has its own number. */
	snprintf(out->desig, sizeof(out->desig), "%s%04u", carriers[carrier].code,
		 100u + (index * 389u) % 900u);
	strcpy(out->airline, carriers[carrier].name);

	from = (int)(mix(index, 2) % N_AIRPORTS);
	to = (int)((from + 1u + mix(index, 3) % (N_AIRPORTS - 1)) % N_AIRPORTS);
	a = &airports[from];
	b = &airports[to];
	strcpy(out->from, a->code);
	strcpy(out->to, b->code);

	/* One departure every six minutes, nudged by up to four: still in order. */
	out->dep = (uint16_t)(index * (FLIGHTDB_MIN_PER_DAY / FLIGHTS_PER_DAY) +
			      mix(index, 4) % 5u);

	dx = (uint32_t)abs(a->x - b->x);
	dy = (uint32_t)abs(a->y - b->y);
	out->distance = (uint16_t)isqrt(dx * dx + dy * dy);
	/* 450 kt cruise plus half an hour of taxi, climb and approach, in fives. */
	out->block = (uint16_t)((30u + out->distance * 60u / 450u + 4u) / 5u * 5u);

	h = mix(index, 5) % 100u;
	if (h < 70u) {
		out->delay = 0;
	} else if (h < 90u) {
		out->delay = (uint16_t)(5u + mix(index, 6) % 6u * 5u);
	} else if (h < 97u) {
		out->delay = (uint16_t)(30u + mix(index, 6) % 19u * 5u);
	} else {
		out->cancelled = true;
	}

	eq = &fleet[0];
	while (out->distance > eq->max_nm) {
		eq++;
	}
	sub = (int)(mix(index, 7) & 1u);
	strcpy(out->equipment, eq->type[sub]);
	for (int c = 0; c < FLIGHTDB_CABINS; c++) {
		out->seats[c] = eq->seats[sub][c];
		pct = 55u + mix(index, 8 + (uint32_t)c) % 46u;   /* 55..100 % full */
		out->booked[c] = (uint16_t)(out->seats[c] * pct / 100u);
	}

	h = mix(index, 11);
	{
		int pier = (int)(h % 4u);

		snprintf(out->gate, sizeof(out->gate), "%c%u", 'A' + pier,
			 (unsigned)(1u + (h >> 8) % (a->gates / 2u + 1u)));
		out->terminal = (uint8_t)(1 + pier / 2);
	}
}
