/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * flight_cbor — zcbor encode/decode for the flight store. See flight_cbor.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>

#include "flight_cbor.h"

#define NBACKUPS 2

/* Map keys. Never renumber: the store outlives the build that wrote it. */
enum {
	A_CODE = 1, A_NAME, A_CITY, A_COUNTRY, A_UTC, A_ELEV, A_RWY, A_GATES,
};
#define AIRPORT_PAIRS 8

enum {
	F_DESIG = 1, F_AIRLINE, F_FROM, F_TO, F_DEP, F_BLOCK, F_DELAY,
	F_CANCELLED, F_DISTANCE, F_EQUIP, F_GATE, F_TERMINAL, F_SEATS, F_BOOKED,
};
#define FLIGHT_PAIRS 14

enum {
	P_SURNAME = 1, P_GIVEN, P_TITLE, P_NATIONALITY, P_BIRTH, P_FFP, P_TIER,
};
#define PASSENGER_PAIRS 7

enum {
	B_LOCATOR = 1, B_PASSENGER, B_FLIGHT, B_CABIN, B_SEAT, B_STATUS,
	B_CHECKED_IN, B_BAGS, B_FARE,
};
#define BOOKING_PAIRS 9

enum {
	L_CODE = 1, L_NAME, L_CALLSIGN, L_HUB, L_COUNTRY, L_ALLIANCE, L_FOUNDED,
};
#define AIRLINE_PAIRS 7

enum {
	K_REG = 1, K_TYPE, K_AIRLINE, K_SEATS, K_BUILT, K_SECTORS,
};
#define AIRCRAFT_PAIRS 6

enum {
	C_STAFF = 1, C_SURNAME, C_GIVEN, C_RANK, C_AIRLINE, C_BASE, C_DUTIES,
};
#define CREW_PAIRS 7

enum {
	G_TAG = 1, G_BOOKING, G_WEIGHT, G_DEST, G_STATUS, G_PRIORITY,
};
#define BAG_PAIRS 6

enum {
	SB_VERSION = 1, SB_AIRPORTS_ROOT, SB_FLIGHTS_ROOT, SB_N_AIRPORTS,
	SB_N_FLIGHTS, SB_PASSENGERS_ROOT, SB_BOOKINGS_ROOT, SB_N_PASSENGERS,
	SB_N_BOOKINGS, SB_AIRLINES_ROOT, SB_AIRCRAFT_ROOT, SB_CREW_ROOT,
	SB_BAGS_ROOT, SB_N_AIRLINES, SB_N_AIRCRAFT, SB_N_CREW, SB_N_BAGS,
};
#define SB_PAIRS 17

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

static bool put_cabins(zcbor_state_t *zs, uint32_t key, const uint16_t *v)
{
	bool ok = zcbor_uint32_put(zs, key) &&
		  zcbor_list_start_encode(zs, FLIGHTDB_CABINS);

	for (int i = 0; ok && i < FLIGHTDB_CABINS; i++) {
		ok = zcbor_uint32_put(zs, v[i]);
	}
	return ok && zcbor_list_end_encode(zs, FLIGHTDB_CABINS);
}

static bool get_cabins(zcbor_state_t *zs, uint32_t key, uint16_t *v)
{
	uint32_t x;
	int n = 0;

	if (!zcbor_uint32_expect(zs, key) || !zcbor_list_start_decode(zs)) {
		return false;
	}
	while (!zcbor_array_at_end(zs)) {
		if (n == FLIGHTDB_CABINS || !zcbor_uint32_decode(zs, &x) ||
		    x > UINT16_MAX) {
			return false;
		}
		v[n++] = (uint16_t)x;
	}
	return n == FLIGHTDB_CABINS && zcbor_list_end_decode(zs);
}

/* A list of up to @p max positions, as many as @p n says. */
static bool put_u16_list(zcbor_state_t *zs, uint32_t key, const uint16_t *v, uint8_t n,
			 size_t max)
{
	bool ok = n <= max && zcbor_uint32_put(zs, key) && zcbor_list_start_encode(zs, max);

	for (int i = 0; ok && i < n; i++) {
		ok = zcbor_uint32_put(zs, v[i]);
	}
	return ok && zcbor_list_end_encode(zs, max);
}

