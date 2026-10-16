/*-------------------------------------------------------------------------
 *
 * costmodel.c
 *
 * The time-based cost model of the sjbloom extension (Batch 5).
 *
 * It estimates how long the join's remote part takes for each way of
 * asking site B for rows (see costmodel.h for the overview):
 *
 *   index path         key list A->B, n index lookups at B, M rows back
 *   filter path BLOOM  Bloom filter A->B, |S| checks at B,
 *                      M rows + false positives back
 *   filter path EXACT  sorted key list (binary) A->B, |S| checks at B,
 *                      M rows back
 *   filter path PASS   header only A->B, |S| checks at B, all |S| rows back
 *
 *   time = T_CPU * work  +  T_MSG * messages  +  T_TR * bytes
 *
 * the total-time formula of Ozsu & Valduriez, "Principles of Distributed
 * Database Systems" (chapter on query optimization, cost functions).
 * Our terms:
 *   T_TR * bytes      (bytes A->B + bytes B->A) / bandwidth
 *   T_MSG * messages  messages * per_message_ms
 *   T_CPU * work      rows_back * per_row_us          (returned rows)
 *                     + n * t_lookup_us               (index path, at B)
 *                     + s_rows * t_check_ns           (filter path, at B)
 *                     + n * t_build_us                (filter path, at A)
 * The constants are measured by bench/calibrate.py.
 *
 * Every function here is plain arithmetic on doubles: no SQL values, no
 * memory allocation. sjbloom.c calls sjb_estimate_all, and Batch 6's
 * planner hook will call it too, so all decisions are made in ONE place.
 *
 * What the model does NOT contain: the rest of site A's work (scanning
 * orders, collecting the keys, the final join) and planning time. They
 * are the same for both paths or do not depend on the path choice.
 * bench/validate_model.py compares the model with measured times.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "sjb_filter.h"
#include "costmodel.h"


/*
 * sjb_fp_rate
 *
 * What it does: the predicted false-positive rate of a Bloom filter with
 * m bits, k hash positions per key and n keys added:
 *
 *   fp = (1 - e^(-k*n/m))^k
 *
 * Source: Broder & Mitzenmacher, "Network Applications of Bloom Filters:
 * A Survey" (2004). This is the ONLY false-positive formula we use.
 *
 * Inputs:  n - number of keys in the filter
 *          m - number of bits
 *          k - number of hash positions per key
 * Output:  the probability (0 .. 1) that a key that was NOT added passes
 *
 * Why it exists: false positives are extra rows that site B sends back
 * for nothing, so the BLOOM estimate needs to know how many there are.
 */
double
sjb_fp_rate(double n, double m, double k)
{
	if (n <= 0 || m <= 0 || k <= 0)
		return 0.0;				/* an empty filter lets nothing through */

	return pow(1.0 - exp(-k * n / m), k);
}

/*
 * sjb_best_bits
 *
 * What it does: the number of bits per key b that makes the total bytes
 * of the Bloom filter path as small as possible:
 *
 *   b* = ln( 3.844 * (s_rows - matches) * row_width / (wire_factor * n) )
 *        / 0.4805
 *
 * How it was derived (see docs/WALKTHROUGH.md, Batch 3 and Batch 5):
 * with the best k, fp(b) = e^(-0.4805 * b), where 0.4805 = (ln 2)^2.
 * Total bytes = filter (wire_factor * n * b / 8) + true matches
 * (matches * row_width, does not depend on b) + false positives
 * ((s_rows - matches) * row_width * fp(b)). Setting the derivative to 0
 * gives b*. 3.844 = 8 * 0.4805. It is the same formula as the SQL function
 * sjb_best_bits in sjbloom--1.0.sql.
 *
 * Inputs:  n           - number of keys in the filter
 *          s_rows      - rows of S at site B
 *          matches     - rows of S that really match (they come back anyway)
 *          row_width   - bytes of one returned row
 *          wire_factor - wire bytes per filter byte (2 = hex text)
 * Output:  b*, or 0 when Bloom is not worth it: the value inside ln() is
 *          1 or less, so even the smallest filter costs more than the
 *          false positives it saves. Also 0 when n <= 0.
 *
 * Why it exists: the BLOOM mode is sized with it (dynamic sizing).
 */
