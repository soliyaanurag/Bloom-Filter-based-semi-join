# Manual test guide: check for yourself that the Bloom semijoin works

This guide takes you from an empty Ubuntu (or WSL2 Ubuntu) machine to a full check of the project. Each step says **what to run**, **what you should see**, and **why** you should see it, so that while you run it you also understand why the code is correct.

The expected outputs come from my own run on 2026-10-17 (report: [verification_report.md](verification_report.md)). Numbers marked "≈" depend on your machine or on ANALYZE's random sample and will differ a little. Numbers without "≈" are exact: if yours differ, something is wrong.

**Conventions**
- `$` lines run in a bash terminal. Lines without `$` inside an SQL block run in `psql`.
- The repository is cloned to `~/postgre-project` and PostgreSQL lives in `~/postgres/postgresql`, as in the README.
- Total time: about 30 to 45 minutes, most of it compiling PostgreSQL. The optional benchmarks add about 10 minutes.

---

## Part 0. The idea in 60 seconds (read this first)

- Site A (`site_a` database) has `orders` (200,000 rows). Site B (`site_b` database) has `customers` (1,000,000 rows). A sees B's table as a **foreign table** through postgres_fdw.
- The test query is `SELECT o.order_id, c.name FROM orders o JOIN customers c ON c.customer_id = o.customer_id WHERE o.order_id <= n`.
- **Plain postgres_fdw** asks B for all 1,000,000 customers and joins at A.
- **This project** first collects the distinct `customer_id`s of the selected orders at A, sends them to B, and B returns only matching customers. That is a **semijoin**. The keys travel in one of two ways:
  - **index path**: as a key list, `customer_id = ANY($1)`. B uses its primary-key index.
  - **filter path**: as a filter, `sjb_contains($1, customer_id)`. B scans all rows and tests each one. The filter is a **Bloom** filter (small, may let some extra rows through), an **EXACT** sorted key list, or **PASS** (lets everything through).
- A **cost model** estimates the time of each option and picks the cheapest.
- **Why the result is always exact:** the filter can only *remove* customers that have no matching order. A Bloom filter never says "no" to a key that is in it (no false negatives); it only sometimes says "yes" to a key that is not (false positives). The original join condition still runs at A, so those extra customers find no order and disappear. So the answer is always exactly the plain answer, only cheaper.

Keep these two facts in mind. Almost every check below tests one of them:
1. **Same rows as plain postgres_fdw**, always.
2. **Fewer rows/bytes from B**, chosen by the cost model.

---

## Part 1. Build PostgreSQL 18.6 with the patched postgres_fdw

### 1.1 System packages (once, needs sudo)
```bash
$ sudo apt update
$ sudo apt install -y build-essential git flex bison libreadline-dev zlib1g-dev pkg-config python3 python3-pip python3-venv
```
**Expect:** packages install. **Why:** PostgreSQL's source build needs a C compiler, `flex`/`bison` (for the SQL parser), readline (psql) and zlib.

### 1.2 Get the code
```bash
$ git clone https://github.com/soliyaanurag/Bloom-Filter-based-semi-join.git ~/postgre-project
$ mkdir -p ~/postgres
$ git clone --branch REL_18_6 --depth 1 https://git.postgresql.org/git/postgresql.git ~/postgres/postgresql
```
If `git.postgresql.org` is blocked, the GitHub mirror has the same tag: `https://github.com/postgres/postgres.git`.

### 1.3 Apply the patch
```bash
$ cd ~/postgres/postgresql
$ git apply --check ~/postgre-project/patches/postgres_fdw-semijoin.patch
$ git apply ~/postgre-project/patches/postgres_fdw-semijoin.patch
$ git status --short
```
**Expect:** `--check` prints **nothing**. `git status` lists exactly these 8 paths:
```
 M contrib/postgres_fdw/Makefile
 M contrib/postgres_fdw/meson.build
 M contrib/postgres_fdw/option.c
 M contrib/postgres_fdw/postgres_fdw.c
 M contrib/postgres_fdw/postgres_fdw.h
?? contrib/postgres_fdw/expected/semijoin.out
?? contrib/postgres_fdw/semijoin.c
?? contrib/postgres_fdw/sql/semijoin.sql
```
**Why:** the project changes **only** `contrib/postgres_fdw`; PostgreSQL's core is untouched. Silence from `--check` means every hunk matches REL_18_6 exactly.

