--
-- correctness_auto_one.sql
--
-- One correctness check of the AUTOMATIC semijoin rewrite (Batch 6) for one
-- value of n and one value of postgres_fdw.semijoin_force_path.
-- Called by correctness_auto.sql, which sets n and path first.
--
-- Each query is run twice into a temp table: once with the rewrite off and
-- once with it on. A single "query EXCEPT ALL query" would not work: it is
-- a set operation, and the rewrite is never applied to set operations.
--
-- It prints one row per query:
--   off_rows / on_rows  number of rows of each run
--   off_except_on       rows of the original result missing with the rewrite
--   on_except_off       rows with the rewrite that are not in the original
-- EXCEPT ALL keeps duplicates, so both 0 means the results are identical.
--

-- Step 1: choose the path (auto = the cost model decides).
SET postgres_fdw.semijoin_force_path = :'path';

-- Step 2: the join query, without and with the rewrite.
SET postgres_fdw.enable_semijoin = off;
CREATE TEMP TABLE r_off AS
SELECT o.order_id, c.name
FROM orders o
JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= :n;

SET postgres_fdw.enable_semijoin = on;
CREATE TEMP TABLE r_on AS
SELECT o.order_id, c.name
FROM orders o
JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= :n;

-- Step 3: the GROUP BY query, without and with the rewrite.
SET postgres_fdw.enable_semijoin = off;
CREATE TEMP TABLE g_off AS
SELECT c.city, count(*) AS num_orders, sum(o.amount) AS total_amount
FROM orders o
JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= :n
GROUP BY c.city;

SET postgres_fdw.enable_semijoin = on;
CREATE TEMP TABLE g_on AS
SELECT c.city, count(*) AS num_orders, sum(o.amount) AS total_amount
FROM orders o
JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= :n
GROUP BY c.city;

-- Step 4: compare (with the rewrite off, so nothing here is rewritten).
SET postgres_fdw.enable_semijoin = off;
SELECT :n AS n,
       :'path' AS path,
       'join' AS query,
       (SELECT count(*) FROM r_off) AS off_rows,
       (SELECT count(*) FROM r_on) AS on_rows,
       (SELECT count(*) FROM (SELECT * FROM r_off EXCEPT ALL SELECT * FROM r_on) AS d) AS off_except_on,
       (SELECT count(*) FROM (SELECT * FROM r_on EXCEPT ALL SELECT * FROM r_off) AS d) AS on_except_off
UNION ALL
SELECT :n, :'path', 'group by',
       (SELECT count(*) FROM g_off),
       (SELECT count(*) FROM g_on),
       (SELECT count(*) FROM (SELECT * FROM g_off EXCEPT ALL SELECT * FROM g_on) AS d),
       (SELECT count(*) FROM (SELECT * FROM g_on EXCEPT ALL SELECT * FROM g_off) AS d);

-- Step 5: clean up for the next run.
DROP TABLE r_off, r_on, g_off, g_on;
RESET postgres_fdw.enable_semijoin;
RESET postgres_fdw.semijoin_force_path;
