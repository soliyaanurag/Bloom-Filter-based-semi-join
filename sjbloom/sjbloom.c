/*-------------------------------------------------------------------------
 *
 * sjbloom.c
 *
 * The SQL-callable functions of the sjbloom extension:
 *
 *   sjb_build(keys int4[], m int8, k int4) RETURNS bytea
 *       Site A: build a Bloom filter with a given size.
 *   sjb_build_auto(keys int4[], s_rows, rows_per_key, row_width,
 *                  network_mbit, t_check_ns,
 *                  max_filter_bytes, wire_factor,
 *                  per_message_ms, per_row_us, t_build_us,
 *                  fetch_size) RETURNS bytea
 *       Site A: let the cost model pick the mode (BLOOM, EXACT or PASS)
 *       and the Bloom size, then build that filter.            (Batch 5)
 *   sjb_contains(filter bytea, key int4) RETURNS bool
 *       Site B: does this key pass the filter? Works for all three modes.
 *   sjb_info(filter bytea) RETURNS text
 *       Debugging: describe a filter in one line of text.
 *   sjb_choice_report(n, s_rows, rows_per_key, row_width, network_mbit,
 *                     t_check_ns, t_lookup_us, per_message_ms,
 *                     per_row_us, t_build_us, fetch_size) RETURNS text
 *       A readable report of the cost model's estimates.       (Batch 5)
 *   sjb_plan_choice(same arguments) RETURNS text
 *       'index' or 'filter': which path the cost model picks.  (Batch 5)
 *
 * This file turns SQL values (arrays, bytea, float8) into plain C data and
 * back. The Bloom filter logic is in bloom.c, the cost model in
 * costmodel.c.
 *
 * Filter layout (see sjb_filter.h):
 *   [varlena length header][24-byte SjbHeader][payload]
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <stdlib.h>

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"

#include "sjb_filter.h"
#include "costmodel.h"

/*
 * PG_MODULE_MAGIC puts a small "magic block" into sjbloom.so. The server
 * checks it when loading the library, to refuse a library that was built
 * for a different PostgreSQL major version.
 */
PG_MODULE_MAGIC;


/* ------------------------------------------------------------------------
 * Part 1: checking a filter
 * ------------------------------------------------------------------------
 */

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

	/*
	 * A filter of another version was built with a different algorithm
	 * (for example version 1 = classic double hashing). Reading it with our
	 * bit positions could drop rows that really match, so we refuse it.
	 * This happens when site A and site B run different builds of sjbloom.
	 * errhint adds a "HINT:" line that tells the user how to fix it.
	 */
	if (header->version != SJB_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: filter has version %d, but this sjbloom understands only version %d",
						header->version, SJB_VERSION),
				 errhint("Site A and site B must run the same sjbloom build. "
						 "Install it on both sites and rebuild the filter.")));
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
 * check_exact_filter
 *
 * What it does: checks that an EXACT filter's payload holds exactly
 * nkeys int4 keys (4 bytes each).
 *
 * Inputs:  header       - the header copied out by read_header
 *          payload_size - the number of bytes after the header
 * Output:  none; returns normally only if the sizes agree
 *
 * Why it exists: the binary search in sjb_contains trusts nkeys, so a
 * damaged filter must give an error before we read past the payload.
 */
static void
check_exact_filter(const SjbHeader *header, Size payload_size)
{
	Size		expected_size = (Size) header->nkeys * sizeof(int32);

	if (payload_size != expected_size)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: EXACT payload is %zu bytes, expected %zu for %u keys",
						payload_size, expected_size, header->nkeys)));
}

/*
 * check_pass_filter
 *
 * What it does: checks that a PASS filter has no payload.
 * Input:  payload_size - the number of bytes after the header
 * Output: none; returns normally only if the payload is empty
 * Why it exists: a PASS filter is only a header; anything else means the
 * bytes were damaged.
 */
static void
check_pass_filter(Size payload_size)
{
	if (payload_size != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("sjbloom: PASS filter must have no payload, found %zu bytes",
						payload_size)));
}

/*
 * error_unknown_mode
 *
 * What it does: raises an error for a mode number we do not know.
 * Input:  mode - the mode number from the header
 * Output: none (never returns)
 * Why it exists: only modes 1, 2 and 3 exist; anything else is damage.
 */
