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

## Batch 2: the sjbloom extension with a Bloom filter

In this batch we built a PostgreSQL extension in C called `sjbloom`. It adds three SQL functions:

| Function | Runs at | What it does |
|---|---|---|
| `sjb_build(keys int4[], m int8, k int4) → bytea` | site A | Builds a Bloom filter with `m` bits from a list of join keys. |
| `sjb_contains(filter bytea, key int4) → bool` | site B | Says whether a key passes the filter. |
| `sjb_info(filter bytea) → text` | anywhere | Describes a filter in one line, for debugging. |

Only the BLOOM mode works so far. EXACT and PASS come in Batch 5; until then `sjb_contains` gives the error "filter mode ... is not supported yet" for them.

### What is a Bloom filter?

A Bloom filter is a row of `m` bits, all 0 at the start.

- **Add a key:** compute `k` bit positions from the key and set those bits to 1.
- **Test a key:** compute the same `k` positions. If **any** of them is 0, the key was certainly never added. If **all** are 1, the key was probably added.

"Probably", because other keys may have set those same bits by chance. That is a **false positive**. A Bloom filter **never** gives a false negative: a key that was added always passes. For our join this is exactly what we need. Site B may send back a few extra rows of S, but never forgets a row that matches. The normal join at site A then throws the extra rows away.

### How one key becomes k bit positions

```
 key (int4), seed
        │
        ▼
 hashint4extended(key, seed)       PostgreSQL's own int4 hash, 64 bits
        │
        ▼
 ┌──────────── 64-bit hash ────────────┐
 │   high 32 bits    │   low 32 bits   │
 └─────────┬─────────┴────────┬────────┘
           ▼                  ▼
   h2 = high | 1          h1 = low       (h2 is made odd, so it is never 0)

 position_i = (h1 + i * h2) mod m        for i = 0, 1, ..., k-1
```

This trick is called **double hashing**: one hash call gives all `k` positions. Bit position `p` is stored in byte `p / 8`, at bit `p % 8` inside that byte.

Example with m = 64 and k = 3: if h1 = 10 and h2 = 7, the positions are 10, 17, 24. So bit 2 of byte 1, bit 1 of byte 2 and bit 0 of byte 3 are set.

We always use PostgreSQL's `hashint4extended` (never our own hash of raw bytes), so site A and site B compute exactly the same positions.

### Filter byte layout

A filter is a `bytea`. PostgreSQL puts its own length header in front, then come our 24-byte header and the payload:

```
 ┌─────────┬──────────────────────────────── SjbHeader (24 bytes) ───────────────────────────────┬────────────────────┐
 │ varlena │ version │ mode │    k    │  nkeys  │          m          │         seed          │  Bloom bits        │
 │ length  │ uint8   │ uint8│ uint16  │ uint32  │       uint64        │        uint64         │  ceil(m/8) bytes   │
 └─────────┴─────────┴──────┴─────────┴─────────┴─────────────────────┴───────────────────────┴────────────────────┘
  (PostgreSQL) offset 0  1      2         4         8                     16                     24
```

- `version` = 1, `mode` = 1 (BLOOM), `k` = positions per key, `nkeys` = how many (non-NULL) keys were added, `m` = number of bits, `seed` = hash seed (0x5EED5EED).
- The fields are ordered from small to large so the compiler adds no padding. The header is exactly 24 bytes, and a `StaticAssertDecl` makes the build fail if it ever is not.
- We always copy the header in and out with `memcpy`. Bytes inside a bytea may sit at any address, and reading a `uint64` from an unaligned address by casting a pointer can crash on some machines.

Example: `sjb_info(sjb_build(ARRAY[1,2,3], 64, 3))` gives
`mode=bloom nkeys=3 m=64 k=3 bits_set=12.50% bytes=32`. That is 24 header bytes + 8 bytes of bits; 3 keys × 3 positions = 9 positions, of which 8 distinct bits ended up set (12.5% of 64).

### Files

**`sjbloom/Makefile`** uses PGXS, PostgreSQL's build system for extensions. `pg_config` (from our install's `bin/`, which is on PATH) tells it where the headers are. `MODULE_big` + `OBJS` say "link sjbloom.o and bloom.o into sjbloom.so". `DATA` is the SQL install script, and `REGRESS` names the regression test.

