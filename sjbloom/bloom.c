/*-------------------------------------------------------------------------
 *
 * bloom.c
 *
 * The pure Bloom filter logic of the sjbloom extension.
 *
 * A Bloom filter is an array of m bits, all 0 at the start. To add a key
 * we compute k bit positions from the key and set those k bits to 1. To
 * test a key we compute the same k positions: if any of those bits is 0
 * the key was certainly never added; if all are 1 the key was probably
 * added (it may be a "false positive").
 *
 * This file only works on a plain array of bytes. It knows nothing about
 * bytea values or SQL; sjbloom.c handles that part.
 *
 * Bit position p lives in byte p / 8, at bit p % 8 inside that byte.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/fmgrprotos.h"

#include "sjb_filter.h"


/*
 * compute_two_hashes
 *
 * What it does: hashes one key with PostgreSQL's own int4 hash function
 * and splits the 64-bit result into two 32-bit numbers h1 and h2.
 *
 * Inputs:  key  - the join key to hash
 *          seed - the hash seed (taken from the filter header)
 * Output:  *h1 = low 32 bits of the hash
 *          *h2 = high 32 bits of the hash, with the lowest bit forced to 1
 *
 * Why it exists: double hashing (see bit_position below) needs two hash
 * values. Forcing h2 to be odd makes sure h2 is never 0, so the k
 * positions do not all collapse onto the same bit.
 *
 * We use hashint4extended (the same function PostgreSQL uses for hash
 * indexes on int4) so that site A and site B always get the same hash for
 * the same key, on any machine.
 */
static void
compute_two_hashes(int32 key, uint64 seed, uint32 *h1, uint32 *h2)
{
	Datum		hash_datum;
	uint64		hash;

	/*
	 * DirectFunctionCall2 calls a PostgreSQL SQL-callable C function
	 * directly from C with two arguments. Arguments and the result are
	 * passed as "Datum" values: Int32GetDatum and Int64GetDatum wrap our C
	 * numbers into Datums, and DatumGetUInt64 unwraps the result.
	 */
	hash_datum = DirectFunctionCall2(hashint4extended,
									 Int32GetDatum(key),
									 Int64GetDatum((int64) seed));
	hash = DatumGetUInt64(hash_datum);

	*h1 = (uint32) (hash & 0xFFFFFFFF);
	*h2 = ((uint32) (hash >> 32)) | 1;
}

/*
 * bit_position
 *
 * What it does: returns the i-th bit position for a key, using
 * double hashing:  position_i = (h1 + i * h2) mod m
 *
 * Inputs:  h1, h2 - the two hash values from compute_two_hashes
 *          i      - which position we want (0 .. k-1)
 *          m      - the number of bits in the filter
 * Output:  a bit position between 0 and m-1
 *
 * Why it exists: hashing a key k separate times would be slow. Double
 * hashing gives k well-spread positions from just one hash call.
 * All arithmetic is done in uint64, so h1 + i * h2 cannot overflow
 * (h1 and h2 are below 2^32 and i is at most 31).
 */
static uint64
bit_position(uint32 h1, uint32 h2, int i, uint64 m)
{
	uint64		combined;

	combined = (uint64) h1 + (uint64) i * (uint64) h2;
	return combined % m;
}

/*
 * set_bit
 *
 * What it does: sets bit number "position" to 1.
 * Inputs:  bits     - the Bloom filter bit array
 *          position - the bit to set
 * Output:  none (the bit array is changed)
 * Why it exists: keeps the "byte p/8, bit p%8" rule in one place.
 */
static void
set_bit(uint8 *bits, uint64 position)
{
	uint64		byte_index = position / 8;
	int			bit_in_byte = (int) (position % 8);

	bits[byte_index] = bits[byte_index] | (uint8) (1 << bit_in_byte);
}

/*
 * bit_is_set
 *
 * What it does: tells whether bit number "position" is 1.
 * Inputs:  bits     - the Bloom filter bit array
 *          position - the bit to look at
 * Output:  true if the bit is 1, false if it is 0
 * Why it exists: the reading counterpart of set_bit.
 */
static bool
bit_is_set(const uint8 *bits, uint64 position)
{
	uint64		byte_index = position / 8;
	int			bit_in_byte = (int) (position % 8);

	if ((bits[byte_index] & (uint8) (1 << bit_in_byte)) != 0)
		return true;
	else
		return false;
}

/*
 * sjb_bloom_add
 *
 * What it does: adds one key to a Bloom filter by setting its k bits.
 *
 * Inputs:  bits - the bit array, (m + 7) / 8 bytes long
 *          m    - the number of bits in the filter (at least 1)
 *          k    - how many bits to set per key
 *          seed - the hash seed
 *          key  - the key to add
 * Output:  none (the bit array is changed)
 *
 * Why it exists: called by sjb_build for every join key at site A.
 */
