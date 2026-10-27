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

#include <zephyr/sys/util.h>

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

/* Each carrier's hub is an airport above; its aircraft are registered
 * under the prefix. */
static const struct {
	const char *code;
	const char *name;
	const char *callsign;
	const char *hub;
	const char *alliance;
	const char *reg;
	uint16_t    founded;
} carriers[] = {
	{ "XA", "Aurora Airways",   "AURORA",    "NVK", "SKYCIRCLE", "VD", 1948 },
	{ "QV", "Quillvale Air",    "QUILLVALE", "TSQ", "",          "LM", 1971 },
	{ "KR", "Kestrel Regional", "KESTREL",   "AMB", "SKYCIRCLE", "GR", 1989 },
	{ "VL", "Vellum Airlines",  "VELLUM",    "KZD", "MERIDIAN",  "SH", 1962 },
	{ "ZN", "Zenith Nordic",    "ZENITH",    "ORS", "MERIDIAN",  "NR", 1957 },
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

/* A hash mixer, so fields of one flight are independent. */
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

/* -- passengers and bookings -------------------------------------------- */

static const char *const surnames[] = {
	"ABERNATHY", "BALOGUN", "CASTELLANO", "DRAGOMIR", "EKSTROM", "FONTAINE",
	"GALLAGHER", "HAKIMI", "IVANOVA", "JANSSEN", "KOWALCZYK", "LINDQVIST",
	"MARCHETTI", "NAKAMURA", "OKONKWO", "PETRAKIS", "QUINTERO", "ROSSETTI",
	"SANTORO", "TANAKA", "UDOVIC", "VASQUEZ", "WINTERBOURNE", "YILMAZ",
	"ZELENKO", "ACHTERBERG", "BRENNAN", "CHAUDHRY", "DESROCHES", "ESPINOZA",
	"FALKENRATH", "GRUBER",
};

static const struct {
	const char *name;
	bool        female;
} givens[] = {
	{ "ADA", true }, { "BORIS", false }, { "CELESTE", true }, { "DMITRI", false },
	{ "ELENA", true }, { "FELIX", false }, { "GRETA", true }, { "HUGO", false },
	{ "INGRID", true }, { "JONAS", false }, { "KAMALA", true }, { "LEANDER", false },
	{ "MAREN", true }, { "NIKOLAI", false }, { "OLIVIA", true }, { "PIETRO", false },
	{ "RASHIDA", true }, { "SOREN", false }, { "TAMSIN", true }, { "VIKTOR", false },
	{ "WILHELMINA", true }, { "YUSUF", false }, { "ZORA", true }, { "AUGUSTIN", false },
};

#define N_SURNAMES ((uint32_t)ARRAY_SIZE(surnames))
#define N_GIVENS   ((uint32_t)ARRAY_SIZE(givens))

/* No I, O, 0 or 1: a locator is read out over the phone. */
static const char locator_chars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

/* Where each cabin's rows start, and its seat letters. Rows only need to
 * be plausible; slot j of a flight's bookings takes row first + j (+ 5k in
 * economy), so no two of them share a seat. */
static const uint8_t cabin_row[FLIGHTDB_CABINS] = { 1, 10, 20 };
static const char *const cabin_seats[FLIGHTDB_CABINS] = { "ADGK", "ACDFHK", "ABCDEFHJK" };

/* Fare per nautical mile, in hundredths, and the floor, by cabin. */
static const uint16_t fare_per_nm[FLIGHTDB_CABINS] = { 95, 48, 14 };
static const uint16_t fare_min[FLIGHTDB_CABINS] = { 900, 420, 59 };

void flights_passenger(uint32_t index, struct flightdb_passenger *out)
{
	uint32_t g, h;

	index %= FLIGHTS_PASSENGERS;
	memset(out, 0, sizeof(*out));
	g = mix(index, 21) % N_GIVENS;
	strcpy(out->surname, surnames[mix(index, 20) % N_SURNAMES]);
	strcpy(out->given, givens[g].name);
	out->birth_year = (uint16_t)(1942u + mix(index, 22) % 74u);
	/* Children under 16 in 2027, the terminal's year; MRS and DR adults only. */
	if (out->birth_year >= 2011u) {
		strcpy(out->title, givens[g].female ? "MISS" : "MSTR");
	} else if (givens[g].female) {
		strcpy(out->title, (out->birth_year < 1995u && (mix(index, 23) & 1u)) ? "MRS" : "MS");
	} else {
		strcpy(out->title, (out->birth_year < 1995u && mix(index, 23) % 8u == 0u) ? "DR"
										      : "MR");
	}
	strcpy(out->nationality, airports[mix(index, 24) % N_AIRPORTS].country);

	/* Two in five are in a programme, the higher tiers rarer. */
	h = mix(index, 25) % 100u;
	if (h < 40u) {
		snprintf(out->ffp, sizeof(out->ffp), "%s%07u",
			 carriers[mix(index, 26) % N_CARRIERS].code,
			 (unsigned)(mix(index, 27) % 10000000u));
		out->tier = (h < 4u) ? FLIGHTDB_TIER_GOLD
			  : (h < 14u) ? FLIGHTDB_TIER_SILVER : FLIGHTDB_TIER_BLUE;
	}
}

void flights_booking(uint32_t index, struct flightdb_booking *out)
{
	struct flightdb_flight f;
	uint32_t slot = index % BOOKINGS_PER_FLIGHT;
	uint32_t lap, total = 0, r, h, row;
	uint8_t c;

	index %= FLIGHTS_PER_DAY * BOOKINGS_PER_FLIGHT;
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < FLIGHTDB_LOCATOR_LEN; i++) {
		out->locator[i] = locator_chars[mix(index, 30 + (uint32_t)i) % 32u];
	}

	/* The first lap in order, later ones permuted: 389 is coprime with 600. */
	lap = index / FLIGHTS_PASSENGERS;
	out->passenger = (uint16_t)((lap == 0) ? index
				    : ((index % FLIGHTS_PASSENGERS) * 389u + lap * 97u) %
				      FLIGHTS_PASSENGERS);
	out->flight = (uint16_t)(index / BOOKINGS_PER_FLIGHT);

	/* The cabin in proportion to its seats. */
	flights_flight(out->flight, &f);
	for (c = 0; c < FLIGHTDB_CABINS; c++) {
		total += f.seats[c];
	}
	r = mix(index, 40) % total;
	for (c = 0; c + 1 < FLIGHTDB_CABINS && r >= f.seats[c]; c++) {
		r -= f.seats[c];
	}
	out->cabin = c;

	h = mix(index, 41) % 100u;
	out->status = f.cancelled ? FLIGHTDB_BK_CANCELLED
		    : (h < 4u) ? FLIGHTDB_BK_CANCELLED
		    : (h < 10u) ? FLIGHTDB_BK_WAITLISTED : FLIGHTDB_BK_CONFIRMED;
	if (out->status == FLIGHTDB_BK_CONFIRMED) {
		row = cabin_row[c] + slot;
		if (c == FLIGHTDB_CABINS - 1) {
			row += 5u * (mix(index, 42) % 6u);
		}
		/* Rows stop at 49; the modulus tells the compiler so. */
		snprintf(out->seat, sizeof(out->seat), "%u%c", (unsigned)(row % 100u),
			 cabin_seats[c][mix(index, 43) % strlen(cabin_seats[c])]);
		out->checked_in = (mix(index, 44) % 100u) < 65u;
	}
	out->bags = (uint8_t)(mix(index, 45) % ((c == FLIGHTDB_CABINS - 1) ? 3u : 4u));
	out->fare = MAX((uint32_t)fare_min[c], (uint32_t)f.distance * fare_per_nm[c] / 100u) +
		    mix(index, 46) % 60u;
}