double
sjb_best_bits(double n, double s_rows, double matches,
			  double row_width, double wire_factor)
{
	double		inside_ln;

	if (n <= 0 || wire_factor <= 0)
		return 0.0;

	inside_ln = 3.844 * (s_rows - matches) * row_width / (wire_factor * n);
	if (inside_ln <= 1.0)
		return 0.0;

	return log(inside_ln) / 0.4805;
}

/*
 * sjb_bloom_bits_for
 *
 * What it does: the filter size m for n keys and b bits per key:
 *   m = max(8, ceil(n * b))
 * Inputs:  n, bits_per_key
 * Output:  m (a whole number, as a double)
 * Why it exists: sjb_build_auto and the cost estimate must use the same m.
 */
double
sjb_bloom_bits_for(double n, double bits_per_key)
{
	double		m = ceil(n * bits_per_key);

	if (m < SJB_MIN_BITS)
		m = SJB_MIN_BITS;
	return m;
}

/*
 * sjb_bloom_hashes_for
 *
 * What it does: the number of hash positions per key for b bits per key:
 *   k = round(0.693 * b), at least 1 and at most 32
 * 0.693 = ln 2; k = b * ln 2 gives the lowest false-positive rate for a
 * given b (Broder & Mitzenmacher 2004).
 * Input:   bits_per_key
 * Output:  k (a whole number, as a double)
 * Why it exists: sjb_build_auto and the cost estimate must use the same k.
 */
double
sjb_bloom_hashes_for(double bits_per_key)
{
	double		k = round(0.693 * bits_per_key);

	if (k < SJB_MIN_K)
		k = SJB_MIN_K;
	if (k > SJB_MAX_K)
		k = SJB_MAX_K;
	return k;
}

/*
 * sjb_matching_rows
 *
 * What it does: M, the number of rows of S that really match one of the
 * n keys:  M = min(s_rows, n * rows_per_key)
 * Inputs:  n, s_rows, rows_per_key (rows of S per key value; 1 when the
 *          join column is unique at site B)
 * Output:  M
 * Why it exists: these rows come back in every mode, so every estimate
 * needs them.
 */
double
sjb_matching_rows(double n, double s_rows, double rows_per_key)
{
	double		matches = n * rows_per_key;

	if (matches > s_rows)
		matches = s_rows;
	return matches;
}

/*
 * sjb_bloom_filter_bytes
 *
 * What it does: bytes on the wire for a BLOOM filter with m bits:
 *   wire_factor * (24 + ceil(m/8)) + 2
 * 24 = header, ceil(m/8) = bits, and + 2 for the "\x" in front of the
 * hex text, because postgres_fdw sends the bytea as hex text.
 * Inputs:  m, wire_factor
 * Output:  bytes A->B
 * Why it exists: the network part of the BLOOM estimate.
 */
double
sjb_bloom_filter_bytes(double m, double wire_factor)
{
	return wire_factor * (sizeof(SjbHeader) + ceil(m / 8.0)) + 2.0;
}

/*
 * sjb_exact_filter_bytes
 *
 * What it does: bytes on the wire for an EXACT filter with n keys:
 *   wire_factor * (24 + 4*n) + 2      (each key is a 4-byte int4)
 * Inputs:  n, wire_factor
 * Output:  bytes A->B
 * Why it exists: the network part of the EXACT estimate.
 */
double
sjb_exact_filter_bytes(double n, double wire_factor)
{
	return wire_factor * (sizeof(SjbHeader) + 4.0 * n) + 2.0;
}

/*
 * sjb_pass_filter_bytes
 *
 * What it does: bytes on the wire for a PASS filter (header only):
 *   wire_factor * 24 + 2
 * Input:   wire_factor
 * Output:  bytes A->B
 * Why it exists: the network part of the PASS estimate.
 */