static bool get_u16_list(zcbor_state_t *zs, uint32_t key, uint16_t *v, uint8_t *n,
			 size_t max)
{
	uint32_t x;

	*n = 0;
	if (!zcbor_uint32_expect(zs, key) || !zcbor_list_start_decode(zs)) {
		return false;
	}
	while (!zcbor_array_at_end(zs)) {
		if (*n == max || !zcbor_uint32_decode(zs, &x) || x > UINT16_MAX) {
			return false;
		}
		v[(*n)++] = (uint16_t)x;
	}
	return zcbor_list_end_decode(zs);
}

static bool fits(const char *s, size_t max)
{
	return strnlen(s, max + 1) <= max;
}

int airport_cbor_encode(const struct flightdb_airport *a, uint8_t *buf,
			size_t buf_sz, size_t *out_len)
{
	if (!fits(a->code, FLIGHTDB_CODE_LEN) || !fits(a->name, FLIGHTDB_NAME_MAX) ||
	    !fits(a->city, FLIGHTDB_CITY_MAX) ||
	    !fits(a->country, FLIGHTDB_COUNTRY_MAX)) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, AIRPORT_PAIRS) &&
		  zcbor_uint32_put(zs, A_CODE) &&
		  zcbor_tstr_put_term(zs, a->code, FLIGHTDB_CODE_LEN) &&
		  zcbor_uint32_put(zs, A_NAME) &&
		  zcbor_tstr_put_term(zs, a->name, FLIGHTDB_NAME_MAX) &&
		  zcbor_uint32_put(zs, A_CITY) &&
		  zcbor_tstr_put_term(zs, a->city, FLIGHTDB_CITY_MAX) &&
		  zcbor_uint32_put(zs, A_COUNTRY) &&
		  zcbor_tstr_put_term(zs, a->country, FLIGHTDB_COUNTRY_MAX) &&
		  zcbor_uint32_put(zs, A_UTC) && zcbor_int32_put(zs, a->utc_offset) &&
		  zcbor_uint32_put(zs, A_ELEV) && zcbor_uint32_put(zs, a->elevation) &&
		  zcbor_uint32_put(zs, A_RWY) && zcbor_uint32_put(zs, a->runways) &&
		  zcbor_uint32_put(zs, A_GATES) && zcbor_uint32_put(zs, a->gates) &&
		  zcbor_map_end_encode(zs, AIRPORT_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int airport_cbor_decode(const uint8_t *buf, size_t len,
			struct flightdb_airport *out)
{
	int32_t utc;

	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, A_CODE, out->code, sizeof(out->code)) &&
		  get_tstr(zs, A_NAME, out->name, sizeof(out->name)) &&
		  get_tstr(zs, A_CITY, out->city, sizeof(out->city)) &&
		  get_tstr(zs, A_COUNTRY, out->country, sizeof(out->country)) &&
		  zcbor_uint32_expect(zs, A_UTC) && zcbor_int32_decode(zs, &utc) &&
		  utc >= INT8_MIN && utc <= INT8_MAX &&
		  get_u16(zs, A_ELEV, &out->elevation) &&
		  get_u8(zs, A_RWY, &out->runways) &&
		  get_u8(zs, A_GATES, &out->gates) && zcbor_map_end_decode(zs);

	if (!ok) {
		return -EILSEQ;
	}
	out->utc_offset = (int8_t)utc;
	return 0;
}