/* -- airlines, aircraft and crew ---------------------------------------- */

/* Minutes an aircraft stays at the gate between sectors, and a crew member
 * rests between duties. */
#define TURNAROUND 45u
#define CREW_REST  60u

/* Who works a flight, by aircraft band (fleet[]): two pilots always, a
 * purser from the narrowbodies up, and attendants by size. */
static const uint8_t cabin_crew[ARRAY_SIZE(fleet)] = { 1, 2, 4 };

struct plan_aircraft {
	uint8_t  carrier, band, sub;
	uint8_t  n;
	uint16_t free_at;        /* minute the aircraft can leave again */
	uint16_t sectors[FLIGHTDB_SECTORS_MAX];
};

struct plan_crew {
	uint8_t  carrier, rank;
	uint8_t  n;
	uint16_t free_at;
	uint16_t duties[FLIGHTDB_DUTIES_MAX];
};

/*
 * The day's aircraft rotations and crew rosters, worked out once from the
 * schedule: each flight in departure order takes the first aircraft of its
 * carrier and type, and the first crew member of its carrier and each rank
 * it needs, that is free by then, and a new one when none is. A pure
 * function of the schedule, like everything here; only computed lazily.
 */
static struct {
	bool                 done;
	uint32_t             n_aircraft;
	uint32_t             n_crew;
	struct plan_aircraft aircraft[FLIGHTDB_AIRCRAFT_MAX];
	struct plan_crew     crew[FLIGHTDB_CREW_MAX];
} plan;

