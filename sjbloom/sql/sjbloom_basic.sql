--
-- sjbloom_basic.sql
--
-- Regression test for the Bloom filter of the sjbloom extension.
-- Run with:  make installcheck   (inside sjbloom/)
-- Every check prints one boolean or one number, so the expected output
-- is easy to read.
--
-- Note: several checks use "WITH ... AS MATERIALIZED". This makes
-- PostgreSQL build the filter once and reuse it for every row. Without
-- MATERIALIZED the planner may copy the sjb_build(...) call into the WHERE
-- clause and rebuild the filter for every single row.
--

-- Step 1: load the extension into the test database.
CREATE EXTENSION sjbloom;

-- Step 2: no false negatives. Build a filter of keys 1..10000
-- (m = 100000 bits, k = 7). Every one of those keys must pass,
-- so the count of keys that do NOT pass must be 0.
WITH f AS MATERIALIZED (
    SELECT sjb_build(ARRAY(SELECT generate_series(1, 10000)), 100000, 7) AS filter
)
SELECT count(*) AS false_negatives
FROM f, generate_series(1, 10000) AS key
WHERE NOT sjb_contains(f.filter, key);

-- Step 3: false-positive rate. Test 1,000,000 keys that were NOT added
-- (10001..1010000) against the same filter. Theory predicts about 0.0082,
-- so the measured rate must be between 0.004 and 0.015.
WITH f AS MATERIALIZED (
    SELECT sjb_build(ARRAY(SELECT generate_series(1, 10000)), 100000, 7) AS filter
),
measured AS (
    SELECT count(*) FILTER (WHERE sjb_contains(f.filter, key)) / 1000000.0 AS fp_rate
    FROM f, generate_series(10001, 1010000) AS key
)
SELECT fp_rate BETWEEN 0.004 AND 0.015 AS fp_rate_ok
FROM measured;

-- Step 4: an empty array gives a filter with nkeys = 0 ...
SELECT sjb_info(sjb_build('{}'::int4[], 1000, 3)) LIKE 'mode=bloom nkeys=0 %' AS empty_has_zero_keys;

-- ... and no key passes an empty filter (all bits are 0), so this is false.
SELECT sjb_contains(sjb_build('{}'::int4[], 1000, 3), 42) AS empty_contains_42;

-- Step 5: NULL elements in the array are skipped, so nkeys = 2 here.
SELECT sjb_info(sjb_build(ARRAY[1, NULL, 3], 1000, 3)) LIKE 'mode=bloom nkeys=2 %' AS null_elements_skipped;

-- Step 6: a NULL key gives NULL (sjb_contains is STRICT).
SELECT sjb_contains(sjb_build(ARRAY[1, 2, 3], 1000, 3), NULL) IS NULL AS null_key_gives_null;

-- Step 7: the smallest allowed filter, m = 8 bits, works.
SELECT sjb_contains(sjb_build(ARRAY[1, 2, 3], 8, 3), 2) AS m8_works;

-- Step 8: m = 4 is too small and must give an error.
SELECT sjb_build(ARRAY[1, 2, 3], 4, 3);

--
-- Batch 5: sjb_build_auto, the EXACT and PASS modes, and the cost model.
--
-- In these tests the arguments of sjb_build_auto are
--   (keys, s_rows, rows_per_key, row_width, network_mbit, t_check_ns)
-- and we choose them so that each mode wins. Inside the filter path the
-- work at site B (s_rows * t_check_ns) is the same for every mode, so the
-- mode with the fewest network bytes wins.
--

-- Step 9: zero keys give an EXACT filter with no keys ...
SELECT sjb_info(sjb_build_auto('{}'::int4[], 1000000, 1, 30, 1000, 100)) AS zero_keys_info;

-- ... and an array of only NULLs counts as zero keys too ...
SELECT sjb_info(sjb_build_auto(ARRAY[NULL, NULL]::int4[], 1000000, 1, 30, 1000, 100)) AS only_nulls_info;

