--
-- correctness_exact.sql
--
-- Checks that the exact-key query (exact_keys.sql) returns exactly the
-- same rows as the baseline query (baseline.sql), duplicates included.
-- EXCEPT ALL keeps every row of the left side that has no partner on the
-- right side. Both counts must be 0.
-- (The ::int[] cast is explained in exact_keys.sql.)
--
-- Run in site_a:  psql -d site_a -f queries/correctness_exact.sql
--

-- Step 1: n = how many orders to keep (order_id <= n). Default 1000.
\if :{?n}
\else
    \set n 1000
\endif

-- Step 2: rows of baseline that are missing from exact_keys (must be 0).
SELECT count(*) AS baseline_except_exact
FROM (
    SELECT o.order_id, c.name
    FROM orders o
    JOIN customers c ON c.customer_id = o.customer_id
    WHERE o.order_id <= :n
  EXCEPT ALL
    SELECT o.order_id, c.name
    FROM orders o
    JOIN customers c ON c.customer_id = o.customer_id
    WHERE o.order_id <= :n
      AND c.customer_id = ANY ((SELECT array_agg(DISTINCT customer_id)
                                FROM orders
                                WHERE order_id <= :n)::int[])
) AS missing_rows;

-- Step 3: rows of exact_keys that are not in baseline (must be 0).
SELECT count(*) AS exact_except_baseline
FROM (
    SELECT o.order_id, c.name
    FROM orders o
    JOIN customers c ON c.customer_id = o.customer_id
    WHERE o.order_id <= :n
      AND c.customer_id = ANY ((SELECT array_agg(DISTINCT customer_id)
                                FROM orders
                                WHERE order_id <= :n)::int[])
  EXCEPT ALL
    SELECT o.order_id, c.name
    FROM orders o
    JOIN customers c ON c.customer_id = o.customer_id
    WHERE o.order_id <= :n
) AS extra_rows;
