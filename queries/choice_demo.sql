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
-- network time is small, so the CPU work decides: at n = 10000, 10000
-- index lookups are cheaper than checking all 1,000,000 rows (index path
-- wins); at n = 50000 they are not (filter path wins). On the slow network
-- the bytes decide: the Bloom filter (about 2 * b/8 bytes per key as hex)
-- is smaller than the key list (about 7 bytes per key), so the filter path
-- wins at both n. Step 5 finds the break-even n on each network.
--
-- Run in site_a:  psql -X -d site_a -f queries/choice_demo.sql
--

-- Step 1: the calibrated constants (bench/calibrate.py,
-- results/calibration.json "suggested"). Override with -v on the command
-- line after recalibrating on your machine. per_message_ms, per_row_us,
-- t_build_us and fetch_size are not passed below, so the functions use
-- their defaults, which are our calibrated values (Batch 5b).
\if :{?t_check_ns}
\else
    \set t_check_ns 158.19
\endif
\if :{?t_lookup_us}
\else
    \set t_lookup_us 4.7956
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

-- Step 5 (Batch 5b): the break-even point. For n = 500, 1000, 1500, ...
-- keys, ask sjb_plan_choice which path wins, and show the smallest n for
-- which the filter path wins, on each network. index_wins_above counts the
-- n above it where the index path wins again (0 = it switches only once).
WITH choices AS (
    SELECT network_mbit,
           n,
           sjb_plan_choice(n, :s_rows, :rows_per_key, :row_width, network_mbit,
                           :t_check_ns, :t_lookup_us) AS choice
    FROM (VALUES (1000), (1)) AS networks(network_mbit),
         generate_series(500, 1000000, 500) AS n
),
break_even AS (
    SELECT network_mbit, min(n) AS first_n_filter_wins
    FROM choices
    WHERE choice = 'filter'
    GROUP BY network_mbit
)
SELECT b.network_mbit,
       b.first_n_filter_wins,
       (SELECT count(*) FROM choices c
        WHERE c.network_mbit = b.network_mbit
          AND c.n > b.first_n_filter_wins
          AND c.choice = 'index') AS index_wins_above
FROM break_even b
ORDER BY b.network_mbit DESC;
