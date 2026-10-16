/*-------------------------------------------------------------------------
 *
 * sjbloom.c
 *
 * The SQL-callable functions of the sjbloom extension:
 *
 *   sjb_build(keys int4[], m int8, k int4) RETURNS bytea
 *       Site A: build a Bloom filter from the join keys.
 *   sjb_contains(filter bytea, key int4) RETURNS bool
 *       Site B: does this key pass the filter?
 *   sjb_info(filter bytea) RETURNS text
 *       Debugging: describe a filter in one line of text.
 *
 * This file turns SQL values (arrays, bytea) into plain C data and back.
 * The Bloom filter logic itself is in bloom.c.
 *
 * Filter layout (see sjb_filter.h):
 *   [varlena length header][24-byte SjbHeader][payload]
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "sjb_filter.h"

/*
 * PG_MODULE_MAGIC puts a small "magic block" into sjbloom.so. The server
 * checks it when loading the library, to refuse a library that was built
 * for a different PostgreSQL major version.
 */
PG_MODULE_MAGIC;


/*
 * check_bloom_parameters
 *
 * What it does: raises an error if m or k is outside the allowed range.
 *
 * Inputs:  m - the number of bits in the Bloom filter
 *          k - the number of hash positions per key
 * Output:  none; returns normally only if both values are valid
 *
 * Why it exists: sjb_build uses it to check the user's arguments, and
 * sjb_contains / sjb_info use it to check the values stored in a filter
 * header. A value of m = 0 would cause a division by zero, and a huge m
 * would try to allocate too much memory.
 */
static void
check_bloom_parameters(int64 m, int32 k)
{
	/*
	 * ereport(ERROR, ...) is how PostgreSQL raises an SQL error. It stops
	 * this function, aborts the current query and shows the message to the
	 * user. errcode sets the SQLSTATE error code and errmsg the text.
	 */
	if (m < SJB_MIN_BITS || m > SJB_MAX_BITS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sjbloom: m must be between %d and %d bits, got %lld",
						SJB_MIN_BITS, SJB_MAX_BITS, (long long) m)));

	if (k < SJB_MIN_K || k > SJB_MAX_K)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sjbloom: k must be between %d and %d, got %d",
						SJB_MIN_K, SJB_MAX_K, k)));
}

/*
 * bloom_payload_bytes
 *
 * What it does: returns how many bytes are needed to hold m bits,
 * i.e. ceil(m / 8).
 * Input:  m - the number of bits
 * Output: the number of bytes
 * Why it exists: the same rule is needed when building and when checking
 * a filter, so it lives in one place.
 */
static uint64
bloom_payload_bytes(uint64 m)
{
	return (m + 7) / 8;
}

/*
 * read_header
 *
 * What it does: copies the 24-byte header out of a filter and checks it.
 *
 * Inputs:  filter_data - pointer to the first byte after the varlena
 *                        length header (the start of SjbHeader)
 *          filter_size - number of bytes at filter_data
 * Output:  *header is filled in
 *
 * Why it exists: every function that reads a filter must first make sure
 * the bytes really are a filter of our version. We use memcpy because the
 * bytes inside a bytea may not be aligned for 8-byte reads.
 */
static void
read_header(const char *filter_data, Size filter_size, SjbHeader *header)
{
	if (filter_size < sizeof(SjbHeader))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: filter is too small (%zu bytes, need at least %zu)",
						filter_size, sizeof(SjbHeader))));

	memcpy(header, filter_data, sizeof(SjbHeader));

	if (header->version != SJB_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: unknown filter version %d (expected %d)",
						header->version, SJB_VERSION)));
}

/*
 * check_bloom_filter
 *
 * What it does: checks that a BLOOM filter's header values are valid and
 * that its payload has exactly the right size.
 *
 * Inputs:  header       - the header copied out by read_header
 *          payload_size - the number of bytes after the header
 * Output:  none; returns normally only if the filter is consistent
 *
 * Why it exists: a damaged filter must give an error, never make us read
 * outside the payload.
 */
static void
check_bloom_filter(const SjbHeader *header, Size payload_size)
{
	check_bloom_parameters((int64) header->m, (int32) header->k);

	if (payload_size != bloom_payload_bytes(header->m))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: Bloom payload is %zu bytes, expected %llu",
						payload_size,
						(unsigned long long) bloom_payload_bytes(header->m))));
}

