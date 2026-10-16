--
-- data_orders.sql
--
-- Batch 8: (re)creates the local table R = orders at site A (200,000
-- orders) and refreshes the statistics of both tables at site A. Called by
-- setup/make_data.sh. With the default arguments it makes exactly the same
-- rows as site_a_setup.sql.
--
-- Run inside site_a, with three psql variables:
--     s_rows          rows of customers at site B
--     match_fraction  share of the order keys that exist at site B (0..1]
--     skewed          false = uniform keys, true = skewed keys
--     psql -v ON_ERROR_STOP=1 -v s_rows=1000000 -v match_fraction=1.0 \
--          -v skewed=false -d site_a -f setup/data_orders.sql
--

-- Step 1: the largest key an order can have. Keys are drawn from
-- 1..max_key, and only 1..s_rows exist at site B, so a share of about
-- s_rows / max_key = match_fraction of the orders has a customer.
-- \gset stores the result column in the psql variable max_key.
SELECT round(:s_rows / :match_fraction)::int AS max_key \gset

-- Step 2: a fresh orders table, with the same columns as site_a_setup.sql.
DROP TABLE IF EXISTS orders;
CREATE TABLE orders (
    order_id    int PRIMARY KEY,
    customer_id int,
    amount      numeric
);

-- Step 3: fix the random seed so every run produces the same orders.
SELECT setseed(0.42);

-- Step 4: insert 200,000 orders.
-- Uniform: every key in 1..max_key is equally likely (with max_key =
--   1000000 this is the same expression as in site_a_setup.sql).
-- Skewed: random()^3 is mostly close to 0, so small keys come up far more
--   often than large ones (many orders share a few customers).
\if :skewed
INSERT INTO orders (order_id, customer_id, amount)
SELECT g,
       1 + floor(:max_key * random() ^ 3)::int,
       round((random() * 1000)::numeric, 2)
FROM generate_series(1, 200000) AS g;
\else
INSERT INTO orders (order_id, customer_id, amount)
SELECT g,
       (random() * (:max_key - 1))::int + 1,
       round((random() * 1000)::numeric, 2)
FROM generate_series(1, 200000) AS g;
\endif

-- Step 5: statistics. VACUUM (ANALYZE) orders reads the local table (and
-- keeps autovacuum quiet during measurements). ANALYZE customers makes
-- site A fetch a sample from site B: our planner hook reads the row count
-- and n_distinct of the FOREIGN table, so this must be redone whenever
-- site B's data changes.
VACUUM (ANALYZE) orders;
ANALYZE customers;

-- Step 6: show what we made.
SELECT :s_rows AS s_rows,
       :match_fraction AS match_fraction,
       :max_key AS max_key,
       :'skewed' AS skewed,
       count(*) AS orders,
       count(DISTINCT customer_id) AS distinct_keys
FROM orders;
