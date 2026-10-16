--
-- choice_demo.sql
--
-- The presentation demo of the time-based cost model (Batch 5).
-- sjb_choice_report shows, for the INDEX path ("customer_id = ANY(key
-- list)", n index lookups at site B) and for each filter mode
-- ("sjb_contains(filter, customer_id)", all rows checked at site B), how
-- many bytes travel, how long the network and site B take, and the winner.
--
-- It runs for n = 10000 and n = 50000 keys, each on a fast network
-- (1000 Mbit/s) and a slow one (1 Mbit/s). On the fast network the
-- network time is tiny, so site B's work decides (n index lookups are
-- cheaper than checking all 1,000,000 rows): the index path wins. On the
-- slow network the bytes decide. The Bloom filter (about 2 * b/8 bytes
-- per key as hex) is smaller than the key list (about 7 bytes per key),
-- and the saving grows with n: at n = 10000 the two paths are almost tied,
-- at n = 50000 the filter path wins.
--
-- Run in site_a:  psql -X -d site_a -f queries/choice_demo.sql
--

-- Step 1: the calibrated constants of site B (bench/calibrate.py,
-- results/calibration.json "suggested"). Override with -v on the command
-- line after recalibrating on your machine.
\if :{?t_check_ns}
\else
    \set t_check_ns 182.62
\endif
\if :{?t_lookup_us}
\else
    \set t_lookup_us 1.6547
\endif

-- Step 2: statistics of customers from site_a's catalog
-- (sets :s_rows, :rows_per_key and :row_width).
\ir catalog_stats.sql

-- Step 3: print the reports as plain text (no table borders, no headers).
-- QUIET stops psql from announcing each \pset change.
\set QUIET on
\pset format unaligned
\pset tuples_only on

\echo
\echo '=== n = 10000, network 1000 Mbit/s ==='
SELECT sjb_choice_report(10000, :s_rows, :rows_per_key, :row_width, 1000, :t_check_ns, :t_lookup_us);

\echo
\echo '=== n = 10000, network 1 Mbit/s ==='
SELECT sjb_choice_report(10000, :s_rows, :rows_per_key, :row_width, 1, :t_check_ns, :t_lookup_us);

\echo
\echo '=== n = 50000, network 1000 Mbit/s ==='
SELECT sjb_choice_report(50000, :s_rows, :rows_per_key, :row_width, 1000, :t_check_ns, :t_lookup_us);

\echo
\echo '=== n = 50000, network 1 Mbit/s ==='
SELECT sjb_choice_report(50000, :s_rows, :rows_per_key, :row_width, 1, :t_check_ns, :t_lookup_us);

-- Step 4: back to the normal output format.
\pset format aligned
\pset tuples_only off
\set QUIET off