### 1.4 Build and install
```bash
$ ./configure --prefix=$HOME/postgres/postgresql/install --enable-debug --without-icu
$ make -j"$(nproc)" && make install
$ make -C contrib/postgres_fdw && make -C contrib/postgres_fdw install
```
**Expect:** ends without errors; the postgres_fdw build prints no `warning:` lines. **Why:** contrib modules are not built by the top-level `make`, so postgres_fdw needs its own build.

---

## Part 2. Start a server

```bash
$ export PATH=$HOME/postgres/postgresql/install/bin:$PATH
$ export PGDATA=$HOME/postgres/postgresql/install/data
$ initdb -E UTF8 --locale=C.UTF-8
$ pg_ctl -l ~/pg.log start
$ psql -d postgres -c "CREATE ROLE postgres SUPERUSER LOGIN"
$ psql -d postgres -c "SELECT version()"
```
**Expect:** `PostgreSQL 18.6 on x86_64-pc-linux-gnu ...`.

**Why each detail matters:**
- `-E UTF8`: older versions of `bench/final_eval.py` crashed on a SQL_ASCII cluster (fixed, see the report section 5.1). UTF8 is the normal choice anyway, and what you get under WSL with a normal locale.
- Run `initdb` **as your normal user** (not root, not with `-U postgres`). Your OS user then becomes a superuser role. postgres_fdw's own regression tests connect "as the OS user the server runs as", so that role must exist. If you did use `-U postgres`, also run `psql -U postgres -d postgres -c "CREATE ROLE $(whoami) SUPERUSER LOGIN"`.
- The `postgres` role is what the project's scripts use (`setup/env.example.sh`).

---

## Part 3. Build and test the sjbloom extension

```bash
$ cd ~/postgre-project
$ make -C sjbloom && make -C sjbloom install
$ source setup/env.example.sh          # PGHOST=localhost PGPORT=5432 PGUSER=postgres
$ make -C sjbloom installcheck
```
**Expect:**
```
ok 1         - sjbloom_basic        ... ms
1..1
# All 1 tests passed.
```
**Why:** this test builds filters and checks: no false negatives for 10,000 keys, a false-positive rate in the predicted band, NULL handling, size limits, the EXACT and PASS modes and the cost-model functions. Passing means the C code behaves as specified on your machine.

---

## Part 4. Create the two sites

```bash
$ setup/setup_all.sh
```
**Expect:** six steps, then `=== Setup finished ===`. The last table printed is:
```
  srvname   |                                    srvoptions
------------+----------------------------------------------------------------------------------
 site_b_srv | {host=localhost,port=5432,dbname=site_b,extensions=sjbloom,semijoin_filter=true}
```
It takes about 10 to 30 s.

**Why these two options:**
- `extensions=sjbloom` tells postgres_fdw that B also has sjbloom, so it may **send** `sjb_contains(...)` to B. Without it, the filter would run at A after all rows had arrived: still correct, but nothing saved.
- `semijoin_filter=true` is the opt-in switch for the rewrite. Without it the patched postgres_fdw behaves exactly like the original.

Check the data:
```bash
$ psql -X -d site_a -c "SELECT count(*), count(DISTINCT customer_id) FROM orders" -c "SELECT count(*) FROM customers"
```
**Expect:** `200000 | 181329` and `1000000`. **Why exact:** the data generator uses a fixed random seed, so everyone gets the same rows. `customers` is the foreign table, so the second count travels from B.

---

## Part 5. Play with the filter by hand (the sjbloom functions)

Open `psql -X -d site_a` and run:

```sql
SELECT sjb_info(sjb_build(ARRAY[1,2,3], 64, 3));
```
**Expect:** `mode=bloom nkeys=3 m=64 k=3 bits_set=14.06% bytes=32`

**Why:** a Bloom filter with m = 64 bits and k = 3 positions per key. 3 keys × 3 positions = 9 bits set (if none collide): 9/64 = 14.06%. Size = 24-byte header + 64/8 = 8 bytes of bits = 32.

