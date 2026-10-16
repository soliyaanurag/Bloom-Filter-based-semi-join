--
-- param_nestloop.sql
--
-- postgres_fdw has a built-in alternative to fetching all of customers:
-- with use_remote_estimate it asks site B for cost estimates, and may
-- then choose a "parameterized" plan that sends one remote query per
-- order (WHERE customer_id = $1). Here we just look at what the planner
-- picks; it may or may not be a nested loop.
--
-- Run in site_a:  psql -d site_a -f queries/param_nestloop.sql
--

-- Step 1: n = how many orders to keep (order_id <= n). Default 1000.
\if :{?n}
\else
    \set n 1000
\endif

-- Step 2: start a transaction, so that if anything fails the server
-- option below is rolled back and never left switched on.
BEGIN;

-- Step 3: let postgres_fdw ask site B for cost estimates.
ALTER SERVER site_b_srv OPTIONS (ADD use_remote_estimate 'true');

-- Step 4: the baseline query again.
EXPLAIN (ANALYZE, VERBOSE)
SELECT o.order_id, c.name
FROM orders o
JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= :n;

-- Step 5: switch the option off again.
ALTER SERVER site_b_srv OPTIONS (DROP use_remote_estimate);

COMMIT;