static void
error_unknown_mode(int mode)
{
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


/* ------------------------------------------------------------------------
 * Part 2: the EXACT mode payload (sorted, unique int4 keys)
 * ------------------------------------------------------------------------
 */

/*
 * read_exact_key
 *
 * What it does: reads key number "index" out of an EXACT payload.
 * Inputs:  payload - the first byte after the header
 *          index    - which key (0 = the first one)
 * Output:  the key
 * Why it exists: the payload sits inside a bytea and may not be aligned
 * for 4-byte reads, so we copy the 4 bytes with memcpy instead of casting
 * the pointer to int32 *.
 */
static int32
read_exact_key(const char *payload, uint32 index)
{
	int32		key;
	Size		offset = (Size) index * sizeof(int32);

	memcpy(&key, payload + offset, sizeof(int32));
	return key;
}

/*
 * exact_filter_contains
 *
 * What it does: binary search for a key in the sorted EXACT payload.
 * We look at the middle key; if it is too small the key can only be in
 * the right half, if it is too big only in the left half. Each step
 * halves the range, so 1,000,000 keys need at most 20 steps.
 *
 * Inputs:  payload  - the first byte after the header
 *          num_keys - number of keys in the payload
 *          key      - the key to look for
 * Output:  true if the key is in the payload, false otherwise
 *
 * Why it exists: this is sjb_contains for mode EXACT. There are no false
 * positives: true means the key really is one of site A's keys.
 */
static bool
exact_filter_contains(const char *payload, uint32 num_keys, int32 key)
{
	int64		low = 0;
	int64		high = (int64) num_keys - 1;

	while (low <= high)
	{
		int64		middle = low + (high - low) / 2;
		int32		middle_key = read_exact_key(payload, (uint32) middle);

		if (middle_key == key)
			return true;

		if (middle_key < key)
			low = middle + 1;	/* the key can only be to the right */
		else
			high = middle - 1;	/* the key can only be to the left */
	}
	return false;
}


/* ------------------------------------------------------------------------
 * Part 3: building filters
 * ------------------------------------------------------------------------
 */

/*
 * new_filter
 *
 * What it does: allocates a new filter bytea with room for the 24-byte
 * header and payload_bytes bytes of payload, all set to 0.
 *
 * Input:  payload_bytes - size of the payload
 * Output: the new bytea (its header must still be written)
 *
 * Why it exists: every build function needs the same allocation.
 */
static bytea *
new_filter(Size payload_bytes)
{
	Size		total_size = VARHDRSZ + sizeof(SjbHeader) + payload_bytes;
	bytea	   *filter;

	/*
	 * A bytea is a "varlena": a 4-byte length header (VARHDRSZ bytes)
	 * followed by the data. palloc0 allocates memory in the current
	 * PostgreSQL memory context and fills it with zeros, so all Bloom bits
	 * start at 0. (palloc memory is freed automatically at the end of the
	 * query, so we never call free.) SET_VARSIZE writes the total size
	 * into the length header.
	 */
	filter = (bytea *) palloc0(total_size);
	SET_VARSIZE(filter, total_size);
	return filter;
}

/*
 * write_header
 *
 * What it does: fills in an SjbHeader and copies it to the start of the
 * filter's data.
 *
 * Inputs:  filter - a bytea from new_filter
 *          mode   - SJB_MODE_BLOOM, SJB_MODE_EXACT or SJB_MODE_PASS
 *          k, m   - Bloom parameters (0 for EXACT and PASS)
 *          nkeys  - number of keys the filter was built from
 * Output:  none (writes into the filter)
 *
 * Why it exists: all modes share the header; memcpy is used because the
 * data inside a bytea may not be aligned for 8-byte writes.
 */
static void
write_header(bytea *filter, int mode, int k, uint64 m, uint32 nkeys)
{
	SjbHeader	header;

	memset(&header, 0, sizeof(header));
	header.version = SJB_VERSION;
	header.mode = (uint8) mode;
	header.k = (uint16) k;
	header.nkeys = nkeys;
	header.m = m;
	header.seed = SJB_SEED;

	/* VARDATA gives a pointer to the first byte after the length header. */
	memcpy(VARDATA(filter), &header, sizeof(SjbHeader));
}

/*
 * filter_payload
 *
 * What it does: returns a pointer to the first payload byte of a filter
 * we are building (right after the 24-byte header).
 * Input:  filter - a bytea from new_filter
 * Output: pointer to the payload
 * Why it exists: so the "skip the header" step is written only once.
 */
static char *
filter_payload(bytea *filter)
{
	return VARDATA(filter) + sizeof(SjbHeader);
}

/*
 * build_bloom_filter
 *
 * What it does: builds a BLOOM filter with m bits and k positions per key
 * and adds every key of the C array to it.
 *
 * Inputs:  keys, num_keys - the keys (no NULLs)
 *          m, k           - filter size and positions per key (checked)
 * Output:  the new filter
 *
 * Why it exists: used by sjb_build and sjb_build_auto.
 */
static bytea *
build_bloom_filter(const int32 *keys, int num_keys, uint64 m, int k)
{
	bytea	   *filter = new_filter(bloom_payload_bytes(m));
	uint8	   *bits = (uint8 *) filter_payload(filter);
	int			i;

	for (i = 0; i < num_keys; i++)
		sjb_bloom_add(bits, m, k, SJB_SEED, keys[i]);

	write_header(filter, SJB_MODE_BLOOM, k, m, (uint32) num_keys);
	return filter;
}

/*
 * build_exact_filter
 *
 * What it does: builds an EXACT filter: the header, then the keys one
 * after another, 4 bytes each, copied one by one with memcpy.
 *
 * Inputs:  keys, num_keys - SORTED, UNIQUE keys (num_keys may be 0)
 * Output:  the new filter
 *
 * Why it exists: an EXACT filter has no false positives. With 0 keys it
 * lets nothing pass, which is right for an empty R' (the join is empty).
 */
static bytea *
build_exact_filter(const int32 *keys, int num_keys)
{
	bytea	   *filter = new_filter((Size) num_keys * sizeof(int32));
	char	   *payload = filter_payload(filter);
	int			i;

	for (i = 0; i < num_keys; i++)
	{
		Size		offset = (Size) i * sizeof(int32);

		memcpy(payload + offset, &keys[i], sizeof(int32));
	}

	write_header(filter, SJB_MODE_EXACT, 0, 0, (uint32) num_keys);
	return filter;
}

/*
 * build_pass_filter
 *
 * What it does: builds a PASS filter: only the header, no payload.
 * Input:  num_keys - stored in the header for information only
 * Output: the new filter
 * Why it exists: when (almost) every row of S matches anyway, sending a
 * real filter costs more than it saves.
 */
static bytea *
build_pass_filter(int num_keys)
{
	bytea	   *filter = new_filter(0);

	write_header(filter, SJB_MODE_PASS, 0, 0, (uint32) num_keys);
	return filter;
}

/*
 * compare_int32
 *
 * What it does: the comparison function qsort needs: negative if the
 * first key is smaller, positive if it is bigger, 0 if equal.
 * Inputs:  a, b - pointers to two int32 values
 * Output:  -1, 0 or 1
 * Why it exists: qsort sorts any kind of data, so we must tell it how to
 * compare two keys. We compare with < and > instead of returning
 * (x - y), because x - y can overflow for very large or very negative
 * keys and then give the wrong sign.
 */
static int
compare_int32(const void *a, const void *b)
{
	int32		first = *(const int32 *) a;
	int32		second = *(const int32 *) b;

	if (first < second)
		return -1;
	if (first > second)
		return 1;
	return 0;
}

/*
 * remove_duplicates
 *
 * What it does: removes repeated keys from a SORTED array, in place.
 * Equal keys are next to each other after sorting, so we keep a key only
 * if it differs from the last key we kept.
 *
 * Inputs:  keys, num_keys - a sorted array
 * Output:  the new number of keys (the first ones in the array)
 *
 * Why it exists: n must count DISTINCT keys, and the EXACT payload must
 * be unique for the binary search to stay simple.
 */
static int
remove_duplicates(int32 *keys, int num_keys)
{
	int			num_unique = 0;
	int			i;

	for (i = 0; i < num_keys; i++)
	{
		if (num_unique == 0 || keys[i] != keys[num_unique - 1])
		{
			keys[num_unique] = keys[i];
			num_unique = num_unique + 1;
		}
	}
	return num_unique;
}

/*
 * collect_unique_keys
 *
 * What it does: copies the non-NULL elements of an int4[] into a C array,
 * sorts it and removes duplicates.
 *
 * Inputs:  keys_array - the SQL array
 * Output:  the palloc'd C array; *num_keys_out = number of unique keys
 *
 * Why it exists: sjb_build_auto needs the exact number of distinct keys
 * (the n of the cost model), and the EXACT mode needs them sorted.
 */
static int32 *
collect_unique_keys(ArrayType *keys_array, int *num_keys_out)
{
	Datum	   *elements;
	bool	   *nulls;
	int			num_elements;
	int32	   *keys;
	int			num_keys = 0;
	int			i;

	/*
	 * deconstruct_array_builtin unpacks an SQL array of a built-in type
	 * (here INT4OID = int4) into two C arrays: elements[i] holds the value
	 * and nulls[i] says whether that element is NULL.
	 */
	deconstruct_array_builtin(keys_array, INT4OID,
							  &elements, &nulls, &num_elements);

	/* palloc is PostgreSQL's malloc (freed at the end of the query). */
	keys = (int32 *) palloc(sizeof(int32) * (num_elements + 1));

	for (i = 0; i < num_elements; i++)
	{
		if (nulls[i])
			continue;			/* a NULL key never matches in a join */

		/* DatumGetInt32 unwraps a Datum back into a C int32. */
		keys[num_keys] = DatumGetInt32(elements[i]);
		num_keys = num_keys + 1;
	}

	qsort(keys, num_keys, sizeof(int32), compare_int32);
	*num_keys_out = remove_duplicates(keys, num_keys);
	return keys;
}


/* ------------------------------------------------------------------------
 * Part 4: reading cost model arguments
 * ------------------------------------------------------------------------
 */

/*
 * check_cost_argument
 *
 * What it does: raises an error if a cost model argument is not a normal
 * number, is negative, or is 0 when 0 is not allowed.
 *
 * Inputs:  name         - the argument name, for the error message
 *          value        - its value
 *          zero_allowed - false for values we divide by (network_mbit,
 *                         wire_factor)
 * Output:  none; returns normally only if the value is valid
 *
 * Why it exists: a NaN, an infinity or a negative count would make the
 * estimates meaningless, and a zero bandwidth would divide by zero.
 */
static void
check_cost_argument(const char *name, double value, bool zero_allowed)
{
	bool		is_valid = true;

	if (isnan(value) || isinf(value) || value < 0)
		is_valid = false;
	if (value == 0 && !zero_allowed)
		is_valid = false;

	if (!is_valid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sjbloom: %s must be a number %s 0, got %g",
						name, zero_allowed ? ">=" : ">", value)));
}

