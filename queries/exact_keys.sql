--
-- exact_keys.sql
--
-- The same join, but we also send site B the exact list of customer_ids
-- we need. This is a hand-written semi-join: only matching customers come
-- back. (Our extension will later do this automatically, and also with a
-- Bloom filter.)
--
-- Run in site_a:  psql -d site_a -f queries/exact_keys.sql
--                 psql -d site_a -v n=5000 -f queries/exact_keys.sql
--

-- Step 1: n = how many orders to keep (order_id <= n). Default 1000.
\if :{?n}
\else
    \set n 1000
\endif

-- Step 2: show the plan. The sub-SELECT is computed once at site A
-- (an "InitPlan"), and its result, an int[] array, is sent to site B as a
-- query parameter ($1), so B only returns customers in that list.
--
-- Why the ::int[] cast: without it, PostgreSQL reads "= ANY ((SELECT ...))"
-- as "= ANY (subquery)", which compares customer_id with each ROW of the
-- subquery (one int[] row) and fails with "operator does not exist:
-- integer = integer[]". The cast makes the sub-SELECT a plain array value.
EXPLAIN (ANALYZE, VERBOSE)
SELECT o.order_id, c.name
FROM orders o
JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= :n
  AND c.customer_id = ANY ((SELECT array_agg(DISTINCT customer_id)
                            FROM orders
                            WHERE order_id <= :n)::int[]);
