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
	SB_VERSION = 1, SB_AIRPORTS_ROOT, SB_FLIGHTS_ROOT, SB_N_AIRPORTS,
	SB_N_FLIGHTS,
};
#define SB_PAIRS 5

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
	if (out->version != FLDB_SB_VERSION) {
		return -ENOTSUP;
	}

	bool ok = zcbor_uint32_expect(zs, SB_AIRPORTS_ROOT) &&
		  zcbor_uint64_decode(zs, &out->airports_root) &&
		  zcbor_uint32_expect(zs, SB_FLIGHTS_ROOT) &&
		  zcbor_uint64_decode(zs, &out->flights_root) &&
		  zcbor_uint32_expect(zs, SB_N_AIRPORTS) &&
		  zcbor_uint32_decode(zs, &out->n_airports) &&
		  zcbor_uint32_expect(zs, SB_N_FLIGHTS) &&
		  zcbor_uint32_decode(zs, &out->n_flights) && zcbor_map_end_decode(zs);

	return ok ? 0 : -EILSEQ;
}