/*
 * check_cost_inputs
 *
 * What it does: checks every field of an SjbCostInputs with
 * check_cost_argument.
 * Input:  inputs
 * Output: none; returns normally only if all values are valid
 * Why it exists: all three SQL functions that use the cost model need
 * the same checks.
 */
static void
check_cost_inputs(const SjbCostInputs *inputs)
{
	check_cost_argument("n", inputs->num_keys, true);
	check_cost_argument("s_rows", inputs->s_rows, true);
	check_cost_argument("rows_per_key", inputs->rows_per_key, true);
	check_cost_argument("row_width", inputs->row_width, true);
	check_cost_argument("network_mbit", inputs->network_mbit, false);
	check_cost_argument("t_check_ns", inputs->t_check_ns, true);
	check_cost_argument("t_lookup_us", inputs->t_lookup_us, true);
	check_cost_argument("max_filter_bytes", inputs->max_filter_bytes, true);
	check_cost_argument("wire_factor", inputs->wire_factor, false);
	check_cost_argument("per_message_ms", inputs->per_message_ms, true);
	check_cost_argument("per_row_us", inputs->per_row_us, true);
	check_cost_argument("t_build_us", inputs->t_build_us, true);
	check_cost_argument("fetch_size", inputs->fetch_size, false);
}