int flight_cbor_encode(const struct flightdb_flight *f, uint8_t *buf,
		       size_t buf_sz, size_t *out_len)
{
	if (!fits(f->desig, FLIGHTDB_DESIG_MAX) ||
	    !fits(f->airline, FLIGHTDB_AIRLINE_MAX) ||
	    !fits(f->from, FLIGHTDB_CODE_LEN) || !fits(f->to, FLIGHTDB_CODE_LEN) ||
	    !fits(f->equipment, FLIGHTDB_EQUIP_MAX) ||
	    !fits(f->gate, FLIGHTDB_GATE_MAX)) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, FLIGHT_PAIRS) &&
		  zcbor_uint32_put(zs, F_DESIG) &&
		  zcbor_tstr_put_term(zs, f->desig, FLIGHTDB_DESIG_MAX) &&
		  zcbor_uint32_put(zs, F_AIRLINE) &&
		  zcbor_tstr_put_term(zs, f->airline, FLIGHTDB_AIRLINE_MAX) &&
		  zcbor_uint32_put(zs, F_FROM) &&
		  zcbor_tstr_put_term(zs, f->from, FLIGHTDB_CODE_LEN) &&
		  zcbor_uint32_put(zs, F_TO) &&
		  zcbor_tstr_put_term(zs, f->to, FLIGHTDB_CODE_LEN) &&
		  zcbor_uint32_put(zs, F_DEP) && zcbor_uint32_put(zs, f->dep) &&
		  zcbor_uint32_put(zs, F_BLOCK) && zcbor_uint32_put(zs, f->block) &&
		  zcbor_uint32_put(zs, F_DELAY) && zcbor_uint32_put(zs, f->delay) &&
		  zcbor_uint32_put(zs, F_CANCELLED) && zcbor_bool_put(zs, f->cancelled) &&
		  zcbor_uint32_put(zs, F_DISTANCE) && zcbor_uint32_put(zs, f->distance) &&
		  zcbor_uint32_put(zs, F_EQUIP) &&
		  zcbor_tstr_put_term(zs, f->equipment, FLIGHTDB_EQUIP_MAX) &&
		  zcbor_uint32_put(zs, F_GATE) &&
		  zcbor_tstr_put_term(zs, f->gate, FLIGHTDB_GATE_MAX) &&
		  zcbor_uint32_put(zs, F_TERMINAL) && zcbor_uint32_put(zs, f->terminal) &&
		  put_cabins(zs, F_SEATS, f->seats) && put_cabins(zs, F_BOOKED, f->booked) &&
		  zcbor_map_end_encode(zs, FLIGHT_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int flight_cbor_decode(const uint8_t *buf, size_t len,
		       struct flightdb_flight *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, F_DESIG, out->desig, sizeof(out->desig)) &&
		  get_tstr(zs, F_AIRLINE, out->airline, sizeof(out->airline)) &&
		  get_tstr(zs, F_FROM, out->from, sizeof(out->from)) &&
		  get_tstr(zs, F_TO, out->to, sizeof(out->to)) &&
		  get_u16(zs, F_DEP, &out->dep) &&
		  get_u16(zs, F_BLOCK, &out->block) &&
		  get_u16(zs, F_DELAY, &out->delay) &&
		  zcbor_uint32_expect(zs, F_CANCELLED) &&
		  zcbor_bool_decode(zs, &out->cancelled) &&
		  get_u16(zs, F_DISTANCE, &out->distance) &&
		  get_tstr(zs, F_EQUIP, out->equipment, sizeof(out->equipment)) &&
		  get_tstr(zs, F_GATE, out->gate, sizeof(out->gate)) &&
		  get_u8(zs, F_TERMINAL, &out->terminal) &&
		  get_cabins(zs, F_SEATS, out->seats) &&
		  get_cabins(zs, F_BOOKED, out->booked) && zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int passenger_cbor_encode(const struct flightdb_passenger *p, uint8_t *buf,
			  size_t buf_sz, size_t *out_len)
{
	if (!fits(p->surname, FLIGHTDB_SURNAME_MAX) || !fits(p->given, FLIGHTDB_GIVEN_MAX) ||
	    !fits(p->title, FLIGHTDB_TITLE_MAX) ||
	    !fits(p->nationality, FLIGHTDB_COUNTRY_MAX) || !fits(p->ffp, FLIGHTDB_FFP_MAX) ||
	    p->tier >= FLIGHTDB_TIER__COUNT) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, PASSENGER_PAIRS) &&
		  zcbor_uint32_put(zs, P_SURNAME) &&
		  zcbor_tstr_put_term(zs, p->surname, FLIGHTDB_SURNAME_MAX) &&
		  zcbor_uint32_put(zs, P_GIVEN) &&
		  zcbor_tstr_put_term(zs, p->given, FLIGHTDB_GIVEN_MAX) &&
		  zcbor_uint32_put(zs, P_TITLE) &&
		  zcbor_tstr_put_term(zs, p->title, FLIGHTDB_TITLE_MAX) &&
		  zcbor_uint32_put(zs, P_NATIONALITY) &&
		  zcbor_tstr_put_term(zs, p->nationality, FLIGHTDB_COUNTRY_MAX) &&
		  zcbor_uint32_put(zs, P_BIRTH) && zcbor_uint32_put(zs, p->birth_year) &&
		  zcbor_uint32_put(zs, P_FFP) &&
		  zcbor_tstr_put_term(zs, p->ffp, FLIGHTDB_FFP_MAX) &&
		  zcbor_uint32_put(zs, P_TIER) && zcbor_uint32_put(zs, p->tier) &&
		  zcbor_map_end_encode(zs, PASSENGER_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int passenger_cbor_decode(const uint8_t *buf, size_t len,
			  struct flightdb_passenger *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, P_SURNAME, out->surname, sizeof(out->surname)) &&
		  get_tstr(zs, P_GIVEN, out->given, sizeof(out->given)) &&
		  get_tstr(zs, P_TITLE, out->title, sizeof(out->title)) &&
		  get_tstr(zs, P_NATIONALITY, out->nationality, sizeof(out->nationality)) &&
		  get_u16(zs, P_BIRTH, &out->birth_year) &&
		  get_tstr(zs, P_FFP, out->ffp, sizeof(out->ffp)) &&
		  get_u8(zs, P_TIER, &out->tier) && out->tier < FLIGHTDB_TIER__COUNT &&
		  zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int booking_cbor_encode(const struct flightdb_booking *b, uint8_t *buf,
			size_t buf_sz, size_t *out_len)
{
	if (!fits(b->locator, FLIGHTDB_LOCATOR_LEN) || !fits(b->seat, FLIGHTDB_SEAT_MAX) ||
	    b->cabin >= FLIGHTDB_CABINS || b->status >= FLIGHTDB_BK__COUNT) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, BOOKING_PAIRS) &&
		  zcbor_uint32_put(zs, B_LOCATOR) &&
		  zcbor_tstr_put_term(zs, b->locator, FLIGHTDB_LOCATOR_LEN) &&
		  zcbor_uint32_put(zs, B_PASSENGER) && zcbor_uint32_put(zs, b->passenger) &&
		  zcbor_uint32_put(zs, B_FLIGHT) && zcbor_uint32_put(zs, b->flight) &&
		  zcbor_uint32_put(zs, B_CABIN) && zcbor_uint32_put(zs, b->cabin) &&
		  zcbor_uint32_put(zs, B_SEAT) &&
		  zcbor_tstr_put_term(zs, b->seat, FLIGHTDB_SEAT_MAX) &&
		  zcbor_uint32_put(zs, B_STATUS) && zcbor_uint32_put(zs, b->status) &&
		  zcbor_uint32_put(zs, B_CHECKED_IN) && zcbor_bool_put(zs, b->checked_in) &&
		  zcbor_uint32_put(zs, B_BAGS) && zcbor_uint32_put(zs, b->bags) &&
		  zcbor_uint32_put(zs, B_FARE) && zcbor_uint32_put(zs, b->fare) &&
		  zcbor_map_end_encode(zs, BOOKING_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int booking_cbor_decode(const uint8_t *buf, size_t len,
			struct flightdb_booking *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, B_LOCATOR, out->locator, sizeof(out->locator)) &&
		  get_u16(zs, B_PASSENGER, &out->passenger) &&
		  get_u16(zs, B_FLIGHT, &out->flight) &&
		  get_u8(zs, B_CABIN, &out->cabin) && out->cabin < FLIGHTDB_CABINS &&
		  get_tstr(zs, B_SEAT, out->seat, sizeof(out->seat)) &&
		  get_u8(zs, B_STATUS, &out->status) && out->status < FLIGHTDB_BK__COUNT &&
		  zcbor_uint32_expect(zs, B_CHECKED_IN) &&
		  zcbor_bool_decode(zs, &out->checked_in) &&
		  get_u8(zs, B_BAGS, &out->bags) &&
		  zcbor_uint32_expect(zs, B_FARE) && zcbor_uint32_decode(zs, &out->fare) &&
		  zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int airline_cbor_encode(const struct flightdb_airline *a, uint8_t *buf,
			size_t buf_sz, size_t *out_len)
{
	if (!fits(a->code, FLIGHTDB_AIRLINE_CODE) || !fits(a->name, FLIGHTDB_AIRLINE_MAX) ||
	    !fits(a->callsign, FLIGHTDB_CALLSIGN_MAX) || !fits(a->hub, FLIGHTDB_CODE_LEN) ||
	    !fits(a->country, FLIGHTDB_COUNTRY_MAX) ||
	    !fits(a->alliance, FLIGHTDB_ALLIANCE_MAX)) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, AIRLINE_PAIRS) &&
		  zcbor_uint32_put(zs, L_CODE) &&
		  zcbor_tstr_put_term(zs, a->code, FLIGHTDB_AIRLINE_CODE) &&
		  zcbor_uint32_put(zs, L_NAME) &&
		  zcbor_tstr_put_term(zs, a->name, FLIGHTDB_AIRLINE_MAX) &&
		  zcbor_uint32_put(zs, L_CALLSIGN) &&
		  zcbor_tstr_put_term(zs, a->callsign, FLIGHTDB_CALLSIGN_MAX) &&
		  zcbor_uint32_put(zs, L_HUB) &&
		  zcbor_tstr_put_term(zs, a->hub, FLIGHTDB_CODE_LEN) &&
		  zcbor_uint32_put(zs, L_COUNTRY) &&
		  zcbor_tstr_put_term(zs, a->country, FLIGHTDB_COUNTRY_MAX) &&
		  zcbor_uint32_put(zs, L_ALLIANCE) &&
		  zcbor_tstr_put_term(zs, a->alliance, FLIGHTDB_ALLIANCE_MAX) &&
		  zcbor_uint32_put(zs, L_FOUNDED) && zcbor_uint32_put(zs, a->founded) &&
		  zcbor_map_end_encode(zs, AIRLINE_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int airline_cbor_decode(const uint8_t *buf, size_t len,
			struct flightdb_airline *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, L_CODE, out->code, sizeof(out->code)) &&
		  get_tstr(zs, L_NAME, out->name, sizeof(out->name)) &&
		  get_tstr(zs, L_CALLSIGN, out->callsign, sizeof(out->callsign)) &&
		  get_tstr(zs, L_HUB, out->hub, sizeof(out->hub)) &&
		  get_tstr(zs, L_COUNTRY, out->country, sizeof(out->country)) &&
		  get_tstr(zs, L_ALLIANCE, out->alliance, sizeof(out->alliance)) &&
		  get_u16(zs, L_FOUNDED, &out->founded) && zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int aircraft_cbor_encode(const struct flightdb_aircraft *a, uint8_t *buf,
			 size_t buf_sz, size_t *out_len)
{
	if (!fits(a->reg, FLIGHTDB_REG_MAX) || !fits(a->type, FLIGHTDB_EQUIP_MAX) ||
	    !fits(a->airline, FLIGHTDB_AIRLINE_CODE) || a->n_sectors > FLIGHTDB_SECTORS_MAX) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, AIRCRAFT_PAIRS) &&
		  zcbor_uint32_put(zs, K_REG) &&
		  zcbor_tstr_put_term(zs, a->reg, FLIGHTDB_REG_MAX) &&
		  zcbor_uint32_put(zs, K_TYPE) &&
		  zcbor_tstr_put_term(zs, a->type, FLIGHTDB_EQUIP_MAX) &&
		  zcbor_uint32_put(zs, K_AIRLINE) &&
		  zcbor_tstr_put_term(zs, a->airline, FLIGHTDB_AIRLINE_CODE) &&
		  put_cabins(zs, K_SEATS, a->seats) &&
		  zcbor_uint32_put(zs, K_BUILT) && zcbor_uint32_put(zs, a->built) &&
		  put_u16_list(zs, K_SECTORS, a->sectors, a->n_sectors, FLIGHTDB_SECTORS_MAX) &&
		  zcbor_map_end_encode(zs, AIRCRAFT_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int aircraft_cbor_decode(const uint8_t *buf, size_t len,
			 struct flightdb_aircraft *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, K_REG, out->reg, sizeof(out->reg)) &&
		  get_tstr(zs, K_TYPE, out->type, sizeof(out->type)) &&
		  get_tstr(zs, K_AIRLINE, out->airline, sizeof(out->airline)) &&
		  get_cabins(zs, K_SEATS, out->seats) &&
		  get_u16(zs, K_BUILT, &out->built) &&
		  get_u16_list(zs, K_SECTORS, out->sectors, &out->n_sectors,
			       FLIGHTDB_SECTORS_MAX) &&
		  zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int crew_cbor_encode(const struct flightdb_crew *c, uint8_t *buf,
		     size_t buf_sz, size_t *out_len)
{
	if (!fits(c->surname, FLIGHTDB_SURNAME_MAX) || !fits(c->given, FLIGHTDB_GIVEN_MAX) ||
	    !fits(c->airline, FLIGHTDB_AIRLINE_CODE) || !fits(c->base, FLIGHTDB_CODE_LEN) ||
	    c->rank >= FLIGHTDB_RANK__COUNT || c->n_duties > FLIGHTDB_DUTIES_MAX) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, CREW_PAIRS) &&
		  zcbor_uint32_put(zs, C_STAFF) && zcbor_uint32_put(zs, c->staff_no) &&
		  zcbor_uint32_put(zs, C_SURNAME) &&
		  zcbor_tstr_put_term(zs, c->surname, FLIGHTDB_SURNAME_MAX) &&
		  zcbor_uint32_put(zs, C_GIVEN) &&
		  zcbor_tstr_put_term(zs, c->given, FLIGHTDB_GIVEN_MAX) &&
		  zcbor_uint32_put(zs, C_RANK) && zcbor_uint32_put(zs, c->rank) &&
		  zcbor_uint32_put(zs, C_AIRLINE) &&
		  zcbor_tstr_put_term(zs, c->airline, FLIGHTDB_AIRLINE_CODE) &&
		  zcbor_uint32_put(zs, C_BASE) &&
		  zcbor_tstr_put_term(zs, c->base, FLIGHTDB_CODE_LEN) &&
		  put_u16_list(zs, C_DUTIES, c->duties, c->n_duties, FLIGHTDB_DUTIES_MAX) &&
		  zcbor_map_end_encode(zs, CREW_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int crew_cbor_decode(const uint8_t *buf, size_t len, struct flightdb_crew *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  zcbor_uint32_expect(zs, C_STAFF) && zcbor_uint32_decode(zs, &out->staff_no) &&
		  get_tstr(zs, C_SURNAME, out->surname, sizeof(out->surname)) &&
		  get_tstr(zs, C_GIVEN, out->given, sizeof(out->given)) &&
		  get_u8(zs, C_RANK, &out->rank) && out->rank < FLIGHTDB_RANK__COUNT &&
		  get_tstr(zs, C_AIRLINE, out->airline, sizeof(out->airline)) &&
		  get_tstr(zs, C_BASE, out->base, sizeof(out->base)) &&
		  get_u16_list(zs, C_DUTIES, out->duties, &out->n_duties, FLIGHTDB_DUTIES_MAX) &&
		  zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int bag_cbor_encode(const struct flightdb_bag *b, uint8_t *buf,
		    size_t buf_sz, size_t *out_len)
{
	if (!fits(b->tag, FLIGHTDB_TAG_MAX) || !fits(b->dest, FLIGHTDB_CODE_LEN) ||
	    b->status >= FLIGHTDB_BAG__COUNT) {
		return -EINVAL;
	}

	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, BAG_PAIRS) &&
		  zcbor_uint32_put(zs, G_TAG) &&
		  zcbor_tstr_put_term(zs, b->tag, FLIGHTDB_TAG_MAX) &&
		  zcbor_uint32_put(zs, G_BOOKING) && zcbor_uint32_put(zs, b->booking) &&
		  zcbor_uint32_put(zs, G_WEIGHT) && zcbor_uint32_put(zs, b->weight) &&
		  zcbor_uint32_put(zs, G_DEST) &&
		  zcbor_tstr_put_term(zs, b->dest, FLIGHTDB_CODE_LEN) &&
		  zcbor_uint32_put(zs, G_STATUS) && zcbor_uint32_put(zs, b->status) &&
		  zcbor_uint32_put(zs, G_PRIORITY) && zcbor_bool_put(zs, b->priority) &&
		  zcbor_map_end_encode(zs, BAG_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int bag_cbor_decode(const uint8_t *buf, size_t len, struct flightdb_bag *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	bool ok = zcbor_map_start_decode(zs) &&
		  get_tstr(zs, G_TAG, out->tag, sizeof(out->tag)) &&
		  get_u16(zs, G_BOOKING, &out->booking) &&
		  get_u8(zs, G_WEIGHT, &out->weight) &&
		  get_tstr(zs, G_DEST, out->dest, sizeof(out->dest)) &&
		  get_u8(zs, G_STATUS, &out->status) && out->status < FLIGHTDB_BAG__COUNT &&
		  zcbor_uint32_expect(zs, G_PRIORITY) && zcbor_bool_decode(zs, &out->priority) &&
		  zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}

int fldb_sb_cbor_encode(const struct fldb_superblock *sb, uint8_t *buf,
			size_t buf_sz, size_t *out_len)
{
	ZCBOR_STATE_E(zs, NBACKUPS, buf, buf_sz, 1);

	bool ok = zcbor_map_start_encode(zs, SB_PAIRS) &&
		  zcbor_uint32_put(zs, SB_VERSION) &&
		  zcbor_uint32_put(zs, FLDB_SB_VERSION) &&
		  zcbor_uint32_put(zs, SB_AIRPORTS_ROOT) &&
		  zcbor_uint64_put(zs, sb->airports_root) &&
		  zcbor_uint32_put(zs, SB_FLIGHTS_ROOT) &&
		  zcbor_uint64_put(zs, sb->flights_root) &&
		  zcbor_uint32_put(zs, SB_N_AIRPORTS) &&
		  zcbor_uint32_put(zs, sb->n_airports) &&
		  zcbor_uint32_put(zs, SB_N_FLIGHTS) &&
		  zcbor_uint32_put(zs, sb->n_flights) &&
		  zcbor_uint32_put(zs, SB_PASSENGERS_ROOT) &&
		  zcbor_uint64_put(zs, sb->passengers_root) &&
		  zcbor_uint32_put(zs, SB_BOOKINGS_ROOT) &&
		  zcbor_uint64_put(zs, sb->bookings_root) &&
		  zcbor_uint32_put(zs, SB_N_PASSENGERS) &&
		  zcbor_uint32_put(zs, sb->n_passengers) &&
		  zcbor_uint32_put(zs, SB_N_BOOKINGS) &&
		  zcbor_uint32_put(zs, sb->n_bookings) &&
		  zcbor_uint32_put(zs, SB_AIRLINES_ROOT) &&
		  zcbor_uint64_put(zs, sb->airlines_root) &&
		  zcbor_uint32_put(zs, SB_AIRCRAFT_ROOT) &&
		  zcbor_uint64_put(zs, sb->aircraft_root) &&
		  zcbor_uint32_put(zs, SB_CREW_ROOT) &&
		  zcbor_uint64_put(zs, sb->crew_root) &&
		  zcbor_uint32_put(zs, SB_BAGS_ROOT) &&
		  zcbor_uint64_put(zs, sb->bags_root) &&
		  zcbor_uint32_put(zs, SB_N_AIRLINES) &&
		  zcbor_uint32_put(zs, sb->n_airlines) &&
		  zcbor_uint32_put(zs, SB_N_AIRCRAFT) &&
		  zcbor_uint32_put(zs, sb->n_aircraft) &&
		  zcbor_uint32_put(zs, SB_N_CREW) &&
		  zcbor_uint32_put(zs, sb->n_crew) &&
		  zcbor_uint32_put(zs, SB_N_BAGS) &&
		  zcbor_uint32_put(zs, sb->n_bags) &&
		  zcbor_map_end_encode(zs, SB_PAIRS);

	if (!ok) {
		return -ENOMEM;
	}
	*out_len = (size_t)(zs->payload - buf);
	return 0;
}

int fldb_sb_cbor_decode(const uint8_t *buf, size_t len,
			struct fldb_superblock *out)
{
	memset(out, 0, sizeof(*out));

	ZCBOR_STATE_D(zs, NBACKUPS, buf, len, 1, 0);

	if (!zcbor_map_start_decode(zs) || !zcbor_uint32_expect(zs, SB_VERSION) ||
	    !zcbor_uint32_decode(zs, &out->version)) {
		return -EILSEQ;
	}
	if (out->version < FLDB_SB_VERSION_V1 || out->version > FLDB_SB_VERSION) {
		return -ENOTSUP;
	}

	bool ok = zcbor_uint32_expect(zs, SB_AIRPORTS_ROOT) &&
		  zcbor_uint64_decode(zs, &out->airports_root) &&
		  zcbor_uint32_expect(zs, SB_FLIGHTS_ROOT) &&
		  zcbor_uint64_decode(zs, &out->flights_root) &&
		  zcbor_uint32_expect(zs, SB_N_AIRPORTS) &&
		  zcbor_uint32_decode(zs, &out->n_airports) &&
		  zcbor_uint32_expect(zs, SB_N_FLIGHTS) &&
		  zcbor_uint32_decode(zs, &out->n_flights);

	if (ok && out->version >= FLDB_SB_VERSION_V2) {
		ok = zcbor_uint32_expect(zs, SB_PASSENGERS_ROOT) &&
		     zcbor_uint64_decode(zs, &out->passengers_root) &&
		     zcbor_uint32_expect(zs, SB_BOOKINGS_ROOT) &&
		     zcbor_uint64_decode(zs, &out->bookings_root) &&
		     zcbor_uint32_expect(zs, SB_N_PASSENGERS) &&
		     zcbor_uint32_decode(zs, &out->n_passengers) &&
		     zcbor_uint32_expect(zs, SB_N_BOOKINGS) &&
		     zcbor_uint32_decode(zs, &out->n_bookings);
	}
	if (ok && out->version >= FLDB_SB_VERSION) {
		ok = zcbor_uint32_expect(zs, SB_AIRLINES_ROOT) &&
		     zcbor_uint64_decode(zs, &out->airlines_root) &&
		     zcbor_uint32_expect(zs, SB_AIRCRAFT_ROOT) &&
		     zcbor_uint64_decode(zs, &out->aircraft_root) &&
		     zcbor_uint32_expect(zs, SB_CREW_ROOT) &&
		     zcbor_uint64_decode(zs, &out->crew_root) &&
		     zcbor_uint32_expect(zs, SB_BAGS_ROOT) &&
		     zcbor_uint64_decode(zs, &out->bags_root) &&
		     zcbor_uint32_expect(zs, SB_N_AIRLINES) &&
		     zcbor_uint32_decode(zs, &out->n_airlines) &&
		     zcbor_uint32_expect(zs, SB_N_AIRCRAFT) &&
		     zcbor_uint32_decode(zs, &out->n_aircraft) &&
		     zcbor_uint32_expect(zs, SB_N_CREW) &&
		     zcbor_uint32_decode(zs, &out->n_crew) &&
		     zcbor_uint32_expect(zs, SB_N_BAGS) &&
		     zcbor_uint32_decode(zs, &out->n_bags);
	}
	return (ok && zcbor_map_end_decode(zs)) ? 0 : -EILSEQ;
}