/*
 * error_mode_not_supported
 *
 * What it does: raises an error for a filter mode we cannot handle yet.
 * Input:  mode - the mode number from the header
 * Output: none (never returns)
 * Why it exists: EXACT and PASS are added in Batch 5. Until then, and for
 * any unknown mode number, we give a clear error.
 */
static void
error_mode_not_supported(int mode)
{
	if (mode == SJB_MODE_EXACT || mode == SJB_MODE_PASS)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sjbloom: filter mode %d is not supported yet", mode)));
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: unknown filter mode %d", mode)));
}

/*
 * count_set_bits
 *
 * What it does: counts how many bits are 1 in a byte array.
 * Inputs:  bits      - the byte array
 *          num_bytes - its length in bytes
 * Output:  the number of 1 bits
 * Why it exists: sjb_info reports how full the Bloom filter is. A filter
 * with about 50% of its bits set is well sized; much more means too many
 * false positives.
 */
static uint64
count_set_bits(const uint8 *bits, uint64 num_bytes)
{
	uint64		count = 0;
	uint64		byte_index;
	int			bit_in_byte;

	for (byte_index = 0; byte_index < num_bytes; byte_index++)
	{
		for (bit_in_byte = 0; bit_in_byte < 8; bit_in_byte++)
		{
			if ((bits[byte_index] & (uint8) (1 << bit_in_byte)) != 0)
				count = count + 1;
		}
	}
	return count;
}


/*
 * PG_FUNCTION_INFO_V1 tells PostgreSQL that the C function with this name
 * uses the standard ("version 1") calling convention for SQL functions:
 * it is declared as  Datum name(PG_FUNCTION_ARGS)  and reads its arguments
 * with the PG_GETARG_... macros.
 */
PG_FUNCTION_INFO_V1(sjb_build);

/*
 * sjb_build(keys int4[], m int8, k int4) RETURNS bytea
 *
 * What it does: builds a Bloom filter with m bits and k positions per key
 * and adds every non-NULL key of the array to it.
 *
 * Inputs:  keys - array of join keys (NULL elements are skipped)
 *          m    - number of bits (8 .. 8*64*1024*1024)
 *          k    - number of positions per key (1 .. 32)
 * Output:  a bytea: 24-byte header (mode BLOOM) followed by ceil(m/8)
 *          bytes of bits. header.nkeys = number of keys added.
 *
 * Why it exists: this runs at site A; the result is sent to site B.
 */
Datum
sjb_build(PG_FUNCTION_ARGS)
{
	/*
	 * PG_GETARG_ARRAYTYPE_P, PG_GETARG_INT64 and PG_GETARG_INT32 read
	 * argument number 0, 1, 2 of the SQL call as C values.
	 */
	ArrayType  *keys_array = PG_GETARG_ARRAYTYPE_P(0);
	int64		m = PG_GETARG_INT64(1);
	int32		k = PG_GETARG_INT32(2);
	Datum	   *elements;
	bool	   *nulls;
	int			num_elements;
	uint64		payload_bytes;
	Size		total_size;
	bytea	   *result;
	uint8	   *bits;
	SjbHeader	header;
	uint32		num_keys_added = 0;
	int			i;

	check_bloom_parameters(m, k);

	/*
	 * deconstruct_array_builtin unpacks an SQL array of a built-in type
	 * (here INT4OID = int4) into two C arrays: elements[i] holds the value
	 * and nulls[i] says whether that element is NULL.
	 */
	deconstruct_array_builtin(keys_array, INT4OID,
							  &elements, &nulls, &num_elements);

	/*
	 * A bytea is a "varlena": a 4-byte length header (VARHDRSZ bytes)
	 * followed by the data. palloc0 allocates memory in the current
	 * PostgreSQL memory context and fills it with zeros, so all Bloom bits
	 * start at 0. (palloc memory is freed automatically at the end of the
	 * query, so we never call free.) SET_VARSIZE writes the total size
	 * into the length header.
	 */
	payload_bytes = bloom_payload_bytes((uint64) m);
	total_size = VARHDRSZ + sizeof(SjbHeader) + payload_bytes;
	result = (bytea *) palloc0(total_size);
	SET_VARSIZE(result, total_size);

	/*
	 * VARDATA gives a pointer to the first byte after the length header.
	 * The header goes there; the Bloom bits start sizeof(SjbHeader) = 24
	 * bytes later.
	 */
	bits = (uint8 *) VARDATA(result) + sizeof(SjbHeader);

	for (i = 0; i < num_elements; i++)
	{
		int32		key;

		if (nulls[i])
			continue;			/* a NULL key never matches in a join */

		/* DatumGetInt32 unwraps a Datum back into a C int32. */
		key = DatumGetInt32(elements[i]);
		sjb_bloom_add(bits, (uint64) m, k, SJB_SEED, key);
		num_keys_added = num_keys_added + 1;
	}

	/* Fill in the header and copy it to the start of the data. */
	memset(&header, 0, sizeof(header));
	header.version = SJB_VERSION;
	header.mode = SJB_MODE_BLOOM;
	header.k = (uint16) k;
	header.nkeys = num_keys_added;
	header.m = (uint64) m;
	header.seed = SJB_SEED;
	memcpy(VARDATA(result), &header, sizeof(SjbHeader));

	/* PG_RETURN_BYTEA_P returns the bytea to SQL. */
	PG_RETURN_BYTEA_P(result);
}