/*
 * read_report_arguments
 *
 * What it does: reads the 11 arguments of sjb_choice_report and
 * sjb_plan_choice (n, s_rows, rows_per_key, row_width, network_mbit,
 * t_check_ns, t_lookup_us, per_message_ms, per_row_us, t_build_us,
 * fetch_size) into an SjbCostInputs, uses the default max_filter_bytes,
 * wire_factor and key text length, and checks them.
 *
 * Inputs:  fcinfo - the function call info of the SQL function (it holds
 *                   the arguments; the PG_GETARG_ macros read from it)
 * Output:  *inputs is filled in
 *
 * Why it exists: both SQL functions take the same arguments, and they
 * must give the cost model exactly the same inputs.
 */
static void
read_report_arguments(FunctionCallInfo fcinfo, SjbCostInputs *inputs)
{
	/* PG_GETARG_FLOAT8(i) reads argument number i as a C double. */
	inputs->num_keys = PG_GETARG_FLOAT8(0);
	inputs->s_rows = PG_GETARG_FLOAT8(1);
	inputs->rows_per_key = PG_GETARG_FLOAT8(2);
	inputs->row_width = PG_GETARG_FLOAT8(3);
	inputs->network_mbit = PG_GETARG_FLOAT8(4);
	inputs->t_check_ns = PG_GETARG_FLOAT8(5);
	inputs->t_lookup_us = PG_GETARG_FLOAT8(6);
	inputs->per_message_ms = PG_GETARG_FLOAT8(7);
	inputs->per_row_us = PG_GETARG_FLOAT8(8);
	inputs->t_build_us = PG_GETARG_FLOAT8(9);
	inputs->fetch_size = PG_GETARG_INT32(10);
	inputs->max_filter_bytes = SJB_DEFAULT_MAX_FILTER_BYTES;
	inputs->wire_factor = SJB_DEFAULT_WIRE_FACTOR;
	inputs->key_text_len = SJB_DEFAULT_KEY_TEXT_LEN;

	check_cost_inputs(inputs);
}

