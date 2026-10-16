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

-- Step 3: sjb_info. A debugging helper: describes a filter in one line.
CREATE FUNCTION sjb_info(filter bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'sjb_info'
LANGUAGE C IMMUTABLE STRICT;