PG_FUNCTION_INFO_V1(sjb_contains);

/*
 * sjb_contains(filter bytea, key int4) RETURNS bool
 *
 * What it does: tests whether a key passes the filter.
 *
 * Inputs:  filter - a filter made by sjb_build
 *          key    - one join key from table S
 * Output:  true if the key passes (it may be a Bloom false positive),
 *          false if the key is certainly not one of the filter's keys.
 *          (Because the SQL function is STRICT, a NULL key gives NULL
 *          without this C code ever running.)
 *
 * Why it exists: this runs at site B inside the WHERE clause, so only
 * rows that pass are sent back to site A.
 */
Datum
sjb_contains(PG_FUNCTION_ARGS)
{
	/*
	 * PG_GETARG_BYTEA_PP gets the bytea argument, decompressing it or
	 * fetching it from TOAST storage if needed. It may still have a short
	 * 1-byte length header, so we must use VARDATA_ANY (pointer to the
	 * data) and VARSIZE_ANY_EXHDR (data size without the length header),
	 * which work with both header sizes.
	 */
	bytea	   *filter = PG_GETARG_BYTEA_PP(0);
	int32		key = PG_GETARG_INT32(1);
	const char *filter_data = VARDATA_ANY(filter);
	Size		filter_size = VARSIZE_ANY_EXHDR(filter);
	SjbHeader	header;
	Size		payload_size;
	const uint8 *bits;
	bool		passes;

	read_header(filter_data, filter_size, &header);

	if (header.mode != SJB_MODE_BLOOM)
		error_mode_not_supported(header.mode);

	payload_size = filter_size - sizeof(SjbHeader);
	check_bloom_filter(&header, payload_size);

	/* The Bloom bits start right after the 24-byte header. */
	bits = (const uint8 *) filter_data + sizeof(SjbHeader);
	passes = sjb_bloom_test(bits, header.m, header.k, header.seed, key);

	/* PG_RETURN_BOOL returns a C bool to SQL as a boolean. */
	PG_RETURN_BOOL(passes);
}


PG_FUNCTION_INFO_V1(sjb_info);

/*
 * sjb_info(filter bytea) RETURNS text
 *
 * What it does: describes a filter in one line, for example
 *   mode=bloom nkeys=10000 m=220000 k=15 bits_set=48.10% bytes=27524
 * "bytes" is the header plus the payload (without the varlena length).
 *
 * Input:  filter - a filter made by sjb_build
 * Output: the description as text
 *
 * Why it exists: lets us look inside a filter while testing and tuning.
 */
Datum
sjb_info(PG_FUNCTION_ARGS)
{
	bytea	   *filter = PG_GETARG_BYTEA_PP(0);
	const char *filter_data = VARDATA_ANY(filter);
	Size		filter_size = VARSIZE_ANY_EXHDR(filter);
	SjbHeader	header;
	Size		payload_size;
	const uint8 *bits;
	uint64		num_bits_set;
	double		percent_set;
	char	   *description;

	read_header(filter_data, filter_size, &header);

	if (header.mode != SJB_MODE_BLOOM)
		error_mode_not_supported(header.mode);

	payload_size = filter_size - sizeof(SjbHeader);
	check_bloom_filter(&header, payload_size);

	bits = (const uint8 *) filter_data + sizeof(SjbHeader);
	num_bits_set = count_set_bits(bits, payload_size);
	percent_set = 100.0 * (double) num_bits_set / (double) header.m;

	/* psprintf is like sprintf, but returns a new palloc'd string. */
	description = psprintf("mode=bloom nkeys=%u m=%llu k=%d bits_set=%.2f%% bytes=%zu",
						   header.nkeys,
						   (unsigned long long) header.m,
						   (int) header.k,
						   percent_set,
						   filter_size);

	/*
	 * cstring_to_text turns a C string into an SQL text value, and
	 * PG_RETURN_TEXT_P returns it.
	 */
	PG_RETURN_TEXT_P(cstring_to_text(description));
}