/*
 * mode_name
 *
 * What it does: the lower-case name of a filter mode.
 * Input:  mode (SJB_MODE_...)
 * Output: "bloom", "exact" or "pass"
 * Why it exists: for the report text.
 */
static const char *
mode_name(int mode)
{
	if (mode == SJB_MODE_BLOOM)
		return "bloom";
	if (mode == SJB_MODE_EXACT)
		return "exact";
	return "pass";
}

/*
 * append_estimate_line
 *
 * What it does: appends one line of the report table for one estimate:
 * path, mode, bits/key, bytes A->B, rows back, bytes B->A, messages, then
 * the times in ms: network (bytes / bandwidth), messages, returned rows,
 * work at A, work at B and the total. A skipped mode gets
 * "skipped: <reason>" instead.
 *
 * Inputs:  report   - the text being built
 *          path     - "index" or "filter"
 *          estimate - the estimate to print
 * Output:  none (appends to report)
 *
 * Why it exists: the four lines of the table look the same.
 */
static void
append_estimate_line(StringInfo report, const char *path,
					 const SjbCostEstimate *estimate)
{
	const char *mode_column = "-";
	const char *bits_column = "-";

	if (strcmp(path, "filter") == 0)
		mode_column = estimate->name;

	if (!estimate->allowed)
	{
		/* appendStringInfo adds printf-style text to a StringInfo. */
		appendStringInfo(report, "%-7s %-6s skipped: %s\n",
						 path, mode_column, estimate->skip_reason);
		return;
	}

	if (estimate->bits_per_key > 0)
		bits_column = psprintf("%.2f", estimate->bits_per_key);

	appendStringInfo(report, "%-7s %-6s %8s %12.0f %10.0f %12.0f %8.0f %11.3f %11.3f %9.3f %10.3f %10.3f %11.3f\n",
					 path, mode_column, bits_column,
					 estimate->bytes_a_to_b,
					 estimate->rows_back,
					 estimate->bytes_b_to_a,
					 estimate->messages,
					 estimate->network_seconds * 1000.0,
					 estimate->message_seconds * 1000.0,
					 estimate->row_seconds * 1000.0,
					 estimate->work_at_a_seconds * 1000.0,
					 estimate->work_at_b_seconds * 1000.0,
					 estimate->total_seconds * 1000.0);
}


/* ------------------------------------------------------------------------
 * Part 5: the SQL functions
 * ------------------------------------------------------------------------
 */

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
 *          bytes of bits. header.nkeys = number of keys added (repeated
 *          keys are counted each time, as before Batch 5).
 *
 * Why it exists: this runs at site A; the result is sent to site B. It is
 * the "manual" builder where the caller chooses m and k.
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
	int32	   *keys;
	int			num_keys = 0;
	int			i;

	check_bloom_parameters(m, k);

	deconstruct_array_builtin(keys_array, INT4OID,
							  &elements, &nulls, &num_elements);

	/* Copy the non-NULL keys into a plain C array (no sorting here). */
	keys = (int32 *) palloc(sizeof(int32) * (num_elements + 1));
	for (i = 0; i < num_elements; i++)
	{
		if (nulls[i])
			continue;			/* a NULL key never matches in a join */
		keys[num_keys] = DatumGetInt32(elements[i]);
		num_keys = num_keys + 1;
	}

	/* PG_RETURN_BYTEA_P returns the bytea to SQL. */
	PG_RETURN_BYTEA_P(build_bloom_filter(keys, num_keys, (uint64) m, k));
}