void
sjb_bloom_add(uint8 *bits, uint64 m, int k, uint64 seed, int32 key)
{
	uint32		h1;
	uint32		h2;
	int			i;

	compute_two_hashes(key, seed, &h1, &h2);

	for (i = 0; i < k; i++)
	{
		uint64		position = bit_position(h1, h2, i, m);

		set_bit(bits, position);
	}
}

/*
 * sjb_bloom_test
 *
 * What it does: checks whether a key may be in the Bloom filter.
 *
 * Inputs:  the same as sjb_bloom_add (the bits are only read)
 * Output:  false if the key was certainly never added;
 *          true if the key was probably added (could be a false positive)
 *
 * Why it exists: called by sjb_contains for every row of S at site B.
 * It stops at the first 0 bit, because one 0 bit already proves the key
 * is not in the filter.
 */
bool
sjb_bloom_test(const uint8 *bits, uint64 m, int k, uint64 seed, int32 key)
{
	uint32		h1;
	uint32		h2;
	int			i;

	compute_two_hashes(key, seed, &h1, &h2);

	for (i = 0; i < k; i++)
	{
		uint64		position = bit_position(h1, h2, i, m);

		if (!bit_is_set(bits, position))
			return false;
	}

	return true;
}


/*
 * ---------------------------------------------------------------------
 * TEMPORARY (branch fp-investigation): "enhanced double hashing", the
 * method PostgreSQL's own src/backend/lib/bloomfilter.c uses. Only the way
 * the k positions are computed differs from the functions above; the hash
 * (compute_two_hashes), the bit layout and set_bit / bit_is_set are the
 * same. Used by sjb_build_v2 / sjb_contains_v2 to compare false-positive
 * rates. Not part of the fixed design: do not use outside the experiment.
 * ---------------------------------------------------------------------
 */

/*
 * compute_positions_v2
 *
 * What it does: fills positions[0 .. k-1] using enhanced double hashing,
 * following PostgreSQL's k_hashes():
 *     x = h1 mod m,  y = h2 mod m
 *     position_0 = x
 *     for i = 1 .. k-1:   x = (x + y) mod m
 *                         y = (y + i) mod m
 *                         position_i = x
 * Because y grows by 1, 2, 3, ... the step between positions changes
 * every time, so the positions no longer form an evenly spaced row
 * (h1, h1 + h2, h1 + 2*h2, ...) like classic double hashing does.
 *
 * Inputs:  h1, h2    - the two hash values from compute_two_hashes
 *          k         - number of positions (at most SJB_MAX_K)
 *          m         - number of bits in the filter
 * Output:  positions - array of at least k entries, filled in
 *
 * Why it exists: to test whether classic double hashing causes the extra
 * false positives we measured for small filters.
 * (PostgreSQL uses "& (m - 1)" because its m is always a power of two;
 * our m can be any number, so we use "% m". All values stay below m, so
 * x + y and y + i cannot overflow a uint64.)
 */
static void
compute_positions_v2(uint32 h1, uint32 h2, int k, uint64 m, uint64 *positions)
{
	uint64		x = (uint64) h1 % m;
	uint64		y = (uint64) h2 % m;
	int			i;

	positions[0] = x;
	for (i = 1; i < k; i++)
	{
		x = (x + y) % m;
		y = (y + (uint64) i) % m;
		positions[i] = x;
	}
}

/*
 * sjb_bloom_add_v2
 *
 * What it does: the same as sjb_bloom_add, but with enhanced double
 * hashing (compute_positions_v2).
 * Inputs / output: the same as sjb_bloom_add.
 * Why it exists: TEMPORARY, for the false-positive investigation.
 */
void
sjb_bloom_add_v2(uint8 *bits, uint64 m, int k, uint64 seed, int32 key)
{
	uint32		h1;
	uint32		h2;
	uint64		positions[SJB_MAX_K];
	int			i;

	compute_two_hashes(key, seed, &h1, &h2);
	compute_positions_v2(h1, h2, k, m, positions);

	for (i = 0; i < k; i++)
		set_bit(bits, positions[i]);
}

/*
 * sjb_bloom_test_v2
 *
 * What it does: the same as sjb_bloom_test, but with enhanced double
 * hashing (compute_positions_v2).
 * Inputs / output: the same as sjb_bloom_test.
 * Why it exists: TEMPORARY, for the false-positive investigation.
 */
bool
sjb_bloom_test_v2(const uint8 *bits, uint64 m, int k, uint64 seed, int32 key)
{
	uint32		h1;
	uint32		h2;
	uint64		positions[SJB_MAX_K];
	int			i;

	compute_two_hashes(key, seed, &h1, &h2);
	compute_positions_v2(h1, h2, k, m, positions);

	for (i = 0; i < k; i++)
	{
		if (!bit_is_set(bits, positions[i]))
			return false;
	}

	return true;
}