```sql
SELECT sjb_contains(sjb_build(ARRAY[1,2,3], 64, 3), 2)    AS has_2,
       sjb_contains(sjb_build(ARRAY[1,2,3], 64, 3), 1000) AS has_1000;
```
**Expect:** `t | f`. **Why:** 2 was added, so all its bits are 1: it must pass (no false negatives). 1000 was not; with only 14% of the bits set it is very likely to hit a 0 bit, so it is rejected.

```sql
SELECT get_byte(sjb_build(ARRAY[1], 64, 1), 0) AS version_byte;
```
**Expect:** `3`. **Why:** the first header byte is the filter version (`SJB_VERSION = 3`, enhanced double hashing). If you see anything else, your `sjbloom.so` is a stale build.

```sql
SELECT sjb_info(sjb_build_auto(ARRAY[5,5,NULL,7], 1000000, 1, 30, 1000, 158));
SELECT sjb_info(sjb_build_auto(ARRAY(SELECT generate_series(1,10000)), 1000000, 1, 30, 1000, 158));
SELECT sjb_info(sjb_build_auto(ARRAY(SELECT generate_series(1,990000)), 1000000, 1, 30, 1000, 158));
```
**Expect:**
```
mode=exact nkeys=2 min_key=5 max_key=7 bytes=32
mode=bloom nkeys=10000 m=180015 k=12 bits_set=48.71% bytes=22526
mode=pass nkeys=990000 bytes=24
```
**Why:** `sjb_build_auto` removes NULLs and duplicates (so `5,5,NULL,7` → 2 keys) and lets the cost model pick the mode:
- 2 keys: an EXACT list is tiny and has no false positives.
- 10,000 keys: a Bloom filter at ~18 bits per key (22 KB) beats an exact list (40 KB). Note bits_set ≈ 50%: that is the mark of an optimally sized Bloom filter (k ≈ 0.693 · bits per key).
- 990,000 of 1,000,000 keys: almost every row matches anyway, so sending any filter is a waste; PASS lets everything through.

```sql
SELECT sjb_contains(set_byte(sjb_build(ARRAY[1,2], 64, 3), 0, 2), 1);
SELECT sjb_contains('\x0102'::bytea, 1);
SELECT sjb_contains(NULL, 1) IS NULL AS null_filter,
       sjb_contains(sjb_build(ARRAY[1], 64, 3), NULL) IS NULL AS null_key;
```
**Expect:**
```
ERROR:  sjbloom: filter has version 2, but this sjbloom understands only version 3
HINT:  Site A and site B must run the same sjbloom build. ...
ERROR:  sjbloom: filter is too small (2 bytes, need at least 24)
 t | t
```
**Why:** a filter built with a different hashing rule would test the wrong bits and could **drop real matches**. Refusing it is what protects correctness when A and B run different builds. Damaged bytes give an error, never a crash. The functions are `STRICT`, so NULL in gives NULL out, and a NULL key never joins.

Now the key property, on 100,000 random keys:
```sql
CREATE TEMP TABLE rk AS SELECT DISTINCT (random()*4e9 - 2e9)::int AS k FROM generate_series(1,100000);
WITH f AS MATERIALIZED (SELECT sjb_build(array_agg(k), 50000, 3) AS f FROM rk)
SELECT count(*) AS keys, count(*) FILTER (WHERE NOT sjb_contains(f, k)) AS false_negatives FROM rk, f;
```
**Expect:** `false_negatives = 0` (keys ≈ 99,998). **Why:** even a hopelessly overfull filter (0.5 bits per key) only adds false *positives*. Every inserted key sets exactly the bits it is later tested on. (`MATERIALIZED` makes PostgreSQL build the filter once instead of once per row.)

---

## Part 6. Watch the planner rewrite a query

Stay in `psql -X -d site_a`.