/*
 * ---------------------------------------------------------------------
 * TEMPORARY (branch fp-investigation): sjb_build_v2 / sjb_contains_v2.
 *
 * Exact copies of sjb_build / sjb_contains, except that they call
 * sjb_bloom_add_v2 / sjb_bloom_test_v2 (enhanced double hashing, see
 * bloom.c). The filter format is the same, but a v2 filter must only be
 * tested with sjb_contains_v2, because the bit positions differ.
 *
 * They are NOT in sjbloom--1.0.sql: queries/fp_v1_v2.sql declares them as
 * temporary functions (schema pg_temp) that disappear when the session
 * ends. Remove this block when the investigation is finished.
 * ---------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(sjb_build_v2);

/*
 * sjb_build_v2(keys int4[], m int8, k int4) RETURNS bytea
 *
 * What it does: the same as sjb_build, with enhanced double hashing.
 * Inputs / output: the same as sjb_build.
 * Why it exists: TEMPORARY, for the false-positive investigation.
 */
Datum
sjb_build_v2(PG_FUNCTION_ARGS)
{
	ArrayType  *keys_array = PG_GETARG_ARRAYTYPE_P(0);
	int64		m = PG_GETARG_INT64(1);
	int32		k = PG_GETARG_INT32(2);
	Datum	   *elements;
	bool	   *nulls;
	int			num_elements;
	uint64		payload_bytes;
	Size		total_size;
	bytea	   *result;
	uint8	   *bits;
	SjbHeader	header;
	uint32		num_keys_added = 0;
	int			i;

	check_bloom_parameters(m, k);

	deconstruct_array_builtin(keys_array, INT4OID,
							  &elements, &nulls, &num_elements);

	payload_bytes = bloom_payload_bytes((uint64) m);
	total_size = VARHDRSZ + sizeof(SjbHeader) + payload_bytes;
	result = (bytea *) palloc0(total_size);
	SET_VARSIZE(result, total_size);

	bits = (uint8 *) VARDATA(result) + sizeof(SjbHeader);

	for (i = 0; i < num_elements; i++)
	{
		int32		key;

		if (nulls[i])
			continue;

		key = DatumGetInt32(elements[i]);
		sjb_bloom_add_v2(bits, (uint64) m, k, SJB_SEED, key);
		num_keys_added = num_keys_added + 1;
	}

	memset(&header, 0, sizeof(header));
	header.version = SJB_VERSION;
	header.mode = SJB_MODE_BLOOM;
	header.k = (uint16) k;
	header.nkeys = num_keys_added;
	header.m = (uint64) m;
	header.seed = SJB_SEED;
	memcpy(VARDATA(result), &header, sizeof(SjbHeader));

	PG_RETURN_BYTEA_P(result);
}


PG_FUNCTION_INFO_V1(sjb_contains_v2);

/*
 * sjb_contains_v2(filter bytea, key int4) RETURNS bool
 *
 * What it does: the same as sjb_contains, with enhanced double hashing.
 * Inputs / output: the same as sjb_contains.
 * Why it exists: TEMPORARY, for the false-positive investigation.
 */
Datum
sjb_contains_v2(PG_FUNCTION_ARGS)
{
	bytea	   *filter = PG_GETARG_BYTEA_PP(0);
	int32		key = PG_GETARG_INT32(1);
	const char *filter_data = VARDATA_ANY(filter);
	Size		filter_size = VARSIZE_ANY_EXHDR(filter);
	SjbHeader	header;
	Size		payload_size;
	const uint8 *bits;
	bool		passes;

	read_header(filter_data, filter_size, &header);

	if (header.mode != SJB_MODE_BLOOM)
		error_mode_not_supported(header.mode);

	payload_size = filter_size - sizeof(SjbHeader);
	check_bloom_filter(&header, payload_size);

	bits = (const uint8 *) filter_data + sizeof(SjbHeader);
	passes = sjb_bloom_test_v2(bits, header.m, header.k, header.seed, key);

	PG_RETURN_BOOL(passes);
}