PG_FUNCTION_INFO_V1(sjb_build_auto);

/*
 * sjb_build_auto(keys int4[], s_rows float8, rows_per_key float8,
 *                row_width float8, network_mbit float8, t_check_ns float8,
 *                max_filter_bytes int8, wire_factor float8,
 *                per_message_ms float8, per_row_us float8,
 *                t_build_us float8, fetch_size int4) RETURNS bytea
 *
 * What it does: builds the filter the cost model says is fastest:
 *   a) collect the non-NULL keys, sort them, remove duplicates -> n
 *   b) n = 0: return an EXACT filter with no keys (nothing passes; the
 *      join is empty, so that is correct)
 *   c) estimate the time of BLOOM (sized with b*), EXACT and PASS, skipping
 *      BLOOM if b* = 0 and any mode whose filter exceeds max_filter_bytes
 *      (sjb_estimate_all in costmodel.c)
 *   d) build the mode with the smallest time; header nkeys = n
 *
 * Inputs:  keys          - site A's join keys (NULLs and repeats allowed)
 *          s_rows        - rows of S at site B (from the catalog)
 *          rows_per_key  - rows of S per key value (from the catalog)
 *          row_width     - bytes per returned row on the wire
 *          network_mbit  - network speed
 *          t_check_ns    - site B's time to scan + check one row
 *          max_filter_bytes - largest BLOOM/EXACT filter allowed
 *          wire_factor   - wire bytes per filter byte (2 = hex text)
 *          per_message_ms, per_row_us, t_build_us - calibrated costs
 *          fetch_size    - rows per postgres_fdw FETCH
 * Output:  the filter (bytea)
 *
 * Why it exists: the filter is chosen at RUNTIME, when the exact number
 * of distinct keys is known, not guessed by the planner.
 */
Datum
sjb_build_auto(PG_FUNCTION_ARGS)
{
	ArrayType  *keys_array = PG_GETARG_ARRAYTYPE_P(0);
	SjbCostInputs inputs;
	SjbChoice	choice;
	int32	   *keys;
	int			num_keys;

	/* Step a: the distinct keys. */
	keys = collect_unique_keys(keys_array, &num_keys);

	/* Read and check the cost model arguments. */
	inputs.num_keys = num_keys;
	inputs.s_rows = PG_GETARG_FLOAT8(1);
	inputs.rows_per_key = PG_GETARG_FLOAT8(2);
	inputs.row_width = PG_GETARG_FLOAT8(3);
	inputs.network_mbit = PG_GETARG_FLOAT8(4);
	inputs.t_check_ns = PG_GETARG_FLOAT8(5);
	inputs.max_filter_bytes = (double) PG_GETARG_INT64(6);
	inputs.wire_factor = PG_GETARG_FLOAT8(7);
	inputs.per_message_ms = PG_GETARG_FLOAT8(8);
	inputs.per_row_us = PG_GETARG_FLOAT8(9);
	inputs.t_build_us = PG_GETARG_FLOAT8(10);
	inputs.fetch_size = PG_GETARG_INT32(11);
	inputs.t_lookup_us = 0.0;	/* only the index path uses it */
	inputs.key_text_len = SJB_DEFAULT_KEY_TEXT_LEN;
	check_cost_inputs(&inputs);

	/* Step b: no keys at all. */
	if (num_keys == 0)
		PG_RETURN_BYTEA_P(build_exact_filter(keys, 0));

	/* Step c: the estimates. */
	sjb_estimate_all(&inputs, &choice);

	/* Step d: build the winner. */
	if (choice.best_filter_mode == SJB_MODE_BLOOM)
		PG_RETURN_BYTEA_P(build_bloom_filter(keys, num_keys,
											 (uint64) choice.bloom.m,
											 (int) choice.bloom.k));

	if (choice.best_filter_mode == SJB_MODE_EXACT)
		PG_RETURN_BYTEA_P(build_exact_filter(keys, num_keys));

	PG_RETURN_BYTEA_P(build_pass_filter(num_keys));
}


PG_FUNCTION_INFO_V1(sjb_contains);