**`sjbloom/sjbloom.control`** tells `CREATE EXTENSION` the default version (1.0) and where the library is (`$libdir/sjbloom`).

**`sjbloom/sjb_filter.h`** is shared by both C files. It holds the `SjbHeader` struct, the mode numbers, the seed, the allowed ranges for m (8 bits to 64 MB) and k (1 to 32), and the prototypes of the two functions in `bloom.c`.

**`sjbloom/bloom.c`** holds the pure Bloom filter logic, working on a plain array of bytes. It knows nothing about SQL.
- `compute_two_hashes` hashes a key and splits the result into h1 and h2.
- `bit_position` computes `(h1 + i*h2) mod m` in 64-bit arithmetic, so it cannot overflow.
- `set_bit` / `bit_is_set` implement the "byte p/8, bit p%8" rule.
- `sjb_bloom_add` sets the k bits of one key.
- `sjb_bloom_test` checks the k bits of one key and returns false at the first 0 bit.

**`sjbloom/sjbloom.c`** holds the three SQL functions, plus helpers that check inputs and filter headers:
- `sjb_build` checks m and k, unpacks the array, allocates a zero-filled bytea of the right size, adds every non-NULL key, then writes the header.
- `sjb_contains` copies the header out, checks the version, mode and payload size, then calls `sjb_bloom_test`. A damaged filter gives an error instead of reading past the end of the data.
- `sjb_info` does the same checks, counts the 1 bits with a simple double loop, and prints one line.

**`sjbloom/sjbloom--1.0.sql`** is run by `CREATE EXTENSION`. It declares the three functions. They are `IMMUTABLE` because postgres_fdw only ships a WHERE condition to the remote server if all its functions are immutable. Without this, site B would send all of S and the filter would be useless. They are `STRICT` so a NULL key automatically gives NULL, which drops the row, just like a NULL never matches in an inner join.

**`sjbloom/sql/sjbloom_basic.sql`** and **`sjbloom/expected/sjbloom_basic.out`** are the regression test and its expected output. It checks:
- no false negatives for 10000 keys;
- the false-positive rate is between 0.004 and 0.015 (theory: about 0.0082);
- an empty array gives nkeys = 0 and passes nothing;
- NULL array elements are skipped;
- a NULL key gives NULL;
- m = 8 works and m = 4 is an error.

**`queries/fp_rate.sql`** compares the measured false-positive rate with the theoretical rate `(1 - e^(-kn/m))^k` for 4, 8, 12, 16 and 22 bits per key, with `k = round(0.693 × b)`. It tests 10000 keys in the filter against 1,000,000 keys that are not in it.

A note on `WITH ... AS MATERIALIZED` in the SQL files: it forces PostgreSQL to build the filter once. Without it, the planner may copy the `sjb_build(...)` call into the WHERE clause and rebuild the filter for every row tested.

### PostgreSQL macros and functions we used

| Name | What it does |
|---|---|
| `PG_MODULE_MAGIC` | Puts a "magic block" in the .so so the server refuses a library built for a different major version. |
| `PG_FUNCTION_INFO_V1(name)` | Declares that C function `name` uses the standard calling convention for SQL functions. |
| `PG_FUNCTION_ARGS` | The standard parameter list of such a function (one pointer to the call info). |
| `PG_GETARG_INT32(n)`, `PG_GETARG_INT64(n)` | Read SQL argument number n as a C int32 / int64. |
| `PG_GETARG_ARRAYTYPE_P(n)` | Read argument n as an SQL array. |
| `PG_GETARG_BYTEA_PP(n)` | Read argument n as a bytea, decompressing / fetching it from TOAST if needed. It may keep a short 1-byte length header. |
| `deconstruct_array_builtin` | Unpack an array of a built-in type (here `INT4OID`) into a C array of Datums and a C array of null flags. |
| `Datum`, `Int32GetDatum`, `Int64GetDatum`, `DatumGetInt32`, `DatumGetUInt64` | A Datum is PostgreSQL's "any value" type; these macros wrap C values into Datums and unwrap them again. |
| `DirectFunctionCall2(f, a, b)` | Call an SQL-callable C function (here `hashint4extended`) directly from C with two arguments. |
| `palloc0(size)` | Allocate zero-filled memory in the current memory context. It is freed automatically at the end of the query. |
| `VARHDRSZ` | Size (4 bytes) of the length header in front of every bytea/text value. |
| `SET_VARSIZE(ptr, size)` | Write the total size into that length header. |
| `VARDATA(ptr)` | Pointer to the data just after a 4-byte length header (used on our own freshly built bytea). |
| `VARDATA_ANY(ptr)`, `VARSIZE_ANY_EXHDR(ptr)` | Pointer to the data and size of the data for a bytea with either a 1-byte or 4-byte length header. |
| `ereport(ERROR, (errcode(...), errmsg(...)))` | Raise an SQL error: stops the function and aborts the query. |
| `StaticAssertDecl(cond, msg)` | Check a condition at compile time. |
| `UINT64CONST(x)` | Write a 64-bit unsigned constant portably. |
| `psprintf` | Like sprintf, but returns a new palloc'd string. |
| `cstring_to_text` | Turn a C string into an SQL `text` value. |
| `PG_RETURN_BYTEA_P`, `PG_RETURN_BOOL`, `PG_RETURN_TEXT_P` | Return a value from the C function to SQL. |