double
sjb_pass_filter_bytes(double wire_factor)
{
	return wire_factor * sizeof(SjbHeader) + 2.0;
}

/*
 * sjb_key_list_bytes
 *
 * What it does: bytes on the wire for the index path's key list. It is
 * an int4[] sent as text "{12,345,...}":
 *   n * key_text_len + 2      (+ 2 for the braces)
 * Inputs:  n, key_text_len (average digits + comma per key, default 7)
 * Output:  bytes A->B
 * Why it exists: the network part of the index path estimate.
 */
double
sjb_key_list_bytes(double n, double key_text_len)
{
	return n * key_text_len + 2.0;
}

/*
 * sjb_bandwidth_bytes_per_sec
 *
 * What it does: turns megabits per second into bytes per second:
 *   network_mbit * 1,000,000 / 8 = network_mbit * 125,000
 * Input:   network_mbit
 * Output:  bytes per second
 * Why it exists: network time = bytes / bandwidth.
 */
double
sjb_bandwidth_bytes_per_sec(double network_mbit)
{
	return network_mbit * 125000.0;
}

/*
 * sjb_message_count
 *
 * What it does: the number of message round trips between site A and
 * site B for one remote query:
 *   messages = 1 + ceil(rows_back / fetch_size)
 * postgres_fdw first sends the query (opening a cursor), then fetches the
 * rows fetch_size at a time (default 100), one round trip per FETCH.
 * Source: the T_MSG * #messages term of Ozsu & Valduriez; the message
 * pattern is postgres_fdw's (contrib/postgres_fdw, fetch_more_data).
 *
 * Inputs:  rows_back  - rows site B sends back
 *          fetch_size - rows per FETCH (at least 1)
 * Output:  the number of messages
 * Why it exists: many returned rows also mean many round trips.
 */
double
sjb_message_count(double rows_back, double fetch_size)
{
	return 1.0 + ceil(rows_back / fetch_size);
}

/*
 * sjb_message_seconds
 *
 * What it does: the time spent on messages:
 *   messages * per_message_ms * 1e-3      (ms -> seconds)
 * Source: T_MSG * #messages (Ozsu & Valduriez). per_message_ms is
 * measured by bench/calibrate.py.
 * Inputs:  messages, per_message_ms
 * Output:  seconds
 * Why it exists: one part of the total time.
 */
double
sjb_message_seconds(double messages, double per_message_ms)
{
	return messages * per_message_ms * 1e-3;
}

/*
 * sjb_returned_row_seconds
 *
 * What it does: the CPU time per returned row, for all rows:
 *   rows_back * per_row_us * 1e-6      (us -> seconds)
 * Site B turns every returned row into text, and site A turns the text
 * back into values and builds a tuple. This work does not depend on the
 * network speed.
 * Source: a T_CPU * #instructions term of Ozsu & Valduriez, measured per
 * row by bench/calibrate.py.
 * Inputs:  rows_back, per_row_us
 * Output:  seconds
 * Why it exists: validation in Batch 5 showed 2-4 us per returned row
 * that the bytes / bandwidth term alone did not explain.
 */
double
sjb_returned_row_seconds(double rows_back, double per_row_us)
{
	return rows_back * per_row_us * 1e-6;
}

/*
 * sjb_index_lookup_seconds
 *
 * What it does: the index path's work per key:
 *   n * t_lookup_us * 1e-6      (us -> seconds)
 * t_lookup_us is measured END TO END through postgres_fdw (key list to
 * text at A, parsing and planning at B, the index lookup), with the
 * message and per-row costs taken out.
 * Source: a T_CPU * #instructions term of Ozsu & Valduriez.
 * Inputs:  n, t_lookup_us
 * Output:  seconds
 * Why it exists: the work of the index path.
 */