/*
 * sjb_contains(filter bytea, key int4) RETURNS bool
 *
 * What it does: tests whether a key passes the filter:
 *   BLOOM: true if all k bits are 1 (may be a false positive)
 *   EXACT: true if the key is in the sorted key list (binary search)
 *   PASS:  always true
 *
 * Inputs:  filter - a filter made by sjb_build or sjb_build_auto
 *          key    - one join key from table S
 * Output:  true if the key passes, false if it is certainly not one of
 *          site A's keys. (Because the SQL function is STRICT, a NULL key
 *          gives NULL without this C code ever running.)
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
	const char *payload;
	bool		passes = false;

	read_header(filter_data, filter_size, &header);
	payload_size = filter_size - sizeof(SjbHeader);
	payload = filter_data + sizeof(SjbHeader);

	if (header.mode == SJB_MODE_BLOOM)
	{
		check_bloom_filter(&header, payload_size);
		passes = sjb_bloom_test((const uint8 *) payload, header.m, header.k,
								header.seed, key);
	}
	else if (header.mode == SJB_MODE_EXACT)
	{
		check_exact_filter(&header, payload_size);
		passes = exact_filter_contains(payload, header.nkeys, key);
	}
	else if (header.mode == SJB_MODE_PASS)
	{
		check_pass_filter(payload_size);
		passes = true;
	}
	else
		error_unknown_mode(header.mode);

	/* PG_RETURN_BOOL returns a C bool to SQL as a boolean. */
	PG_RETURN_BOOL(passes);
}


PG_FUNCTION_INFO_V1(sjb_info);

/*
 * sjb_info(filter bytea) RETURNS text
 *
 * What it does: describes a filter in one line, for example
 *   mode=bloom nkeys=10000 m=220000 k=15 bits_set=48.10% bytes=27524
 *   mode=exact nkeys=3 min_key=1 max_key=7 bytes=36
 *   mode=exact nkeys=0 bytes=24
 *   mode=pass nkeys=5000 bytes=24
 * "bytes" is the header plus the payload (without the varlena length).
 *
 * Input:  filter - a filter made by sjb_build or sjb_build_auto
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
	const char *payload;
	char	   *description = NULL;

	read_header(filter_data, filter_size, &header);
	payload_size = filter_size - sizeof(SjbHeader);
	payload = filter_data + sizeof(SjbHeader);

	if (header.mode == SJB_MODE_BLOOM)
	{
		uint64		num_bits_set;
		double		percent_set;

		check_bloom_filter(&header, payload_size);
		num_bits_set = count_set_bits((const uint8 *) payload, payload_size);
		percent_set = 100.0 * (double) num_bits_set / (double) header.m;

		/* psprintf is like sprintf, but returns a new palloc'd string. */
		description = psprintf("mode=bloom nkeys=%u m=%llu k=%d bits_set=%.2f%% bytes=%zu",
							   header.nkeys,
							   (unsigned long long) header.m,
							   (int) header.k,
							   percent_set,
							   filter_size);
	}
	else if (header.mode == SJB_MODE_EXACT)
	{
		check_exact_filter(&header, payload_size);
		if (header.nkeys == 0)
			description = psprintf("mode=exact nkeys=0 bytes=%zu", filter_size);
		else
			description = psprintf("mode=exact nkeys=%u min_key=%d max_key=%d bytes=%zu",
								   header.nkeys,
								   read_exact_key(payload, 0),
								   read_exact_key(payload, header.nkeys - 1),
								   filter_size);
	}
	else if (header.mode == SJB_MODE_PASS)
	{
		check_pass_filter(payload_size);
		description = psprintf("mode=pass nkeys=%u bytes=%zu",
							   header.nkeys, filter_size);
	}
	else
		error_unknown_mode(header.mode);

	/*
	 * cstring_to_text turns a C string into an SQL text value, and
	 * PG_RETURN_TEXT_P returns it.
	 */
	PG_RETURN_TEXT_P(cstring_to_text(description));
}


PG_FUNCTION_INFO_V1(sjb_choice_report);

/*
 * sjb_choice_report(n, s_rows, rows_per_key, row_width, network_mbit,
 *                   t_check_ns, t_lookup_us, per_message_ms, per_row_us,
 *                   t_build_us float8, fetch_size int4) RETURNS text
 *
 * What it does: runs the cost model (sjb_estimate_all, the same code
 * sjb_build_auto uses) and returns a readable multi-line report: one line
 * for the index path and one for each filter mode, with bytes A->B, rows
 * back, bytes B->A, messages, and the times in ms (network, messages,
 * returned rows, work at A, work at B, total), then the best filter mode
 * and the winner.
 *
 * Inputs:  n            - number of distinct keys at site A
 *          s_rows .. t_check_ns - as for sjb_build_auto
 *          t_lookup_us  - time per key of the index path
 *          per_message_ms .. fetch_size - as for sjb_build_auto
 *          (max_filter_bytes = 1 MB and wire_factor = 2, the defaults)
 * Output:  the report as text
 *
 * Why it exists: to explain and present the model's decisions, and for
 * bench/validate_model.py to read the predicted times.
 */
