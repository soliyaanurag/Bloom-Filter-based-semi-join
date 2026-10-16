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