### 6.1 Small n: the index path
```sql
SET postgres_fdw.semijoin_verbose = on;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT o.order_id, c.name FROM orders o JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= 1000;
```
**Expect** (numbers ≈):
```
NOTICE:  postgres_fdw semijoin: index path (cost model): n_est=917 ... s_rows=1000000 ...
DETAIL:  cost model winner: index path (5.9 ms <= 159.9 ms for filter path with bloom)
...
 Hash Join
   InitPlan 1
     ->  Aggregate
           Output: array_agg(DISTINCT o_1.customer_id)
           ...
   ->  Foreign Scan on public.customers c
         Remote SQL: SELECT customer_id, name FROM public.customers WHERE ((customer_id = ANY ($1::integer[])))
         Semijoin Path: index
         Semijoin Estimated Keys: ≈920-980
         Semijoin Predicted Time: index=≈6 ms filter=≈160 ms
```
**Why:**
- The `InitPlan` is the extra sub-SELECT the hook added. It runs **once**, collects the ~1,000 distinct keys, and its result becomes `$1`.
- `Remote SQL` is what B receives: B gets the key list and uses its primary-key index. 1,000 index lookups (≈6 ms) are much cheaper than scanning and testing 1,000,000 rows (≈160 ms at ≈158 ns per row), so the index path wins.
- `Estimated Keys` is a plan-time guess (from statistics; the true value is 999). It varies a little after each ANALYZE because ANALYZE samples rows at random.
- The join is a **Hash Join**. Before Batch 7 the planner guessed 10 rows for `= ANY($1)` and picked a slow Nested Loop. The hook now tells it to expect about n rows.

### 6.2 Large n: the filter path with a Bloom filter
```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF)
SELECT o.order_id, c.name FROM orders o JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= 150000;
```
**Expect:**
```
   ->  Foreign Scan on public.customers c (actual rows=141538.00 loops=1)
         Remote SQL: SELECT customer_id, name FROM public.customers WHERE (public.sjb_contains($1::bytea, customer_id))
         Semijoin Path: filter
         Semijoin Predicted Time: index=≈854 ms filter=≈393 ms
         Semijoin Filter: mode=bloom keys=139294 m=1730968 k=9 bytes_sent=432792
 Hash Join (actual rows=150000.00 ...)
```
**Why:**
- With 139,294 keys, 139,294 index lookups at B (≈5 µs each) cost more than one scan of 1,000,000 rows. So the filter path wins.
- `keys=139294` is the **exact** number of distinct keys (it equals `SELECT count(DISTINCT customer_id) FROM orders WHERE order_id <= 150000`). The filter is sized at run time, when this number is known.
- `m=1730968 k=9`: about 12.4 bits per key, sized by the cost model. `bytes_sent=432792` = 2 × (24 + 1,730,968/8) + 2, because postgres_fdw sends a bytea as hex text (2 characters per byte, plus `\x`).
- `actual rows=141538` from B = 139,294 real matches + **2,244 false positives**. The formula (1 − e^(−k·n/m))^k ≈ 0.26% of the 860,706 non-matching customers ≈ 2,200. They are harmless: the Hash Join finds no order for them, so the final count is still exactly 150,000.
- The numbers `139294`, `1730968`, `9`, `432792`, `141538` are deterministic and should match exactly.

### 6.3 Force the filter path at small n: the EXACT mode
```sql
SET postgres_fdw.semijoin_force_path = 'filter';
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF)
SELECT o.order_id, c.name FROM orders o JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= 100;
RESET postgres_fdw.semijoin_force_path;
```
**Expect:** `Semijoin Filter: mode=exact keys=100 bytes_sent=850`, Foreign Scan `actual rows=100`, Execution Time ≈ 70 to 100 ms.

**Why:** for 100 keys an EXACT list (400 bytes) is small and returns no false positives. Each false positive could cost an extra FETCH round trip. 850 = 2 × (24 + 4 × 100) + 2. It is slow (≈90 ms vs ≈3 ms on the index path) because B still tests every one of the 1,000,000 rows. **This is why the cost model never picks the filter path for small n.**

### 6.4 Turn the feature off: plain postgres_fdw
```sql
SET postgres_fdw.enable_semijoin = off;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF)
SELECT o.order_id, c.name FROM orders o JOIN customers c ON c.customer_id = o.customer_id
WHERE o.order_id <= 1000;
RESET postgres_fdw.enable_semijoin;
```
**Expect:** `Remote SQL: SELECT customer_id, name FROM public.customers` (no WHERE), Foreign Scan `actual rows=1000000`, no Semijoin lines, Execution Time ≈ 1,000 to 1,500 ms.

