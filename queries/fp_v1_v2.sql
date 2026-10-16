--
-- fp_v1_v2.sql   (TEMPORARY, branch fp-investigation)
--
-- Compares the false-positive rate of our Bloom filter with
--   v1 = classic double hashing      position_i = (h1 + i*h2) mod m
--   v2 = enhanced double hashing     (as in PostgreSQL's bloomfilter.c)
-- for the filter sizes that bloom_dynamic used in Batch 4, plus b = 10.
-- Everything else (hash function, m, k, bit layout) is identical.
--
--   m = ceil(n * b),  k = round(0.693 * b)
--   members      = keys 1 .. n (added to the filter)
--   non-members  = keys n+1 .. n+1,000,000 (tested; every pass is a false positive)
--   predicted    = 1,000,000 * (1 - e^(-k*n/m))^k
--
-- Needs: sjbloom built and installed from this branch (make -C sjbloom install),
-- so that sjbloom.so contains sjb_build_v2 / sjb_contains_v2.
--
-- Run in site_a:  psql -X -d site_a -f queries/fp_v1_v2.sql
--

-- Step 1: declare the v2 C functions as TEMPORARY functions. Functions in
-- the schema pg_temp exist only in this session and are dropped when it
-- ends, so the database is left exactly as before. They must be called
-- with the "pg_temp." prefix.
CREATE FUNCTION pg_temp.sjb_build_v2(keys int4[], m int8, k int4)
RETURNS bytea
AS '$libdir/sjbloom', 'sjb_build_v2'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pg_temp.sjb_contains_v2(filter bytea, key int4)
RETURNS bool
AS '$libdir/sjbloom', 'sjb_contains_v2'
LANGUAGE C IMMUTABLE STRICT;

-- Step 2: synthetic keys, one row per case.
WITH cases AS (
    SELECT n, b,
           ceil(n * b)::int8 AS m,
           round(0.693 * b)::int4 AS k
    FROM (VALUES (100, 26.4), (1000, 21.6), (10000, 16.8), (10000, 10.0)) AS c(n, b)
),
-- 2a: build one v1 and one v2 filter per case. MATERIALIZED: built once.
filters AS MATERIALIZED (
    SELECT n, b, m, k,
           sjb_build(ARRAY(SELECT generate_series(1, n)), m, k) AS filter_v1,
           pg_temp.sjb_build_v2(ARRAY(SELECT generate_series(1, n)), m, k) AS filter_v2
    FROM cases
),
-- 2b: test the 1,000,000 non-member keys against both filters.
measured AS (
    SELECT f.n, f.b, f.m, f.k,
           count(*) FILTER (WHERE sjb_contains(f.filter_v1, key)) AS measured_v1,
           count(*) FILTER (WHERE pg_temp.sjb_contains_v2(f.filter_v2, key)) AS measured_v2
    FROM filters AS f,
         generate_series(f.n + 1, f.n + 1000000) AS key
    GROUP BY f.n, f.b, f.m, f.k
)
-- 2c: false positives per 1,000,000 non-member keys.
SELECT n, b, m, k,
       round(1000000 * power(1 - exp(-k * n::numeric / m), k), 1) AS predicted_fp,
       measured_v1,
       measured_v2
FROM measured
ORDER BY n, b DESC;

-- Step 3: cross-check with the REAL keys of Batch 4 (bloom_dynamic, n = 100
-- and 1000): members = the distinct customer_ids of orders with
-- order_id <= n, non-members = every other id in 1 .. 1,000,000 (the
-- customers that should NOT come back). m uses the number of distinct keys,
-- exactly like bench/strategies.py. v1 should reproduce Batch 4's false
-- positives (remote_rows - distinct keys: 315 - 100 = 215, 1077 - 999 = 78).
WITH real_cases AS (
    SELECT n, b,
           ARRAY(SELECT DISTINCT customer_id FROM orders WHERE order_id <= n) AS keys
    FROM (VALUES (100, 26.4), (1000, 21.6)) AS c(n, b)
),
real_filters AS MATERIALIZED (
    SELECT n, b, keys,
           cardinality(keys) AS distinct_keys,
           greatest(8, ceil(cardinality(keys) * b))::int8 AS m,
           greatest(1, round(0.693 * b))::int4 AS k
    FROM real_cases
),
real_built AS MATERIALIZED (
    SELECT n, b, keys, distinct_keys, m, k,
           sjb_build(keys, m, k) AS filter_v1,
           pg_temp.sjb_build_v2(keys, m, k) AS filter_v2
    FROM real_filters
),
real_measured AS (
    SELECT r.n, r.b, r.distinct_keys, r.m, r.k,
           count(*) FILTER (WHERE sjb_contains(r.filter_v1, id)) AS measured_v1,
           count(*) FILTER (WHERE pg_temp.sjb_contains_v2(r.filter_v2, id)) AS measured_v2,
           count(*) AS non_members
    FROM real_built AS r,
         generate_series(1, 1000000) AS id
    WHERE id <> ALL (r.keys)
    GROUP BY r.n, r.b, r.distinct_keys, r.m, r.k
)
SELECT n, b, distinct_keys, m, k,
       round(non_members * power(1 - exp(-k * distinct_keys::numeric / m), k), 1) AS predicted_fp,
       measured_v1,
       measured_v2
FROM real_measured
ORDER BY n;