### How to test it

```bash
cd sjbloom
make                 # compile (should print no warnings)
make install         # installs into ~/postgres/postgresql/install, no sudo needed
make installcheck    # runs sql/sjbloom_basic.sql, compares with expected/
```

`make installcheck` should end with `# All 1 tests passed.` If it fails, look at `sjbloom/regression.diffs`.

Then load the extension into both sites and run the false-positive experiment:

```bash
psql -d site_a -c "CREATE EXTENSION sjbloom;" -c "SELECT sjb_info(sjb_build(ARRAY[1,2,3], 64, 3));"
psql -d site_b -c "CREATE EXTENSION sjbloom;" -c "SELECT sjb_info(sjb_build(ARRAY[1,2,3], 64, 3));"
psql -d site_a -f queries/fp_rate.sql
```

Both sites should print `mode=bloom nkeys=3 m=64 k=3 bits_set=12.50% bytes=32`.

**Note:** `setup/setup_all.sh` drops and recreates both databases. Since Batch 3 it also creates sjbloom in both sites for you.

Results in site_a (the measured rates match the theory closely):

```
 bits_per_key |   m    | k  | measured_fp | predicted_fp
--------------+--------+----+-------------+--------------
            4 |  40000 |  3 |    0.145977 |     0.146892
            8 |  80000 |  6 |    0.021581 |     0.021577
           12 | 120000 |  8 |    0.003116 |     0.003142
           16 | 160000 | 11 |    0.000431 |     0.000459
           22 | 220000 | 15 |    0.000024 |     0.000026
```

Each extra 4 bits per key cuts the false-positive rate by roughly a factor of 7 to 10. This is the trade-off the cost model will use later: a bigger filter costs more bytes to send, but lets fewer useless rows of S come back.

After changing `sjbloom--1.0.sql`, reinstall and recreate the extension: `make install`, then `DROP EXTENSION sjbloom CASCADE; CREATE EXTENSION sjbloom;` in each database.

## Batch 3: the Bloom semi-join by hand, and cost model version 1

In this batch the Bloom filter crosses the network for the first time. We write the semi-join by hand in SQL (no changes to postgres_fdw yet), check that the result is exactly right, measure the bytes, and add a formula that picks the best filter size.

### Files

**`setup/enable_sjbloom.sql`** (run in site_a) makes sure sjbloom exists in site_b and site_a (`CREATE EXTENSION IF NOT EXISTS`, using `\connect` to switch databases), then runs:

```sql
ALTER SERVER site_b_srv OPTIONS (ADD extensions 'sjbloom');
```

postgres_fdw only sends a WHERE condition to site B if it is sure site B can run it. It trusts built-in functions, but for extension functions it only trusts the extensions named in the server option `extensions`. **Without this option**, sjb_contains stays at site A: B sends all 1,000,000 customers and A filters them (`Filter: sjb_contains(...)` above the Foreign Scan). The result would still be correct, but nothing would be saved on the network. Run the file only once: a second `ADD` fails because the option already exists.

**`setup/setup_all.sh`** now has 5 steps. After the 3 old ones it runs `CREATE EXTENSION sjbloom` in site_b and in site_a, then `enable_sjbloom.sql`. Before the very first run, install the extension once with `make -C sjbloom install`. During step 5 psql prints two `extension "sjbloom" already exists, skipping` notices. That is expected: step 4 already created it.