Datum
sjb_choice_report(PG_FUNCTION_ARGS)
{
	SjbCostInputs inputs;
	SjbChoice	choice;
	const SjbCostEstimate *best_filter;
	StringInfoData report;

	read_report_arguments(fcinfo, &inputs);
	sjb_estimate_all(&inputs, &choice);
	best_filter = sjb_estimate_for_mode(&choice, choice.best_filter_mode);

	/*
	 * A StringInfo is PostgreSQL's growing text buffer: initStringInfo
	 * starts an empty one, and appendStringInfo adds to the end of it.
	 */
	initStringInfo(&report);
	appendStringInfo(&report,
					 "inputs: n=%.0f s_rows=%.0f rows_per_key=%g row_width=%.2f "
					 "network_mbit=%g t_check_ns=%.2f t_lookup_us=%.4f\n",
					 inputs.num_keys, inputs.s_rows, inputs.rows_per_key,
					 inputs.row_width, inputs.network_mbit,
					 inputs.t_check_ns, inputs.t_lookup_us);
	appendStringInfo(&report,
					 "        per_message_ms=%.4f per_row_us=%.4f t_build_us=%.4f fetch_size=%.0f\n",
					 inputs.per_message_ms, inputs.per_row_us,
					 inputs.t_build_us, inputs.fetch_size);
	appendStringInfo(&report, "matching rows M = %.0f\n", choice.matching_rows);
	appendStringInfo(&report, "%-7s %-6s %8s %12s %10s %12s %8s %11s %11s %9s %10s %10s %11s\n",
					 "path", "mode", "bits/key", "bytes_A_to_B", "rows_back",
					 "bytes_B_to_A", "messages", "network_ms", "messages_ms",
					 "rows_ms", "work_A_ms", "work_B_ms", "total_ms");

	append_estimate_line(&report, "index", &choice.index_path);
	append_estimate_line(&report, "filter", &choice.bloom);
	append_estimate_line(&report, "filter", &choice.exact);
	append_estimate_line(&report, "filter", &choice.pass);

	if (choice.bloom.allowed)
		appendStringInfo(&report, "bloom size: m=%.0f bits, k=%.0f, predicted fp rate=%.6f\n",
						 choice.bloom.m, choice.bloom.k, choice.bloom.fp_rate);

	appendStringInfo(&report, "best filter mode: %s (%.3f ms)\n",
					 mode_name(choice.best_filter_mode),
					 best_filter->total_seconds * 1000.0);

	if (choice.index_path_wins)
		appendStringInfo(&report, "winner: index path (%.3f ms <= %.3f ms for filter path with %s)",
						 choice.index_path.total_seconds * 1000.0,
						 best_filter->total_seconds * 1000.0,
						 mode_name(choice.best_filter_mode));
	else
		appendStringInfo(&report, "winner: filter path with %s (%.3f ms < %.3f ms for index path)",
						 mode_name(choice.best_filter_mode),
						 best_filter->total_seconds * 1000.0,
						 choice.index_path.total_seconds * 1000.0);

	PG_RETURN_TEXT_P(cstring_to_text(report.data));
}


PG_FUNCTION_INFO_V1(sjb_plan_choice);

/*
 * sjb_plan_choice(n, s_rows, rows_per_key, row_width, network_mbit,
 *                 t_check_ns, t_lookup_us float8) RETURNS text
 *
 * What it does: returns 'index' if the index path is estimated to be
 * faster (or equal), otherwise 'filter'.
 *
 * Inputs:  the same as sjb_choice_report
 * Output:  'index' or 'filter'
 *
 * Why it exists: Batch 6's planner hook will call this, so the decision
 * between the two paths lives in ONE place (sjb_estimate_all).
 */
Datum
sjb_plan_choice(PG_FUNCTION_ARGS)
{
	SjbCostInputs inputs;
	SjbChoice	choice;

	read_report_arguments(fcinfo, &inputs);
	sjb_estimate_all(&inputs, &choice);

	if (choice.index_path_wins)
		PG_RETURN_TEXT_P(cstring_to_text("index"));
	PG_RETURN_TEXT_P(cstring_to_text("filter"));
}
