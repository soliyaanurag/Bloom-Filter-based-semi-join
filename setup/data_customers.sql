--
-- data_customers.sql
--
-- Batch 8: (re)creates the remote table S = customers at site B with a
-- chosen number of rows. Called by setup/make_data.sh; with s_rows =
-- 1000000 it makes exactly the same rows as site_b_data.sql.
--
-- Run inside site_b, with the psql variable s_rows:
--     psql -v ON_ERROR_STOP=1 -v s_rows=1000000 -d site_b -f setup/data_customers.sql
--

-- Step 1: drop the old table. The foreign table at site_a only refers to
-- it by name, so it keeps working once the new table exists.
DROP TABLE IF EXISTS customers;

-- Step 2: the same columns as in site_b_data.sql. The primary key is added
-- in Step 4, after the rows: building an index once is faster than
-- updating it for every inserted row (it matters for 3,000,000 rows).
CREATE TABLE customers (
    customer_id int,
    name        text,
    city        text,
    padding     text
);

-- Step 3: customers with ids 1..s_rows, each row about 100 bytes wide.
INSERT INTO customers (customer_id, name, city, padding)
SELECT g,
       'cust_' || g,
       'city_' || (g % 100),
       repeat('x', 80)
FROM generate_series(1, :s_rows) AS g;

-- Step 4: the primary key (its index is called customers_pkey, as before).
ALTER TABLE customers ADD PRIMARY KEY (customer_id);

-- Step 5: VACUUM sets the hint bits and the visibility map now, so that
-- autovacuum does not start in the middle of a measurement. ANALYZE
-- collects the statistics.
VACUUM (ANALYZE) customers;
