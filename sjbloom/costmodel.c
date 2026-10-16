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
 *   time = (bytes A->B + bytes B->A) / bandwidth  +  work at site B
 *
 * (communication cost + local processing cost; Ozsu & Valduriez,
 * "Principles of Distributed Database Systems").
 *
 * Every function here is plain arithmetic on doubles: no SQL values, no
 * memory allocation. sjbloom.c calls sjb_estimate_all, and Batch 6's
 * planner hook will call it too, so all decisions are made in ONE place.
 *
 * What the model does NOT contain (on purpose, for now): work at site A
 * (scanning orders, building the filter, the final hash join) and a cost
 * per returned row (turning rows into text and back, postgres_fdw's
 * fetches of 100 rows at a time). bench/validate_model.py measures how
 * much these missing parts matter.
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
 * What it does: given bytes_a_to_b, rows_back and work_at_b_seconds,
 * fills in the rest:
 *   bytes_b_to_a    = rows_back * row_width
 *   network_seconds = (bytes_a_to_b + bytes_b_to_a) / bandwidth
 *   total_seconds   = network_seconds + work_at_b_seconds
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
	estimate->total_seconds = estimate->network_seconds + estimate->work_at_b_seconds;
}

/*
 * filter_path_work_seconds
 *
 * What it does: work at site B in the filter path. B must scan ALL rows
 * of S and call sjb_contains on each one (it cannot use its index):
 *   s_rows * t_check_ns * 1e-9   (ns -> seconds)
 * Input:   inputs
 * Output:  seconds
 * Why it exists: the same for all three filter modes.
 */
static double
filter_path_work_seconds(const SjbCostInputs *inputs)
{
	return inputs->s_rows * inputs->t_check_ns * 1e-9;
}

/*
 * estimate_index_path
 *
 * What it does: the index path. A sends the key list as text; B does one
 * index lookup per key (n * t_lookup_us); the M matching rows come back.
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
	estimate->work_at_b_seconds = inputs->num_keys * inputs->t_lookup_us * 1e-6;
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
	estimate->work_at_b_seconds = filter_path_work_seconds(inputs);
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
	estimate->work_at_b_seconds = filter_path_work_seconds(inputs);
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
	estimate->work_at_b_seconds = filter_path_work_seconds(inputs);
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
