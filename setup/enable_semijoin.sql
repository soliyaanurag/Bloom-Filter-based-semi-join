--
-- enable_semijoin.sql
--
-- Batch 6: turns on the automatic semijoin rewrite of our modified
-- postgres_fdw (branch cs631-semijoin, patches/postgres_fdw-semijoin.patch)
-- for site_a. Run by setup_all.sh after enable_sjbloom.sql.
--
-- NOTE: this needs the MODIFIED postgres_fdw to be installed. With the
-- original postgres_fdw, step 2 fails with: invalid option "semijoin_filter".
--
-- Run while connected to site_a:
--     psql -v ON_ERROR_STOP=1 -d site_a -f setup/enable_semijoin.sql
--

-- Step 1: load postgres_fdw when every new session of site_a starts.
-- Our planner hook is installed when the library is loaded. Without this,
-- postgres_fdw is only loaded in the middle of planning the first query
-- that uses a foreign table, which is too late for that query.
-- (This only affects NEW connections to site_a.)
ALTER DATABASE site_a SET session_preload_libraries = 'postgres_fdw';

-- Step 2: opt in. The rewrite is only done for foreign tables whose server
-- (or the table itself) has semijoin_filter = true.
ALTER SERVER site_b_srv OPTIONS (ADD semijoin_filter 'true');

-- Step 3: show the result.
SELECT srvname, srvoptions
FROM pg_foreign_server
WHERE srvname = 'site_b_srv';
