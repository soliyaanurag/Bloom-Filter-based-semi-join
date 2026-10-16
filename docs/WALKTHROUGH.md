<!-- Step-by-step walkthrough of each batch: what was built, file by file, and how to test it. -->

# Project walkthrough

## Batch 1: the two sites, the data, and the baseline queries

In this batch we built the test environment and measured what postgres_fdw does **without** our extension. No C code yet.

### The setup

```
        site_a (database)                               site_b (database)
 ┌──────────────────────────────┐                ┌──────────────────────────────┐
 │ orders      200,000 rows     │                │ customers   1,000,000 rows   │
 │ (local table R)              │   postgres_fdw │ (table S, ~100 bytes / row)  │
 │                              │ ─────────────► │                              │
 │ customers   foreign table    │  TCP localhost │                              │
 │ (no data, just a "window")   │ ◄───────────── │                              │
 └──────────────────────────────┘     rows       └──────────────────────────────┘
```

Both "sites" are databases on the same PostgreSQL server. postgres_fdw still connects from site_a to site_b over a real TCP connection (localhost:5432), so every row of customers that site A reads really travels through a network socket.

### Files

**`setup/env.example.sh`** sets `PGHOST=localhost`, `PGPORT=5432`, `PGUSER=postgres`. Load it with `source setup/env.example.sh`. Passwords never go in this file. Put yours in `~/.pgpass` (`chmod 600`), and give it to `setup_all.sh` once through `PGPASSWORD` (see the comments in the file). On our machine the server uses `trust` authentication, so no password is needed.

**`setup/create_sites.sql`** drops site_a and site_b if they exist (`WITH (FORCE)` kicks out old connections), then creates them empty.

**`setup/site_b_data.sql`** creates `customers(customer_id, name, city, padding)` in site_b with 1,000,000 rows. The `padding` column (80 × `'x'`) makes each row about 100 bytes, so transferring rows has a real cost.

**`setup/site_a_setup.sql`** runs in site_a:
- **orders**: 200,000 orders, each pointing at a random customer between 1 and 1,000,000. `setseed(0.42)` makes the "random" data the same on every run.
- **`CREATE EXTENSION postgres_fdw`** installs the *foreign data wrapper*: code that reads tables on another PostgreSQL server by sending it SQL.
- **`CREATE SERVER site_b_srv`** records *where* the other site is (host, port, database).
- **`CREATE USER MAPPING`** records *who* to log in as at site B. The password comes in as the psql variable `pgpass`, never from the file.
- **`IMPORT FOREIGN SCHEMA ... LIMIT TO (customers)`** creates a local *foreign table* `customers` with the same columns as the remote one. It stores no data: every query on it becomes SQL sent to site B.
- **`ANALYZE orders; ANALYZE customers;`** collects statistics. For the foreign table, site A fetches a sample of rows from site B, so its planner knows the remote table has about 1,000,000 rows.

**`setup/setup_all.sh`** runs the three SQL files in order. `set -e` stops the script at the first failing command, and `psql -v ON_ERROR_STOP=1` stops each SQL file at its first error. **Warning:** it drops and recreates both databases.

**`queries/baseline.sql`** is the plain join `orders ⋈ customers` for `order_id <= n`, under `EXPLAIN (ANALYZE, VERBOSE)`. `n` is a psql variable (default 1000; change it with `psql -v n=5000 ...`).

**`queries/exact_keys.sql`** is the same join, plus a hand-written semi-join: `c.customer_id = ANY ((SELECT array_agg(DISTINCT customer_id) FROM orders WHERE order_id <= n)::int[])`. Site A computes the list of needed customer ids once and sends it to site B. The `::int[]` cast is needed: without it, PostgreSQL reads `= ANY ((SELECT ...))` as "compare with each *row* of a subquery". It then fails with `operator does not exist: integer = integer[]`.

**`queries/param_nestloop.sql`** turns on the server option `use_remote_estimate` (postgres_fdw then asks site B for cost estimates), runs the baseline query, and turns it off again. Everything is inside `BEGIN ... COMMIT`, so if something fails the option is rolled back and never left on.

**`queries/correctness_exact.sql`** prints `(baseline EXCEPT ALL exact_keys)` and `(exact_keys EXCEPT ALL baseline)`. `EXCEPT ALL` keeps duplicates, so if both counts are 0 the two queries return exactly the same rows, the same number of times. This is the correctness test we will use for every strategy.

### How to read the EXPLAIN output

`EXPLAIN (ANALYZE, VERBOSE)` runs the query and prints the plan as a tree. Each `->` is a child node that feeds rows to the node above it. Every node shows two sets of numbers:

```
Foreign Scan on public.customers c  (cost=100.00..237257.00 rows=1000000 width=15) (actual time=1.064..1206.720 rows=1000000.00 loops=1)
                                     └──────── planner's GUESS ───────────────┘  └──────────── what REALLY happened ─────────────────┘
```

- **cost / rows / width** (first parentheses): the planner's estimate before running.
- **actual time / rows / loops** (second parentheses): measured. `actual rows` is the number of rows the node produced *per loop*, and `loops` is how many times the node ran. Total rows = rows × loops.
- **Foreign Scan**: the node that reads a foreign table. Its rows come from site B over the network. **This is the number our project wants to make small.**
- **Remote SQL:** (only with `VERBOSE`): the exact SQL text site A sent to site B. If a condition appears in its `WHERE`, site B filters the rows. If not, site B sends everything and site A filters.
- **InitPlan**: a sub-query that does not depend on the outer rows, so it is computed **once**, before the main plan starts. Its result is passed to the rest of the plan as a parameter (`$1`, ...). postgres_fdw can ship such a parameter to site B.
- **Output:** the columns a node produces. **Join Filter / Rows Removed by Join Filter**: a join condition checked row by row, and how many row pairs it threw away.

### Results on our machine (n = 1000)

**1. Baseline: all of customers is fetched.**
```
Hash Join  (...) (actual time=3.244..1314.900 rows=1000.00 loops=1)
  ->  Foreign Scan on public.customers c  (...) (actual time=1.064..1206.720 rows=1000000.00 loops=1)
        Remote SQL: SELECT customer_id, name FROM public.customers
Execution Time: 1318.242 ms
```
The Remote SQL has **no WHERE clause**, so site B sends all **1,000,000** rows to answer a query with 1000 result rows. postgres_fdw cannot push the join condition down, because the other side of the join (orders) lives at site A.

**2. Exact keys: only the needed customers are fetched.**
```
Nested Loop  (...) (actual time=10.692..125.596 rows=1000.00 loops=1)
  InitPlan 1
    ->  Aggregate  (...) (actual ... rows=1.00 loops=1)
          Output: array_agg(DISTINCT orders.customer_id)
  ...
  ->  Foreign Scan on public.customers c  (...) (actual time=4.579..11.715 rows=999.00 loops=1)
        Remote SQL: SELECT customer_id, name FROM public.customers WHERE ((customer_id = ANY ($1::integer[])))
Execution Time: 128.828 ms
```
`InitPlan 1` computes the array of distinct customer ids once. It goes to site B as `$1`, and B returns only **999** rows (1000 orders reference 999 distinct customers), 10× faster. One weakness is visible: the planner guessed only `rows=10` from the foreign scan, so it picked a simple nested loop that compares 1000 × 999 row pairs (`Rows Removed by Join Filter: 998000`). It is still fast here, but a cost model with good estimates matters. Sending a full key list also gets expensive when there are many keys, which is why we will add Bloom filters.

**3. use_remote_estimate: a parameterized nested loop.**
```
Nested Loop  (...) (actual time=0.311..223.948 rows=1000.00 loops=1)
  ->  Index Scan using orders_pkey on public.orders o  (...) (actual ... rows=1000.00 loops=1)
  ->  Memoize  (...) (actual ... rows=1.00 loops=1000)
        Cache Key: o.customer_id
        Hits: 1  Misses: 999  ...
        ->  Foreign Scan on public.customers c  (...) (actual ... rows=1.00 loops=999)
              Remote SQL: SELECT customer_id, name FROM public.customers WHERE ((customer_id = $1::integer))
Execution Time: 224.365 ms
```
In simple words: with real estimates from site B, the planner decided to walk through the 1000 orders and, **for each order, send one small query to site B** asking for that single customer (`customer_id = $1`). `Memoize` remembers answers it already has: 1 cache hit, 999 misses, so **999 separate remote queries**, each returning 1 row. This moves very little data, but every query is a network round trip. That is fine for 1000 keys and terrible for 100,000. Our filter approach sends **one** query, whatever the number of keys.

**4. Correctness:** `baseline_except_exact = 0` and `exact_except_baseline = 0`.

### How to test it

```bash
source setup/env.example.sh
setup/setup_all.sh                                   # should end with "=== Setup finished ==="
psql -d site_a -f queries/baseline.sql               # Remote SQL without WHERE, ~1,000,000 rows
psql -d site_a -f queries/exact_keys.sql             # Remote SQL with = ANY ($1...), ~1,000 rows
psql -d site_a -f queries/param_nestloop.sql         # plan with use_remote_estimate
psql -d site_a -f queries/correctness_exact.sql      # both counts must be 0
psql -d site_a -v n=10000 -f queries/baseline.sql    # try other sizes with -v n=...
```
