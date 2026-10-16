/*
 * sjbloom--1.0.sql
 *
 * Creates the SQL functions of the sjbloom extension. PostgreSQL runs this
 * file when you type CREATE EXTENSION sjbloom; in both site_a and site_b.
 *
 * Why IMMUTABLE matters: postgres_fdw only sends a WHERE condition to the
 * remote server (site B) if every function in it is IMMUTABLE. If
 * sjb_contains were not IMMUTABLE, site B would send ALL rows of S back
 * and site A would filter them itself, which defeats the whole project.
 *
 * Why STRICT matters: a STRICT function returns NULL whenever any argument
 * is NULL, without running the C code. So sjb_contains(filter, NULL) is
 * NULL, and "WHERE NULL" drops the row, exactly like a NULL key never
 * matching in an inner join. The C code never has to check for NULLs.
 */

-- Stop if someone runs this file directly with psql instead of CREATE EXTENSION.
\echo Use "CREATE EXTENSION sjbloom" to load this file. \quit

-- Step 1: sjb_build. Runs at site A: builds a Bloom filter (m bits, k
-- positions per key) from the join keys.
CREATE FUNCTION sjb_build(keys int4[], m int8, k int4)
RETURNS bytea
AS 'MODULE_PATHNAME', 'sjb_build'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Step 2: sjb_contains. Runs at site B inside the WHERE clause: true if the
-- key passes the filter (Bloom false positives are possible).
CREATE FUNCTION sjb_contains(filter bytea, key int4)
RETURNS bool
AS 'MODULE_PATHNAME', 'sjb_contains'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- Step 3: sjb_info. A debugging helper: describes a filter (any mode) in
-- one line.
CREATE FUNCTION sjb_info(filter bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'sjb_info'
LANGUAGE C IMMUTABLE STRICT;

/*
 * Step 4: sjb_best_bits. Cost model version 1: how many filter bits per
 * key (b) make the total network traffic as small as possible?
 *
 * Inputs:
 *   n           number of distinct join keys at site A (keys in the filter)
 *   s_rows      number of rows of S (customers) at site B
 *   matches     rows of S that really match a key (they come back anyway)
 *   row_width   bytes of one returned row, as text
 *   wire_factor how many bytes on the wire per filter byte. Default 2,
 *               because postgres_fdw sends the bytea filter as hex text
 *               (every byte becomes 2 hex characters).
 *
 * The total bytes we send and receive, for b bits per key, are:
 *
 *   filter bytes (A to B)       n * b / 8 * wire_factor
 * + true-match rows (B to A)    matches * row_width
 * + false-positive rows (B to A)(s_rows - matches) * row_width * fp(b)
 *
 * With the best number of hash functions (k = 0.693 * b), the Bloom
 * false-positive rate is fp(b) = e^(-0.4805 * b), where 0.4805 = (ln 2)^2.
 *
 * A bigger b makes the filter cost more but lets fewer false positives
 * through. The true-match rows do not depend on b. Setting the derivative
 * of the total to 0 and solving for b gives:
 *
 *   b* = ln( 8 * 0.4805 * (s_rows - matches) * row_width
 *            / (n * wire_factor) ) / 0.4805
 *
 * If the value inside ln() is 1 or less, b* would be 0 or negative: even
 * the smallest filter costs more than the false-positive rows it removes,
 * so a Bloom filter is not worth it and the function returns 0. It also
 * returns 0 when n <= 0 (no keys, so nothing to divide by).
 *
 * Example: sjb_best_bits(10000, 1000000, 10000, 100, 2) is about 20.5.
 */
CREATE FUNCTION sjb_best_bits(n float8,
                              s_rows float8,
                              matches float8,
                              row_width float8,
                              wire_factor float8 DEFAULT 2)
RETURNS float8
LANGUAGE sql IMMUTABLE PARALLEL SAFE
AS $$
    SELECT CASE
        -- No keys: there is no filter to size.
        WHEN n <= 0 THEN 0
        -- Value inside ln() is <= 1: Bloom is not worth it.
        WHEN 8 * 0.4805 * (s_rows - matches) * row_width / (n * wire_factor) <= 1 THEN 0
        -- Otherwise: the formula above.
        ELSE ln(8 * 0.4805 * (s_rows - matches) * row_width / (n * wire_factor)) / 0.4805
    END
$$;

/*
 * Step 5: sjb_build_auto (Batch 5). Runs at site A: the cost model picks
 * the filter mode (BLOOM, EXACT or PASS) and the Bloom size at RUNTIME,
 * when the exact number of distinct keys is known, then builds it.
 *
 * Inputs:
 *   keys             site A's join keys (NULLs and repeats are fine)
 *   s_rows           rows of S (customers) at site B
 *   rows_per_key     rows of S per key value (1 for a unique column)
 *   row_width        bytes of one returned row ON THE WIRE
 *                    (text width + 7 + 4 * number of columns)
 *   network_mbit     network speed in megabits per second
 *   t_check_ns       site B's time to scan + check one row (calibrated)
 *   max_filter_bytes BLOOM or EXACT filters bigger than this are not built
 *   wire_factor      wire bytes per filter byte (2: hex text)
 *
 * All settings are ARGUMENTS, never read from configuration settings, so
 * the function really is IMMUTABLE: the same inputs always give the same
 * filter. The formulas are in costmodel.c.
 */
CREATE FUNCTION sjb_build_auto(keys int4[],
                               s_rows float8,
                               rows_per_key float8,
                               row_width float8,
                               network_mbit float8,
                               t_check_ns float8,
                               max_filter_bytes int8 DEFAULT 1048576,
                               wire_factor float8 DEFAULT 2)
RETURNS bytea
AS 'MODULE_PATHNAME', 'sjb_build_auto'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/*
 * Step 6: sjb_choice_report (Batch 5). A readable multi-line report of the
 * cost model: for the INDEX path ("customer_id = ANY(key list)", n index
 * lookups at B) and for each filter mode ("sjb_contains", |S| checks at B)
 * it shows bytes A->B, rows back, bytes B->A, network time, work at B and
 * total time, then the winner. It runs the same C code as sjb_build_auto.
 *
 * n is the number of distinct keys. t_lookup_us is site B's time for one
 * index lookup (calibrated). max_filter_bytes and wire_factor are the
 * defaults of sjb_build_auto (1 MB and 2).
 */
CREATE FUNCTION sjb_choice_report(n float8,
                                  s_rows float8,
                                  rows_per_key float8,
                                  row_width float8,
                                  network_mbit float8,
                                  t_check_ns float8,
                                  t_lookup_us float8)
RETURNS text
AS 'MODULE_PATHNAME', 'sjb_choice_report'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

/*
 * Step 7: sjb_plan_choice (Batch 5). The same estimates as
 * sjb_choice_report, but only the decision: 'index' or 'filter'. Batch 6's
 * planner hook will call it, so the decision lives in one place.
 */
CREATE FUNCTION sjb_plan_choice(n float8,
                                s_rows float8,
                                rows_per_key float8,
                                row_width float8,
                                network_mbit float8,
                                t_check_ns float8,
                                t_lookup_us float8)
RETURNS text
AS 'MODULE_PATHNAME', 'sjb_plan_choice'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