-- ... and nothing passes it (the join with an empty R' is empty).
SELECT count(*) AS zero_keys_passing
FROM generate_series(-1000, 1000) AS key
WHERE sjb_contains(sjb_build_auto('{}'::int4[], 1000000, 1, 30, 1000, 100), key);

-- Step 10: sjb_build_auto removes NULLs and repeated keys and sorts the
-- rest, so nkeys = 3, min_key = 1, max_key = 3.
SELECT sjb_info(sjb_build_auto(ARRAY[3, 1, NULL, 3, 2], 1e12, 1, 30, 1000, 100)) AS dedup_info;

-- Step 11: which mode wins, for keys 1..5000.
--   s_rows = 1,000,000: a Bloom filter of about 19 bits per key is
--     smaller than 4 bytes (32 bits) per key, so BLOOM wins.
--   s_rows = 10^12: false positives are so expensive that Bloom needs
--     about 48 bits per key, more than EXACT's 32, so EXACT wins.
--   s_rows = 5,000 (every row of S matches): any filter is a waste, PASS.
--   s_rows = 10^12 but max_filter_bytes = 10000: EXACT (20,024 bytes) and
--     BLOOM (about 30,000 bytes) are both too big, so PASS is the only
--     mode left.
SELECT 's_rows = 1e6' AS test_case,
       sjb_info(sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 1e6, 1, 30, 1000, 100)) AS info
UNION ALL
SELECT 's_rows = 1e12',
       sjb_info(sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 1e12, 1, 30, 1000, 100))
UNION ALL
SELECT 's_rows = num_keys',
       sjb_info(sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 5000, 1, 30, 1000, 100))
UNION ALL
SELECT 'max_filter_bytes = 10000',
       sjb_info(sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 1e12, 1, 30, 1000, 100, 10000));

-- Step 12: no false negatives in any mode: keys 1..5000 must all pass the
-- BLOOM, EXACT and PASS filters built from them.
WITH filters AS MATERIALIZED (
    SELECT 'bloom' AS expected_mode,
           sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 1e6, 1, 30, 1000, 100) AS filter
    UNION ALL
    SELECT 'exact',
           sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 1e12, 1, 30, 1000, 100)
    UNION ALL
    SELECT 'pass',
           sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 5000, 1, 30, 1000, 100)
)
SELECT f.expected_mode,
       split_part(sjb_info(f.filter), ' ', 1) AS actual_mode,
       count(*) FILTER (WHERE NOT sjb_contains(f.filter, key)) AS false_negatives
FROM filters f, generate_series(1, 5000) AS key
GROUP BY f.expected_mode, f.filter
ORDER BY f.expected_mode;

-- Step 13: EXACT has no false positives: none of the keys 5001..1005000,
-- nor 0 or negative keys, pass the EXACT filter of keys 1..5000.
WITH f AS MATERIALIZED (
    SELECT sjb_build_auto(ARRAY(SELECT generate_series(1, 5000)), 1e12, 1, 30, 1000, 100) AS filter
)
SELECT count(*) AS exact_false_positives
FROM f, generate_series(-1000, 1005000) AS key
WHERE (key < 1 OR key > 5000) AND sjb_contains(f.filter, key);

-- Step 14: the binary search works with the smallest and largest int4 and
-- with negative keys. Only the 5 keys in the filter pass.
WITH f AS MATERIALIZED (
    SELECT sjb_build_auto(ARRAY[7, -5, -2147483648, 2147483647, 0], 1e12, 1, 30, 1000, 100) AS filter
)
SELECT sjb_info(f.filter) AS info,
       array_agg(key ORDER BY key) FILTER (WHERE sjb_contains(f.filter, key)) AS passing_keys
FROM f, unnest(ARRAY[-2147483648, -2147483647, -6, -5, -4, 0, 1, 6, 7, 8,
                     2147483646, 2147483647]) AS key
GROUP BY f.filter;

-- Step 15: the cost model report. n = 10000 keys, |S| = 1,000,000, one
-- row per key, rows of 32 bytes, 1000 Mbit/s, 150 ns per check, 2 us per
-- index lookup. 10000 lookups (20 ms) are much cheaper than 1,000,000
-- checks (150 ms), so the index path wins.
SELECT sjb_choice_report(10000, 1e6, 1, 32, 1000, 150, 2) AS report;

-- Step 16: sjb_plan_choice gives the same decision as the report ...
SELECT sjb_plan_choice(10000, 1e6, 1, 32, 1000, 150, 2) AS fast_network;

-- ... and on a slow network (1 Mbit/s) with many keys (200000), the Bloom
-- filter's smaller bytes A->B make the filter path win.
SELECT sjb_plan_choice(200000, 1e6, 1, 32, 1, 150, 2) AS slow_network;

-- Step 17: invalid arguments give clear errors.
SELECT sjb_build_auto(ARRAY[1, 2, 3], 1e6, 1, 30, 0, 100);
SELECT sjb_choice_report(-1, 1e6, 1, 30, 1000, 100, 2);

-- Step 18: an unknown mode number (here 9, written into byte 1) is
-- rejected, never read.
SELECT sjb_contains(set_byte(sjb_build_auto(ARRAY[1, 2, 3], 1e12, 1, 30, 1000, 100), 1, 9), 2);