double
sjb_index_lookup_seconds(double n, double t_lookup_us)
{
	return n * t_lookup_us * 1e-6;
}

/*
 * sjb_filter_build_seconds
 *
 * What it does: the filter path's extra work at site A, building the
 * filter from n keys (sort, remove repeats, hash or copy the keys):
 *   n * t_build_us * 1e-6      (us -> seconds)
 * Source: a T_CPU * #instructions term of Ozsu & Valduriez, measured per
 * key by bench/calibrate.py (timing sjb_build_auto at site A).
 * Inputs:  n, t_build_us
 * Output:  seconds
 * Why it exists: the index path does not build a filter, so this is a
 * real difference between the two paths.
 */
double
sjb_filter_build_seconds(double n, double t_build_us)
{
	return n * t_build_us * 1e-6;
}

/*
 * start_estimate
 *
 * What it does: sets every field of an estimate to 0, gives it a name and
 * marks it as allowed.
 * Inputs:  estimate - the estimate to fill in
 *          name     - "index", "bloom", "exact" or "pass"
 * Output:  none (fills *estimate)
 * Why it exists: so no field is ever left with garbage in it.
 */
static void
start_estimate(SjbCostEstimate *estimate, const char *name)
{
	memset(estimate, 0, sizeof(SjbCostEstimate));
	estimate->name = name;
	estimate->allowed = true;
	estimate->skip_reason = NULL;
}

/*
 * skip_estimate
 *
 * What it does: marks an estimate as skipped, with the reason.
 * Inputs:  estimate, reason (a constant string)
 * Output:  none
 * Why it exists: a skipped mode is never chosen, and the report shows why.
 */
static void
skip_estimate(SjbCostEstimate *estimate, const char *reason)
{
	estimate->allowed = false;
	estimate->skip_reason = reason;
}

/*
 * finish_estimate
 *
 * What it does: given bytes_a_to_b, rows_back, work_at_a_seconds and
 * work_at_b_seconds, fills in the rest:
 *   bytes_b_to_a    = rows_back * row_width
 *   network_seconds = (bytes_a_to_b + bytes_b_to_a) / bandwidth   (T_TR)
 *   messages        = 1 + ceil(rows_back / fetch_size)
 *   message_seconds = messages * per_message_ms                  (T_MSG)
 *   row_seconds     = rows_back * per_row_us                     (T_CPU)
 *   total_seconds   = network + messages + rows + work at A + work at B
 * Inputs:  inputs, estimate
 * Output:  none (fills *estimate)
 * Why it exists: the last steps are the same for all four estimates.
 */
static void
finish_estimate(const SjbCostInputs *inputs, SjbCostEstimate *estimate)
{
	double		bandwidth = sjb_bandwidth_bytes_per_sec(inputs->network_mbit);

	estimate->bytes_b_to_a = estimate->rows_back * inputs->row_width;
	estimate->network_seconds = (estimate->bytes_a_to_b + estimate->bytes_b_to_a) / bandwidth;
	estimate->messages = sjb_message_count(estimate->rows_back, inputs->fetch_size);
	estimate->message_seconds = sjb_message_seconds(estimate->messages, inputs->per_message_ms);
	estimate->row_seconds = sjb_returned_row_seconds(estimate->rows_back, inputs->per_row_us);
	estimate->total_seconds = estimate->network_seconds
		+ estimate->message_seconds
		+ estimate->row_seconds
		+ estimate->work_at_a_seconds
		+ estimate->work_at_b_seconds;
}

/*
 * set_filter_path_work
 *
 * What it does: sets the work of the filter path, the same for all three
 * filter modes:
 *   at site B: scan ALL rows of S and call sjb_contains on each one (it
 *              cannot use its index): s_rows * t_check_ns * 1e-9
 *   at site A: build the filter from the n keys: n * t_build_us * 1e-6
 * Inputs:  inputs, estimate
 * Output:  none (fills work_at_a_seconds and work_at_b_seconds)
 * Why it exists: all three filter modes need the same two lines.
 */