**Why:** this is the problem the project solves: B ships all 1,000,000 customers to use 999 of them.

### 6.5 Time it yourself
```sql
\timing on
SET postgres_fdw.enable_semijoin = off;
SELECT count(*) FROM orders o JOIN customers c ON c.customer_id = o.customer_id WHERE o.order_id <= 1000;
RESET postgres_fdw.enable_semijoin;
SELECT count(*) FROM orders o JOIN customers c ON c.customer_id = o.customer_id WHERE o.order_id <= 1000;
```
**Expect:** both counts are `1000`. Off ≈ 1.0 to 1.2 s, on ≈ 5 to 15 ms (about 100× faster). Run each twice; the first run of a session also opens the connection to B.

**Why:** same rows (fact 1), far fewer rows moved (fact 2).

**Careful:** write the join directly in the query. `SELECT count(*) FROM (SELECT ... JOIN customers ...) x`, a view, or a `WITH` query is **not** rewritten, because the hook only looks at the top level of the query, and there it sees a subquery instead of the two tables. Try it with `EXPLAIN VERBOSE`: the Remote SQL has no `WHERE`. (Report section 5.2.)

---

## Part 7. Prove correctness

### 7.1 The project's correctness script
```bash
$ psql -X -d site_a -f queries/correctness_auto.sql 2>/dev/null | grep -E "^ +[0-9]+ \|"
```
**Expect:** 18 rows (n = 100, 10,000, 150,000 × paths auto/index/filter × queries join/group by). In **every** row the last two columns, `off_except_on` and `on_except_off`, are `0`:
```
   100 | auto   | join     |    100 |    100 | 0 | 0
   100 | auto   | group by |     63 |     63 | 0 | 0
   ...
150000 | filter | group by |    100 |    100 | 0 | 0
```
**Why this test is strong:** it stores the result with the rewrite off and with it on, then computes `(off EXCEPT ALL on)` and `(on EXCEPT ALL off)`. `EXCEPT ALL` compares **multisets**, so a missing row, an extra row or a wrong duplicate count would show up as a non-zero number. Both being 0 means the two results are identical. (The script uses temp tables because a query with `EXCEPT ALL` itself is never rewritten.)

### 7.2 Your own check with edge-case data
Run my edge-case script (it builds a 10-row table with a NULL key, a duplicate key, keys missing at B, negative keys, 0, and the int4 limits ±2,147,483,647/8, then compares on vs off for every path):
```bash
$ psql -X -d site_a -f queries/edge_cases.sql 2>&1 | grep -E "^==|off=|ERROR|false_neg|fn_"
```
(The file is [queries/edge_cases.sql](../queries/edge_cases.sql).)

**Expect:** every `off=X on=X off-on=0 on-off=0` line has the same X on both sides and both differences 0 (24 lines). T9's plans show **no** `Semijoin` lines and a plain `Remote SQL`. T10 shows the version / size / mode errors. T11 shows `fn_bloom_small = 0`, `fn_auto = 0`.

**Why:**
- T1: NULL keys never join and are skipped; keys missing at B simply return no row; duplicate keys still give one row per order.
- T2/T3: an empty key set returns no rows on every path (the filter is NULL or an empty EXACT list).
- T9: the hook **refuses** queries where rewriting could be wrong or unsupported: LEFT JOIN (unmatched orders must still appear), an OR that mixes both tables, a volatile function such as `random()` (the local table is read twice and could select different rows), two join conditions, and `$1` parameters (no value at plan time).

---

## Part 8. Count the bytes on the wire (optional, Linux/WSL)

All postgres_fdw traffic between the two databases goes over the loopback interface `lo`. If you connect psql through the Unix socket, your own session does not use `lo`, so the counter shows only A↔B traffic.

