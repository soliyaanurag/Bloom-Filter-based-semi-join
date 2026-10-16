--
-- correctness_auto.sql
--
-- Checks that the automatic semijoin rewrite inside postgres_fdw (Batch 6)
-- never changes a query result: for n = 100, 10000 and 150000, and for
-- semijoin_force_path = auto, index and filter, a join query and a GROUP BY
-- query must give exactly the same rows with the rewrite on and off.
-- Every off_except_on and on_except_off printed must be 0.
--
-- The check itself is in correctness_auto_one.sql; this file only runs it
-- for each combination. (\ir = include a file from the same folder.)
--
-- Needs: postgres_fdw preloaded in site_a (session_preload_libraries) and
-- ALTER SERVER site_b_srv OPTIONS (ADD semijoin_filter 'true'), both done by
-- setup/setup_all.sh.
--
-- Run in site_a:  psql -d site_a -f queries/correctness_auto.sql
--

-- Step 1: show one NOTICE line per rewrite (terse = without the DETAIL),
-- so the output proves that the "on" runs really were rewritten.
\set VERBOSITY terse
SET postgres_fdw.semijoin_verbose = on;

-- Step 2: n = 100.
\set n 100
\set path auto
\ir correctness_auto_one.sql
\set path index
\ir correctness_auto_one.sql
\set path filter
\ir correctness_auto_one.sql

-- Step 3: n = 10000.
\set n 10000
\set path auto
\ir correctness_auto_one.sql
\set path index
\ir correctness_auto_one.sql
\set path filter
\ir correctness_auto_one.sql

-- Step 4: n = 150000.
\set n 150000
\set path auto
\ir correctness_auto_one.sql
\set path index
\ir correctness_auto_one.sql
\set path filter
\ir correctness_auto_one.sql