**`queries/bloom_manual.sql`** is the join with a Bloom semi-join, under `EXPLAIN (ANALYZE, VERBOSE)`. psql variables: `n` (orders kept, default 1000) and `b` (filter bits per key, default 10). The filter is built from the distinct customer_ids of the kept orders, with `m = max(8, b × keys)` bits and `k = max(1, round(0.693 × b))` positions per key. That k gives the lowest false-positive rate for b bits per key.

**`queries/correctness_bloom.sql`** runs **`queries/correctness_bloom_one.sql`** (with psql's `\ir`, "include a file from this folder") for n = 100, 1000 and 10000. Each run prints `baseline_except_bloom` and `bloom_except_baseline`, the two EXCEPT ALL counts. Both must be 0. You can change b with `-v b=4`.

**`queries/byte_estimate.sql`** estimates the network bytes for one n and b:
- `bytes_a_to_b` = `octet_length(filter::text)`: the filter as hex text, which is how postgres_fdw sends it (2 × binary size + 2 bytes for `\x`).
- `avg_row_width` = average of `octet_length(customer_id::text) + octet_length(name)` over all customers. Only these two columns travel, because the query only needs them. postgres_fdw runs this `avg(...)` at site B and gets back one number.
- `remote_rows` = how many customers pass the filter. We count them with the same pushed-down `sjb_contains` condition, so it equals the Foreign Scan's "actual rows" in `bloom_manual.sql` without copying numbers by hand.
- `bytes_b_to_a` = `remote_rows × avg_row_width`, and `baseline_bytes_b_to_a` = all 1,000,000 rows × width, for comparison.

Small protocol overheads (message headers, a few bytes per row) are ignored.

**`sjbloom/sjbloom--1.0.sql`** has a new SQL function **`sjb_best_bits(n, s_rows, matches, row_width, wire_factor DEFAULT 2)`**: cost model version 1 (see below). It is `LANGUAGE sql IMMUTABLE`, so no C code was needed. After `make install` the extension was recreated in both sites (here by rerunning `setup_all.sh`).

### Data flow of the manual Bloom query

```
 SITE A (site_a)                                          SITE B (site_b)
 ───────────────                                          ───────────────
 1. InitPlan 1 (runs once, before the join):
    SELECT sjb_build(array_agg(DISTINCT customer_id), m, k)
    FROM orders WHERE order_id <= n
      → ~1,000 distinct keys → bytea filter (1,273 bytes for n=1000, b=10)

 2. The filter becomes parameter $1.

 3. Foreign Scan sends the Remote SQL, with $1 as hex text ──────►
    SELECT customer_id, name FROM public.customers
    WHERE public.sjb_contains($1::bytea, customer_id)
                                                          4. B scans customers and keeps
                                                             only rows whose id passes the
                                                             filter: all ~1,000 real matches
                                                             + false positives.
                           ◄────── rows come back as text
 5. Hash Join (exact): orders ⋈ returned customers ON customer_id.
    False positives find no order and are dropped.
    → exactly the baseline result.
```

Step by step:
1. **InitPlan builds the filter.** The sub-SELECT inside `sjb_contains(...)` does not depend on the row of customers, so PostgreSQL runs it once as an *InitPlan*, before the join.
2. **The filter becomes `$1`.** The InitPlan's result is a query parameter.
3. **Remote SQL.** Because sjb_contains is IMMUTABLE, belongs to an extension listed in the server's `extensions` option, and its arguments are a parameter and a column, postgres_fdw puts the whole condition into the SQL it sends to B. The parameter travels as text, so the bytea filter is sent as hex.
4. **B filters.** Site B runs `sjb_contains` for each of its 1,000,000 rows and returns only the rows that pass, which is the approximate semi-join S ⋉ R'.
5. **Exact join at A.** The normal hash join on `customer_id` keeps only true matches, so Bloom false positives never reach the result.

### Results on our machine

`bloom_manual.sql` with n = 1000, b = 10 (shortened):

```
 Hash Join  (actual time=4.243..177.219 rows=1000.00 loops=1)
   Hash Cond: (c.customer_id = o.customer_id)
   InitPlan 1
     ->  Aggregate  (actual rows=1.00 loops=1)
           Output: sjb_build(array_agg(DISTINCT orders.customer_id), (GREATEST('8'::double precision, ceil(((count(DISTINCT orders.customer_id) * 10))::double precision)))::bigint, 7)
   ->  Foreign Scan on public.customers c  (cost=100.00..99756.93 rows=333333 width=15) (actual time=3.826..174.989 rows=9773.00 loops=1)
         Remote SQL: SELECT customer_id, name FROM public.customers WHERE (public.sjb_contains($1::bytea, customer_id))
 Execution Time: 179.901 ms
```

`sjb_contains($1::bytea, customer_id)` is inside the Remote SQL, and there is no `Filter:` line at A. Site B returned **9,773** rows instead of 1,000,000.

Why 9,773 and not "1,000 plus a few"? The false-positive rate (about 0.8% at b = 10) applies to **each of the 999,001 customers that do not match**. The filter had 50.69% of its bits set, so the predicted rate is 0.5069^7 ≈ 0.0086. 999,001 × 0.0086 ≈ 8,600 false positives, and we measured 8,774 (rate 0.0088). So b = 10 is too small for this data. That is exactly what the cost model below is for. With b = 20 only 80 false positives come back (see the table).

Also note the planner's guess `rows=333333`. It doesn't know what sjb_contains does, so it assumes a default selectivity of 1/3. Remember this for timing measurements later.

`correctness_bloom.sql` (b = 10): all six counts are 0.

```
   n   | baseline_except_bloom | bloom_except_baseline
-------+-----------------------+-----------------------
   100 |                     0 |                     0
  1000 |                     0 |                     0
 10000 |                     0 |                     0
```

`byte_estimate.sql` with n = 10000 (9,955 distinct keys; average row width 16.78 bytes as text):

```
  b | bytes_a_to_b | remote_rows | false_positives | bytes_b_to_a | total_bytes | baseline_bytes_b_to_a
----+--------------+-------------+-----------------+--------------+-------------+-----------------------
 10 |       24,938 |      17,921 |           7,966 |      300,675 |     325,613 |            16,777,792
 20 |       49,826 |      10,035 |              80 |      168,365 |     218,191 |            16,777,792
```

The `remote_rows` numbers match the Foreign Scan's actual rows in `bloom_manual.sql -v n=10000` (17,921 and 10,035). Doubling the filter costs 25 KB more A→B, but saves 132 KB B→A. Both are far below the 16.8 MB baseline.

### Cost model version 1: `sjb_best_bits`

For b bits per key, the bytes on the network are:

| Part | Bytes | Depends on b? |
|---|---|---|
| filter, A→B | n × b / 8 × wire_factor | grows with b |
| true-match rows, B→A | matches × row_width | no |
| false-positive rows, B→A | (s_rows − matches) × row_width × e^(−0.4805 b) | shrinks with b |

`e^(−0.4805 b)` is the Bloom false-positive rate with the best k (0.4805 = (ln 2)²). `wire_factor` = 2 because the filter travels as hex text. Setting the derivative of the total to 0 gives the b with the smallest total:

```
b* = ln( 8 × 0.4805 × (s_rows − matches) × row_width / (n × wire_factor) ) / 0.4805
```

If the value inside ln() is ≤ 1, even a tiny filter costs more than the false positives it would remove. The function then returns 0, meaning "Bloom is not worth it". It also returns 0 for n ≤ 0.

```
SELECT sjb_best_bits(10000, 1000000, 10000, 100, 1);   → 21.95
SELECT sjb_best_bits(10000, 1000000, 10000, 100, 2);   → 20.51
SELECT sjb_best_bits(1000000, 1000000, 900000, 1, 2);  → 0   (not worth it)
SELECT sjb_best_bits(9955, 1000000, 9955, 16.78);      → 16.80 (our real n=10000 query)
```

For our real query (rows are only ~17 bytes as text, since just customer_id and name travel), the best b is about 17. That is between the two runs above, and both are already close to the best total.

### How to test it

```bash
source setup/env.example.sh
make -C sjbloom install                  # after any change to the extension
setup/setup_all.sh                        # rebuilds both sites, sjbloom included (~5 s)
psql -d site_a -f queries/bloom_manual.sql                 # look for sjb_contains in Remote SQL
psql -d site_a -f queries/correctness_bloom.sql            # all counts must be 0
psql -d site_a -c "SELECT sjb_best_bits(10000, 1000000, 10000, 100, 1);"   # about 22
psql -d site_a -c "SELECT sjb_best_bits(10000, 1000000, 10000, 100, 2);"   # about 20.5
psql -d site_a -v n=10000 -v b=10 -f queries/byte_estimate.sql
psql -d site_a -v n=10000 -v b=20 -f queries/byte_estimate.sql
```

If the plan shows `Filter: sjb_contains(...)` *above* the Foreign Scan instead of inside the Remote SQL, check that (1) the `extensions 'sjbloom'` server option is set (`SELECT srvoptions FROM pg_foreign_server;`), (2) sjb_contains is IMMUTABLE, and (3) sjbloom is installed in schema public at site B.

## Batch 4: Python benchmarks and the mid-sem charts

In this batch we stop copying numbers out of psql by hand. Python scripts run every strategy for several values of n, measure rows, bytes and time, check correctness, and draw the charts.

### One-time setup: the Python environment

The scripts need two packages, listed in **`bench/requirements.txt`**: `psycopg[binary]` (psycopg 3, the PostgreSQL driver, with its own copy of libpq) and `matplotlib` (charts). They go into a *virtual environment* `.venv/` in the project folder (it is in `.gitignore`).

The usual commands are:

```bash
python3 -m venv .venv
.venv/bin/pip install -r bench/requirements.txt
```

On our WSL Ubuntu the first command failed with `ensurepip is not available`, because the Ubuntu package `python3.12-venv` is missing. There are two ways to fix it:

```bash
# Either install the missing package once (needs sudo), then use the commands above:
sudo apt install python3.12-venv

# or, without sudo: make the venv without pip, and use the system pip to fill it:
python3 -m venv --without-pip .venv
pip --python .venv/bin/python install -r bench/requirements.txt
```

We used the second way. Check it with `.venv/bin/python -c "import psycopg, matplotlib"`.

Connection settings come from the environment variables `PGHOST`, `PGPORT` and `PGUSER` (`source setup/env.example.sh`), and the password, if your server checks one, from `~/.pgpass`. libpq reads both by itself, so no password appears in the Python code.

### Files

**`bench/common.py`**: small helpers used by all scripts.
- `connect(dbname)` opens an autocommit connection to site_a or site_b.
- `explain_json(conn, sql)` runs `EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON)` and returns the plan as a Python dict. JSON is a tree we can walk, so we don't need to parse EXPLAIN text.
- `find_foreign_scan(plan)` walks the tree and returns the `Foreign Scan` node. `remote_rows(node)` = its actual rows × loops, `remote_sql(node)` = the SQL sent to site B, `total_time_ms(plan)` = planning time + execution time.
- `find_join_above_foreign_scan(plan)` / `join_type(plan)` find the join node above the Foreign Scan. That node is not always the direct parent: in a Hash Join the Foreign Scan sits under a `Hash` node. `is_nestloop_with_join_filter(plan)` is True for a Nested Loop that tests the join condition as a `Join Filter`, which compares every outer row with every inner row.
- `avg_row_width(width)` runs in site_b: the average text length of the columns that travel. narrow = `customer_id, name` (**16.78 bytes**), wide = `customer_id, name, padding` (**96.78 bytes**).
- `check_correct(conn, baseline_sql, new_sql)` is the project's correctness test: both `EXCEPT ALL` counts must be 0.
- `print_table(rows, columns)` prints the readable tables below.

**`bench/strategies.py`**: one function per strategy. Each returns `(sql_text, bytes_a_to_b_sql)`: the query, and a query that returns the bytes site A sends to B, measured as text because postgres_fdw sends parameters as text.

| Strategy | Extra WHERE condition | bytes A->B |
|---|---|---|
| `baseline(n, width)` | none | 0 |
| `exact_keys(n, width)` | `c.customer_id = ANY ((SELECT array_agg(DISTINCT ...))::int[])` | `octet_length(array::text)`, the `{12,345,...}` text postgres_fdw sends |
| `bloom_fixed10(n, width)` | `sjb_contains(filter, c.customer_id)`, b = 10 | `octet_length(filter::text)`, the hex text |
| `bloom_dynamic(n, b, width)` | same, with b from the cost model | same |

`width` picks the select list: "narrow" = `o.order_id, c.name`, "wide" also selects `c.padding`. `dynamic_bits(conn, n, row_width)` computes the b for bloom_dynamic *before* the query: `sjb_best_bits(n_distinct, s_rows, n_distinct, row_width, 2)`. Here `n_distinct` = distinct customer_ids of the kept orders, and `s_rows` = `pg_class.reltuples` of the foreign table at site_a (1,000,000). Matches = n_distinct, because every customer_id exists at B. Each width uses its own row width, so the wide variant gets a bigger b (bigger rows make false positives more expensive).

**`bench/run_experiments.py`**: for width in (narrow, wide), n in (100, 1000, 10000, 50000), and each strategy:
1. Run the query 3 times under EXPLAIN ANALYZE and keep the run with the **median** time.
2. From that run: `join_type` and `remote_rows`.
3. `bytes_a_to_b` from the strategy's bytes query. `bytes_b_to_a` = remote_rows × the row width **of this variant**. `total_bytes` = the sum.
4. `correct` = `check_correct(baseline, strategy)`.
5. If the join is a **Nested Loop with a Join Filter**, run the same case again after `SET enable_nestloop = off` and record that too (`nestloop_off = True`).

Before measuring, it runs one tiny query on the foreign table, so postgres_fdw's login to site B is not counted in the first measurement. Rows are **appended** to `results/results.csv`, with a `run_at` timestamp. The full table is printed at the end.

**`bench/b_sweep.py`**: n = 10000, b from 2 to 30, for both widths. It runs the Bloom query once per b and saves bytes A->B, bytes B->A and total bytes to `results/b_sweep.csv` (overwritten each run). It prints the predicted b* from `sjb_best_bits`, the measured minimum, and `model_total_bytes`, the total that cost model v1 predicts for each b.

**`bench/plot.py`**: draws the charts into `results/`. For each width: `total_bytes_vs_n_<width>.png`, `time_ms_vs_n_<width>.png`, `remote_rows_vs_n_<width>.png` and `total_bytes_vs_b_<width>.png` (vertical dashed line at the predicted b*). One more chart, `fp_rate.png`, comes from running `queries/fp_rate.sql` from Python (its rows are also saved to `results/fp_rate.csv`). Because results.csv is appended to, plot.py uses the **last** row of each case (strategy, width, n, nestloop_off). All charts use log axes, since the values span several powers of ten.

**`bench/run_all.sh`**: runs the three scripts in order (see "How to rerun" below).

**`bench/capture_bytes.sh`**: optional cross-check with a real packet capture. It runs nothing: it **prints** the `tcpdump` and `capinfos` commands (some need sudo) to capture one Bloom query. psql connects through the Unix socket (`PGHOST=/tmp`), so only postgres_fdw's TCP connection from site A to site B is captured. Expect the capture to be larger than our estimate, because our estimate ignores TCP/IP headers and PostgreSQL's per-row protocol overhead (about 15 bytes per row for 2 columns).

### Results on our machine

Every one of the 36 rows had `correct = True`. Times are in ms (median of 3); bytes are text bytes.

Narrow rows (row width 16.78 bytes):

```
     strategy |      n |     b |   join_type | nestloop_off | remote_rows | bytes_a_to_b | bytes_b_to_a | total_bytes |  time_ms
--------------+--------+-------+-------------+--------------+-------------+--------------+--------------+-------------+---------
     baseline |    100 |       |   Hash Join |        False |   1,000,000 |            0 |   16,777,792 |  16,777,792 | 2,079.15
   exact_keys |    100 |       | Nested Loop |        False |         100 |          693 |        1,678 |       2,371 |     2.83
   exact_keys |    100 |       |   Hash Join |         True |         100 |          693 |        1,678 |       2,371 |     1.69
bloom_fixed10 |    100 |    10 |   Hash Join |        False |      11,481 |          300 |      192,626 |     192,926 |   256.26
bloom_dynamic |    100 | 26.40 |   Hash Join |        False |         315 |          710 |        5,285 |       5,995 |   157.17
     baseline |  1,000 |       |   Hash Join |        False |   1,000,000 |            0 |   16,777,792 |  16,777,792 | 1,899.64
   exact_keys |  1,000 |       | Nested Loop |        False |         999 |        6,887 |       16,761 |      23,648 |   134.20
   exact_keys |  1,000 |       |   Hash Join |         True |         999 |        6,887 |       16,761 |      23,648 |     7.20
bloom_fixed10 |  1,000 |    10 |   Hash Join |        False |       9,773 |        2,548 |      163,969 |     166,517 |   211.68
bloom_dynamic |  1,000 | 21.60 |   Hash Join |        False |       1,077 |        5,446 |       18,070 |      23,516 |   154.59
     baseline | 10,000 |       |   Hash Join |        False |   1,000,000 |            0 |   16,777,792 |  16,777,792 | 1,924.10
   exact_keys | 10,000 |       |   Hash Join |        False |       9,955 |       68,605 |      167,023 |     235,628 |    65.67
bloom_fixed10 | 10,000 |    10 |   Hash Join |        False |      17,921 |       24,938 |      300,675 |     325,613 |   258.41
bloom_dynamic | 10,000 | 16.80 |   Hash Join |        False |      10,290 |       41,862 |      172,643 |     214,505 |   231.34
     baseline | 50,000 |       |   Hash Join |        False |   1,000,000 |            0 |   16,777,792 |  16,777,792 | 2,060.47
   exact_keys | 50,000 |       |   Hash Join |        False |      48,738 |      335,701 |      817,716 |   1,153,417 |   361.30
bloom_fixed10 | 50,000 |    10 |   Hash Join |        False |      56,495 |      121,896 |      947,861 |   1,069,757 |   388.57
bloom_dynamic | 50,000 | 13.41 |   Hash Join |        False |      50,269 |      163,446 |      843,403 |   1,006,849 |   362.33
```

Wide rows (row width 96.78 bytes): same remote rows for baseline, exact_keys and bloom_fixed10. bloom_dynamic picks bigger b values (30.04, 25.25, 20.45, 17.06). Total bytes at n = 10000: baseline 96,777,792; exact_keys 1,032,028; bloom_fixed10 1,759,293; bloom_dynamic 1,019,595.

What the numbers show:
- **Every filter beats the baseline by a lot.** At n = 10000 the baseline moves 16.8 MB (narrow) or 96.8 MB (wide); the filters move 0.2 to 1.8 MB.
- **A fixed b = 10 is a bad choice here.** S has 1,000,000 rows, so even a 0.8% false-positive rate lets about 8,000 extra rows through, more than the real matches for small n. bloom_dynamic uses more bits per key and comes back with close to the true number of rows.
- **Exact keys vs. Bloom.** For small n, the exact list is smallest (no false positives, and a short list). For large n with narrow rows, the dynamic Bloom filter wins: at n = 50000 it sends 163 KB instead of 336 KB, and its few false positives cost less than that difference. With wide rows, false positives are expensive, so the two are close.
- **Join method.** For exact_keys at n = 100 and 1000, the planner guessed only 10 rows from the Foreign Scan and chose a Nested Loop with a Join Filter. With `enable_nestloop = off` it uses a Hash Join and gets much faster (134 ms → 7 ms at n = 1000). The bytes do not change, only the time. The dashed line in `time_ms_vs_n_*.png` shows this.
- **Time.** The Bloom strategies take at least about 150 ms even for tiny n, because site B runs `sjb_contains` on all 1,000,000 rows. exact_keys with a Hash Join is faster, because `= ANY` is cheaper per row. All filter strategies are about 5 to 1,200 times faster than the baseline.

`b_sweep.py` (n = 10000):

```
narrow: measured minimum at b = 16 (214,124 bytes); predicted b* = 16.8
wide:   measured minimum at b = 20 (1,020,991 bytes); predicted b* = 20.45
```

The measured minimum lands right next to the predicted b* for both widths, and the cost model's predicted totals are within 1% of the measured ones for b ≥ 6. Near the minimum the curve is flat: at narrow width, b = 14 to 22 are all within 4% of the best.

`fp_rate.png`: the measured false-positive rates lie on the theoretical curve (same numbers as in Batch 2).

### How to rerun all experiments (one command)

```bash
source setup/env.example.sh
bench/run_all.sh
```

This needs the server running (`pg_ctl status`), both sites set up (`setup/setup_all.sh`), and the `.venv` from above. It takes about 5 minutes. It runs `run_experiments.py` (appends to `results/results.csv`), `b_sweep.py` (rewrites `results/b_sweep.csv`) and `plot.py` (rewrites the PNGs and `results/fp_rate.csv`). To start results.csv fresh, delete it first. The scripts can also be run one at a time, e.g. `.venv/bin/python bench/plot.py`.

`results/` is in `.gitignore`, so each run writes there without touching git. A frozen copy of the mid-sem CSVs and charts is committed in `docs/midsem/`.