```bash
$ cat > /tmp/q.sql <<'EOF'
SELECT count(*) FROM orders o JOIN customers c ON c.customer_id = o.customer_id WHERE o.order_id <= 1000;
EOF
$ rx() { awk '/ lo:/ {print $2}' /proc/net/dev; }
$ a=$(rx); PGOPTIONS="-c postgres_fdw.enable_semijoin=off" psql -X -h /tmp -d site_a -f /tmp/q.sql >/dev/null; b=$(rx); echo "off: $((b-a)) bytes"
$ a=$(rx); psql -X -h /tmp -d site_a -f /tmp/q.sql >/dev/null; b=$(rx); echo "on:  $((b-a)) bytes"
```
(Use `-h /var/run/postgresql` instead of `-h /tmp` if that is where your socket is: `psql -c "SHOW unix_socket_directories"`.)

**Expect:** off ≈ 18,700,000 bytes, on ≈ 28,000 bytes (my run: 18,741,578 and 28,159).

**Why:** `count(*)` needs only `customer_id`, so B sends one column. Off moves all 1,000,000 ids (≈7 bytes of text each plus ≈11 bytes of protocol overhead per row). On sends a list of ≈1,000 keys and gets back 999 rows. (With `c.name` in the SELECT list, as in the benchmarks, off is ≈34 MB.)

---

## Part 9. The false-positive study (deterministic, must match exactly)

```bash
$ psql -X -d site_a -f queries/fp_rate.sql | tail -9
$ psql -X -d site_a -f queries/fp_cases.sql 2>&1 | grep -v "^$" | tail -20
```
**Expect** from `fp_rate.sql`:
```
 bits_per_key |   m    | k  | measured_fp | predicted_fp
            4 |  40000 |  3 |    0.145933 |     0.146892
            8 |  80000 |  6 |    0.021308 |     0.021577
           12 | 120000 |  8 |    0.003179 |     0.003142
           16 | 160000 | 11 |    0.000445 |     0.000459
           22 | 220000 | 15 |    0.000020 |     0.000026
```
and from `fp_cases.sql`: measured false positives `14, 35, 300, 8363` (synthetic keys), `23, 36` (real keys), and "same positions as a member" `13` and `2`.

**Why exact:** the positions come only from PostgreSQL's `hashint4extended(key, seed)` with a fixed seed, so every machine computes the same bits. The measured rate matches the textbook formula for normal filter sizes. At tiny filters (n = 100, m = 2,640) it is above the formula (14 vs 3.1). The doc explains why: all k positions come from two numbers mod m, so about n·1,000,000/m² ≈ 14 outsiders get exactly a member's positions. That is a known property of double hashing, documented and harmless (it costs a few extra rows, never a wrong answer).

---

## Part 10. Regression tests of the patched postgres_fdw

```bash
$ cd ~/postgres/postgresql/contrib/postgres_fdw
$ make installcheck REGRESS=semijoin
$ make installcheck
```
**Expect:** `ok 1 - semijoin`. The full suite: `ok 1 - postgres_fdw` and `ok 2 - query_cancel` (the TAP tests are skipped: "TAP tests not enabled").

**Why:** `semijoin` is the project's own test (rewrites, refusals, EXPLAIN lines, correctness). The full suite is PostgreSQL's own postgres_fdw test: passing it shows the patch did not break normal postgres_fdw behaviour.

**If the full suite fails with only `user mapping not found for user "<name>"`** (2 hunks in `regression.diffs`): your OS user has no role, see Part 2. I checked that the unmodified postgres_fdw fails the same way in that setup, so it is not the patch.

---

## Part 11. Benchmarks (optional, about 10 minutes)

```bash
$ cd ~/postgre-project
$ python3 -m venv .venv && .venv/bin/pip install -r bench/requirements.txt
$ source setup/env.example.sh
$ .venv/bin/python bench/calibrate.py        # ≈1 min, prints this machine's constants
$ .venv/bin/python bench/final_eval.py       # ≈7 min: E1, E6, E2, E3, E4; reloads the data at the end
$ .venv/bin/python bench/final_report.py     # tables + charts in results/final/
```
Before timing on a laptop: plug in the charger and use the best-performance power mode (power saving makes every round trip ~3× slower).