static int carrier_of(const struct flightdb_flight *f)
{
	for (int c = 0; c < N_CARRIERS; c++) {
		if (strncmp(f->desig, carriers[c].code, 2) == 0) {
			return c;
		}
	}
	return 0;
}

/* The fleet[] band and type the flight's equipment is. */
static void type_of(const struct flightdb_flight *f, uint8_t *band, uint8_t *sub)
{
	for (uint8_t b = 0; b < ARRAY_SIZE(fleet); b++) {
		for (uint8_t t = 0; t < 2; t++) {
			if (strcmp(f->equipment, fleet[b].type[t]) == 0) {
				*band = b;
				*sub = t;
				return;
			}
		}
	}
	*band = 0;
	*sub = 0;
}

static void plan_assign_crew(uint32_t flight, uint8_t carrier, uint8_t rank, uint16_t dep,
			     uint16_t free_at)
{
	struct plan_crew *m = NULL;

	for (uint32_t i = 0; i < plan.n_crew; i++) {
		struct plan_crew *c = &plan.crew[i];

		if (c->carrier == carrier && c->rank == rank && c->n < FLIGHTDB_DUTIES_MAX &&
		    c->free_at <= dep) {
			m = c;
			break;
		}
	}
	if (m == NULL) {
		if (plan.n_crew == FLIGHTDB_CREW_MAX) {
			return;         /* a roster gap rather than a corrupt store */
		}
		m = &plan.crew[plan.n_crew++];
		m->carrier = carrier;
		m->rank = rank;
	}
	m->duties[m->n++] = (uint16_t)flight;
	m->free_at = free_at;
}

static void plan_build(void)
{
	struct flightdb_flight f;

	if (plan.done) {
		return;
	}
	for (uint32_t i = 0; i < FLIGHTS_PER_DAY; i++) {
		struct plan_aircraft *a = NULL;
		uint8_t carrier, band, sub;
		uint16_t arr;

		flights_flight(i, &f);
		carrier = (uint8_t)carrier_of(&f);
		type_of(&f, &band, &sub);
		arr = (uint16_t)(f.dep + f.delay + f.block);

		for (uint32_t k = 0; k < plan.n_aircraft; k++) {
			struct plan_aircraft *p = &plan.aircraft[k];

			if (p->carrier == carrier && p->band == band && p->sub == sub &&
			    p->n < FLIGHTDB_SECTORS_MAX && p->free_at <= f.dep) {
				a = p;
				break;
			}
		}
		if (a == NULL && plan.n_aircraft < FLIGHTDB_AIRCRAFT_MAX) {
			a = &plan.aircraft[plan.n_aircraft++];
			a->carrier = carrier;
			a->band = band;
			a->sub = sub;
		}
		if (a != NULL) {
			a->sectors[a->n++] = (uint16_t)i;
			a->free_at = (uint16_t)(arr + TURNAROUND);
		}

		if (f.cancelled) {
			continue;       /* nobody is rostered on a flight not operating */
		}
		plan_assign_crew(i, carrier, FLIGHTDB_RANK_CAPTAIN, f.dep, arr + CREW_REST);
		plan_assign_crew(i, carrier, FLIGHTDB_RANK_FIRST_OFFICER, f.dep, arr + CREW_REST);
		if (band > 0) {
			plan_assign_crew(i, carrier, FLIGHTDB_RANK_PURSER, f.dep, arr + CREW_REST);
		}
		for (int k = 0; k < cabin_crew[band]; k++) {
			plan_assign_crew(i, carrier, FLIGHTDB_RANK_ATTENDANT, f.dep,
					 arr + CREW_REST);
		}
	}
	plan.done = true;
}

int flights_airline_count(void)
{
	return N_CARRIERS;
}