static void
set_filter_path_work(const SjbCostInputs *inputs, SjbCostEstimate *estimate)
{
	estimate->work_at_b_seconds = inputs->s_rows * inputs->t_check_ns * 1e-9;
	estimate->work_at_a_seconds = sjb_filter_build_seconds(inputs->num_keys, inputs->t_build_us);
}

/*
 * estimate_index_path
 *
 * What it does: the index path. A sends the key list as text; B does one
 * index lookup per key (n * t_lookup_us); the M matching rows come back.
 * There is no filter to build at site A.
 * Inputs:  inputs, matching_rows (M)
 * Output:  none (fills *estimate)
 * Why it exists: the planner (Batch 6) compares it with the filter path.
 */
static void
estimate_index_path(const SjbCostInputs *inputs, double matching_rows,
					SjbCostEstimate *estimate)
{
	start_estimate(estimate, "index");
	estimate->bytes_a_to_b = sjb_key_list_bytes(inputs->num_keys, inputs->key_text_len);
	estimate->rows_back = matching_rows;
	estimate->work_at_b_seconds = sjb_index_lookup_seconds(inputs->num_keys, inputs->t_lookup_us);
	estimate->work_at_a_seconds = 0.0;	/* no filter to build */
	finish_estimate(inputs, estimate);
}

/*
 * estimate_bloom
 *
 * What it does: the filter path with a BLOOM filter of b* bits per key.
 * Rows back = M + (s_rows - M) * fp, because every non-matching row
 * passes with probability fp.
 * Skipped when b* = 0 (Bloom not worth it), or when the filter would be
 * too big (more than max_filter_bytes, or more bits than SJB_MAX_BITS).
 *
 * Inputs:  inputs, matching_rows (M)
 * Output:  none (fills *estimate)
 * Why it exists: one of the three filter modes.
 */
static void
estimate_bloom(const SjbCostInputs *inputs, double matching_rows,
			   SjbCostEstimate *estimate)
{
	double		non_matching_rows = inputs->s_rows - matching_rows;

	start_estimate(estimate, "bloom");
	estimate->bits_per_key = sjb_best_bits(inputs->num_keys, inputs->s_rows,
										   matching_rows, inputs->row_width,
										   inputs->wire_factor);
	if (estimate->bits_per_key <= 0)
	{
		skip_estimate(estimate, "best bits per key is 0 (Bloom not worth it)");
		return;
	}

	estimate->m = sjb_bloom_bits_for(inputs->num_keys, estimate->bits_per_key);
	estimate->k = sjb_bloom_hashes_for(estimate->bits_per_key);
	estimate->filter_size = sizeof(SjbHeader) + ceil(estimate->m / 8.0);
	if (estimate->m > SJB_MAX_BITS)
	{
		skip_estimate(estimate, "filter needs more bits than the 64 MB limit");
		return;
	}
	if (estimate->filter_size > inputs->max_filter_bytes)
	{
		skip_estimate(estimate, "filter bigger than max_filter_bytes");
		return;
	}

	estimate->fp_rate = sjb_fp_rate(inputs->num_keys, estimate->m, estimate->k);
	estimate->bytes_a_to_b = sjb_bloom_filter_bytes(estimate->m, inputs->wire_factor);
	estimate->rows_back = matching_rows + non_matching_rows * estimate->fp_rate;
	set_filter_path_work(inputs, estimate);
	finish_estimate(inputs, estimate);
}

/*
 * estimate_exact
 *
 * What it does: the filter path with an EXACT filter (sorted keys). No
 * false positives, so exactly the M matching rows come back.
 * Skipped when the filter would be bigger than max_filter_bytes.
 * Inputs:  inputs, matching_rows (M)
 * Output:  none (fills *estimate)
 * Why it exists: one of the three filter modes.
 */