**Expect:**
- `final_eval.py` ends with `correctness_auto.sql: 18 comparisons, all 0: True` and `done: 129 rows, 0 not correct`. **This is exact. Any "not correct" is a real failure.**
- `final_report.py` (without E5): 28 configurations. My run: auto chose the fastest path in 22 of 28 (the doc: 24 of 28), geometric-mean speedup ≈ 22× (doc 25×), bytes saved ≈ 96.8%, auto never slower than plain postgres_fdw.
- The pattern to look for in `results/final/decision_quality.md`:
  - index path for n ≤ 10,000, filter/bloom at n = 50,000 to 150,000 (1M customers);
  - with 3M customers, index even at n = 50,000;
  - E3 (missing keys) and E4 (skewed keys) at n = 50,000: auto picks filter but index is faster.
- `calibrate.py` constants near the doc's: t_check_ns ≈ 150 to 190, t_lookup_us ≈ 3 to 5, per_message_ms ≈ 0.05 to 0.06, per_row_us ≈ 0.5, t_build_us ≈ 0.24.

**Why counts can differ from the docs:** at n ≈ 50,000 the two paths are within 5 to 20% of each other, about the size of run-to-run noise. Which one "wins" can flip between runs, so a different correct-choice count there is expected and costs little (look at the regret column: 1.1 to 1.3). E3 and E4 lose for documented reasons: the model assumes every key exists at B and does not know about key clustering, and lookups of missing or clustered keys are cheaper than calibrated.

**E5 (slow network)** needs `sudo tc qdisc add dev lo root netem rate 100mbit delay 1ms` and slows **all** local traffic while active; follow docs/WALKTHROUGH.md, Batch 8, and remove it afterwards with `sudo tc qdisc del dev lo root`. Without netem, `final_eval.py --e5 10 --simulated` adds only the bandwidth delay (no latency), so it cannot reproduce the doc's 25 s / 50 s numbers.

---

## Part 12. Quick checklist

| # | check | pass if |
|---|---|---|
| 1 | `git apply --check` | prints nothing |
| 2 | `make -C sjbloom installcheck` | `All 1 tests passed` |
| 3 | `setup_all.sh` | `semijoin_filter=true` in the server options |
| 4 | `get_byte(sjb_build(ARRAY[1],64,1),0)` | `3` |
| 5 | false negatives on 100,000 keys | `0` |
| 6 | EXPLAIN n = 1000 | `Semijoin Path: index`, `= ANY ($1...)`, Hash Join |
| 7 | EXPLAIN ANALYZE n = 150000 | `mode=bloom keys=139294 m=1730968 k=9 bytes_sent=432792`, 141,538 rows from B, 150,000 rows out |
| 8 | `enable_semijoin = off` | 1,000,000 rows from B |
| 9 | `correctness_auto.sql` | 18 rows, all `0 | 0` |
| 10 | `fp_rate.sql` / `fp_cases.sql` | numbers exactly as in Part 9 |
| 11 | `make installcheck REGRESS=semijoin` | `ok 1 - semijoin` |
| 12 | `final_eval.py` (optional) | `0 not correct` |

If 1 to 11 pass, the project works on your machine: it gives exactly the right answer and fetches far less data than plain postgres_fdw.

## Troubleshooting

| symptom | cause | fix |
|---|---|---|
| `user mapping not found for user "anuragsoliya"` on `customers` | you ran psql as your own user; the mapping exists only for `postgres` | `source setup/env.example.sh` (sets PGUSER=postgres) |
| No `Semijoin` lines in EXPLAIN | postgres_fdw not preloaded, option missing, or the original postgres_fdw is installed | rerun `setup/setup_all.sh`; check `SHOW session_preload_libraries` in site_a shows `postgres_fdw`; open a new psql session |
| `invalid option "semijoin_filter"` | the original postgres_fdw is installed | redo Part 1.4 |
| `final_eval.py`: `TypeError: a bytes-like object is required` | an older checkout on a SQL_ASCII cluster | pull the fix, or recreate the cluster with `initdb -E UTF8 --locale=C.UTF-8` |
| `filter has version N` | A and B run different sjbloom builds | `make -C sjbloom install`, then `DROP EXTENSION sjbloom CASCADE; CREATE EXTENSION sjbloom;` in both databases, then `setup/setup_all.sh` |
| timings 2 to 3× slower than above | laptop in power-saving mode | plug in, best-performance mode |