void flights_airline(int i, struct flightdb_airline *out)
{
	int c = i % N_CARRIERS;

	memset(out, 0, sizeof(*out));
	strcpy(out->code, carriers[c].code);
	strcpy(out->name, carriers[c].name);
	strcpy(out->callsign, carriers[c].callsign);
	strcpy(out->hub, carriers[c].hub);
	strcpy(out->alliance, carriers[c].alliance);
	out->founded = carriers[c].founded;
	for (int k = 0; k < N_AIRPORTS; k++) {
		if (strcmp(airports[k].code, carriers[c].hub) == 0) {
			strcpy(out->country, airports[k].country);
		}
	}
}

uint32_t flights_aircraft_count(void)
{
	plan_build();
	return plan.n_aircraft;
}

void flights_aircraft(uint32_t i, struct flightdb_aircraft *out)
{
	const struct plan_aircraft *p;
	/* 389 is coprime with 26^3, so every aircraft has its own letters. */
	uint32_t x = (i * 389u + 1000u) % 17576u;

	plan_build();
	p = &plan.aircraft[i % MAX(plan.n_aircraft, 1u)];
	memset(out, 0, sizeof(*out));
	snprintf(out->reg, sizeof(out->reg), "%s-%c%c%c", carriers[p->carrier].reg,
		 'A' + (int)(x / 676u), 'A' + (int)(x / 26u % 26u), 'A' + (int)(x % 26u));
	strcpy(out->type, fleet[p->band].type[p->sub]);
	strcpy(out->airline, carriers[p->carrier].code);
	for (int c = 0; c < FLIGHTDB_CABINS; c++) {
		out->seats[c] = fleet[p->band].seats[p->sub][c];
	}
	out->built = (uint16_t)(1996u + mix(i, 60) % 30u);
	out->n_sectors = p->n;
	memcpy(out->sectors, p->sectors, sizeof(out->sectors));
}

uint32_t flights_crew_count(void)
{
	plan_build();
	return plan.n_crew;
}

void flights_crew(uint32_t i, struct flightdb_crew *out)
{
	const struct plan_crew *p;

	plan_build();
	p = &plan.crew[i % MAX(plan.n_crew, 1u)];
	memset(out, 0, sizeof(*out));
	/* 389 is coprime with 90000: a staff number each, five digits. */
	out->staff_no = 10000u + (i * 389u) % 90000u;
	strcpy(out->surname, surnames[mix(i, 50) % N_SURNAMES]);
	strcpy(out->given, givens[mix(i, 51) % N_GIVENS].name);
	out->rank = p->rank;
	strcpy(out->airline, carriers[p->carrier].code);
	strcpy(out->base, carriers[p->carrier].hub);
	out->n_duties = p->n;
	memcpy(out->duties, p->duties, sizeof(out->duties));
}

/* -- bags ----------------------------------------------------------------- */

uint32_t flights_booking_bags(uint32_t booking)
{
	struct flightdb_booking b;

	flights_booking(booking, &b);
	return (b.status == FLIGHTDB_BK_CONFIRMED && b.checked_in) ? b.bags : 0u;
}

void flights_bag(uint32_t booking, uint32_t k, struct flightdb_bag *out)
{
	struct flightdb_booking b;
	struct flightdb_passenger p;
	struct flightdb_flight f;
	uint32_t n = booking * 4u + k;   /* a booking has at most three bags */
	uint32_t h = mix(n, 70) % 100u;

	flights_booking(booking, &b);
	flights_flight(b.flight, &f);
	flights_passenger(b.passenger, &p);
	memset(out, 0, sizeof(*out));
	/* 389 is coprime with 900000: a tag each, six digits after the carrier. */
	snprintf(out->tag, sizeof(out->tag), "%.2s%06u", f.desig,
		 (unsigned)(100000u + (n * 389u) % 900000u));
	out->booking = (uint16_t)booking;
	out->weight = (uint8_t)(7u + mix(n, 71) % 26u);
	strcpy(out->dest, f.to);
	out->status = (h < 3u) ? FLIGHTDB_BAG_MISHANDLED
		    : (h < 7u) ? FLIGHTDB_BAG_OFFLOADED
		    : (h < 25u) ? FLIGHTDB_BAG_CHECKED : FLIGHTDB_BAG_LOADED;
	out->priority = b.cabin < FLIGHTDB_CABINS - 1 || p.tier >= FLIGHTDB_TIER_SILVER;
}
