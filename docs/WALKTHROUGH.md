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

**Note:** `setup/setup_all.sh` drops and recreates both databases and does not create sjbloom. After running it, run the two `CREATE EXTENSION sjbloom;` commands above again.

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