static void
estimate_exact(const SjbCostInputs *inputs, double matching_rows,
			   SjbCostEstimate *estimate)
{
	start_estimate(estimate, "exact");
	estimate->filter_size = sizeof(SjbHeader) + 4.0 * inputs->num_keys;
	if (estimate->filter_size > inputs->max_filter_bytes)
	{
		skip_estimate(estimate, "filter bigger than max_filter_bytes");
		return;
	}

	estimate->bytes_a_to_b = sjb_exact_filter_bytes(inputs->num_keys, inputs->wire_factor);
	estimate->rows_back = matching_rows;
	set_filter_path_work(inputs, estimate);
	finish_estimate(inputs, estimate);
}

/*
 * estimate_pass
 *
 * What it does: the filter path with a PASS filter (header only). Every
 * row passes, so all s_rows rows come back. It is never skipped: its
 * filter is only 24 bytes, so there is always at least one mode left.
 * Input:   inputs
 * Output:  none (fills *estimate)
 * Why it exists: when almost every row of S matches anyway, sending a
 * filter is a waste; PASS is then the cheapest.
 */
static void
estimate_pass(const SjbCostInputs *inputs, SjbCostEstimate *estimate)
{
	start_estimate(estimate, "pass");
	estimate->filter_size = sizeof(SjbHeader);
	estimate->bytes_a_to_b = sjb_pass_filter_bytes(inputs->wire_factor);
	estimate->rows_back = inputs->s_rows;
	set_filter_path_work(inputs, estimate);
	finish_estimate(inputs, estimate);
}

/*
 * sjb_estimate_all
 *
 * What it does: computes the four estimates (index path, BLOOM, EXACT,
 * PASS) and makes the two decisions:
 *   best_filter_mode: the allowed filter mode with the smallest time
 *                     (sjb_build_auto builds this one)
 *   index_path_wins:  true if the index path is faster than that best
 *                     filter mode (sjb_plan_choice reports this)
 * On a tie, the simpler choice wins: PASS before EXACT before BLOOM, and
 * the index path before the filter path.
 *
 * Inputs:  inputs - the statistics and settings
 * Output:  *choice is filled in
 *
 * Why it exists: this is the one place where both decisions are made.
 */
void
sjb_estimate_all(const SjbCostInputs *inputs, SjbChoice *choice)
{
	const SjbCostEstimate *best;

	memset(choice, 0, sizeof(SjbChoice));
	choice->matching_rows = sjb_matching_rows(inputs->num_keys, inputs->s_rows,
											  inputs->rows_per_key);

	estimate_index_path(inputs, choice->matching_rows, &choice->index_path);
	estimate_bloom(inputs, choice->matching_rows, &choice->bloom);
	estimate_exact(inputs, choice->matching_rows, &choice->exact);
	estimate_pass(inputs, &choice->pass);

	/* PASS is always allowed, so start with it. */
	choice->best_filter_mode = SJB_MODE_PASS;
	best = &choice->pass;

	if (choice->exact.allowed && choice->exact.total_seconds < best->total_seconds)
	{
		choice->best_filter_mode = SJB_MODE_EXACT;
		best = &choice->exact;
	}

	if (choice->bloom.allowed && choice->bloom.total_seconds < best->total_seconds)
	{
		choice->best_filter_mode = SJB_MODE_BLOOM;
		best = &choice->bloom;
	}

	choice->index_path_wins = (choice->index_path.total_seconds <= best->total_seconds);
}

/*
 * sjb_estimate_for_mode
 *
 * What it does: returns the estimate of one filter mode inside a choice.
 * Inputs:  choice, mode (SJB_MODE_BLOOM, SJB_MODE_EXACT or SJB_MODE_PASS)
 * Output:  a pointer to that estimate (inside *choice)
 * Why it exists: callers often need "the estimate of the winning mode".
 */
const SjbCostEstimate *
sjb_estimate_for_mode(const SjbChoice *choice, int mode)
{
	if (mode == SJB_MODE_BLOOM)
		return &choice->bloom;
	if (mode == SJB_MODE_EXACT)
		return &choice->exact;
	return &choice->pass;
}
