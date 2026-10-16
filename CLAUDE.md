# CS631 Project: Cost-Based Adaptive Semi-Join Filtering for postgres_fdw

## What we are building
Site A joins a local table R (orders) with a table S (customers) that lives at site B and is visible at A as a postgres_fdw foreign table. Normally A fetches all of S over the network. Instead:
1. At A, collect the distinct non-null join keys of the filtered local rows R'.
2. Build a compact filter from those keys: a Bloom filter (mode BLOOM), an exact sorted key list (mode EXACT), or a pass-everything filter (mode PASS).
3. Send the filter to B as a query parameter. B keeps only rows of S whose key passes the filter. This is the semijoin S ⋉ R' (approximate, i.e. a superset, when using Bloom).
4. A runs the normal exact join, which removes Bloom false positives, so the final result is ALWAYS exactly correct.
A cost model chooses the mode and the Bloom filter size m (dynamic sizing, chosen per query).

## Environment
- Machine: WSL2 Ubuntu.
- PostgreSQL 18.6 is built from source and installed at ~/postgres/postgresql/install. Its bin/ is on PATH, so `pg_config`, `psql` and `pg_ctl` come from there. Our C extension is built with PGXS using that pg_config.
- The source tree it was built from is ~/postgres/postgresql. Do NOT build, configure or change anything in that folder, EXCEPT the Batch 6 rule below.
- `make install` writes into the user's home folder, so it needs NO sudo.
- Data directory: ~/postgres/postgresql/install/data (exported as PGDATA). The server runs as the user, not as a system service. WSL does not start it automatically: start it with `pg_ctl -l ~/pg.log start` and check it with `pg_ctl status`.
- Superuser roles: anuragsoliya and postgres. Port 5432.
- pg_hba.conf uses trust for local connections, so no password is checked on this machine. Still keep the password option in the postgres_fdw user mapping, because teammates' setups may check passwords. Pass the password in as a psql variable (for example `psql -v fdw_password=...`). Never hard-code it.
- The two "sites" are two databases, site_a and site_b, on the same local PostgreSQL server. postgres_fdw connects from site_a to site_b over localhost, so data really goes through a network connection.
- From Batch 6 on (replaces the old ./pg-src plan, which was never created): postgres_fdw is modified IN the main tree ~/postgres/postgresql, under these rules:
  - Edit files ONLY under ~/postgres/postgresql/contrib/postgres_fdw, and ONLY on branch cs631-semijoin.
  - Branch base: commit a85b5523e6 on REL_18_STABLE (REL_18_6 + 156 commits; the only postgres_fdw change since the tag is the 2-line typo fix 3a09e91a49 in postgresAcquireSampleRowsFunc). The patch is also checked with `git apply --check` against plain REL_18_6.
  - Commit ONLY with `git add contrib/postgres_fdw` (never `git add -A` or `git commit -a`). The other changed/untracked files in that tree (configure, *.sql, gmake*.out, logfile, install/) are the user's coursework: never add, commit, stash, reset or delete them.
  - Build and install (no sudo): `make -C ~/postgres/postgresql/contrib/postgres_fdw && make -C ~/postgres/postgresql/contrib/postgres_fdw install`.
  - Restore the original postgres_fdw: `git switch REL_18_STABLE` in that tree, then rebuild + install; or copy the backup `cp ~/postgres_fdw.so.orig ~/postgres/postgresql/install/lib/postgres_fdw.so`.
  - The exported patch lives in patches/postgres_fdw-semijoin.patch.

## Fixed design decisions (do not change without asking the user)
- Our extension is named `sjbloom`. It is installed in BOTH site_a and site_b, in schema public. It stays at version 1.0. After changing its SQL file, recreate it with DROP EXTENSION sjbloom CASCADE; CREATE EXTENSION sjbloom; (no upgrade scripts).
- Version 1 supports only: INNER JOIN, one equality join condition, int4 join keys.
- Hashing: always use PostgreSQL's own hash function hashint4extended(key, seed) via DirectFunctionCall2. Never hash raw bytes.
- The k bit positions come from ENHANCED double hashing (Dillinger & Manolios, "Bloom Filters in Probabilistic Verification", FMCAD 2004; the same method as PostgreSQL's src/backend/lib/bloomfilter.c), with ONE hash call per key: h1 = low 32 bits of the hash, h2 = high 32 bits (NOT forced odd, like PostgreSQL; enhanced double hashing does not need an odd step, and forcing it halved the usable (h1, h2) pairs when m is even). Start at x = h1 mod m with a step y = h2 mod m. Position 0 is x. For each next position i = 1..k-1: move forward by the step (x = (x + y) mod m), then make the step i bigger (y = (y + i) mod m). Because the step keeps growing, the positions are not evenly spaced, unlike classic double hashing (h1 + i*h2) mod m, which gave far too many false positives for small m.
- The filter header's version is SJB_VERSION = 3 (1 = classic double hashing, 2 = enhanced with h2 forced odd, 3 = enhanced, h2 not forced odd). Site A and site B must compute the same positions, so a filter with another version is rejected with a clear error, never read with the wrong positions. Bump SJB_VERSION whenever the positions or the layout change.
- A filter is a bytea: a 24-byte header, then a payload. The header holds version uint8, mode uint8, k uint16, nkeys uint32, m uint64, seed uint64. Always read/write the header with memcpy, never by casting pointers.
- False-positive model: ONLY the standard formula predicted_fp = (1 - exp(-k*n/m))^k (Broder & Mitzenmacher, "Network Applications of Bloom Filters: A Survey", 2004). sjb_best_bits uses its best-k form, e^(-(ln 2)^2 * b).
- At small m the measured FP rate is above this formula (a known effect of double hashing, see Kirsch & Mitzenmacher, "Less Hashing, Same Performance", 2006). This is documented in docs/fp_investigation.md as a limitation and is NOT modelled.
- Modes: 1 = BLOOM (payload = ceil(m/8) bytes of bits), 2 = EXACT (payload = sorted, unique int4 keys), 3 = PASS (no payload; every key passes).
- The filter SQL functions are IMMUTABLE STRICT PARALLEL SAFE. postgres_fdw only ships immutable functions to the remote server, and STRICT gives correct NULL handling automatically.
- postgres_fdw sends query parameters as TEXT: a bytea filter travels as hex text (2 bytes per filter byte, plus "\x"), and an int4[] travels as text like {12,345}. Rows also come back as text. All byte estimates must use these text sizes.
- The cost model's GOAL is estimated TIME, not bytes: time = bytes / bandwidth + work at site B (communication cost + processing cost, as in Ozsu & Valduriez, "Principles of Distributed Database Systems"). See "Paths, modes and the cost model" below.
- Cost model settings (network_mbit, t_check_ns, t_lookup_us, max_filter_bytes, wire_factor, per_message_ms, per_row_us, t_build_us, fetch_size) are passed as function ARGUMENTS, never read from GUCs, so the functions stay truly IMMUTABLE. The last four have defaults = our calibrated values (in sjbloom--1.0.sql); regression tests pass them explicitly so they do not depend on this machine.

## Paths, modes and the cost model (Batch 5)
- Two PATHS: index path = B gets "customer_id = ANY(key list)" and uses its index (work: n * t_lookup_us); filter path = B gets "sjb_contains(filter, customer_id)" and scans ALL rows (work: s_rows * t_check_ns). The path is chosen at PLAN time (Batch 6) with sjb_plan_choice; sjb_choice_report shows the same estimates as a readable table.
- Inside the filter path, sjb_build_auto picks the MODE (BLOOM, EXACT, PASS) at RUNTIME from the exact number of distinct keys. Its work at B is the same for all modes, so the mode choice depends only on bytes.
- Batch 5b: full Ozsu & Valduriez formula, time = T_CPU*work + T_MSG*messages + T_TR*bytes:
  bytes/(network_mbit*125000) + (1 + ceil(rows_back/fetch_size))*per_message_ms + rows_back*per_row_us + work at B (index: n*t_lookup_us; filter: s_rows*t_check_ns) + work at A (filter only: n*t_build_us).
- Bytes: row_width = width ON THE WIRE = text width + 7 + 4 * (columns in the Remote SQL). Wire bytes: bloom 2*(24+ceil(m/8))+2, exact 2*(24+4n)+2, pass 2*24+2, key list n*7+2. Rows back: bloom M + (s_rows-M)*fp, exact/index M, pass s_rows, with M = min(s_rows, n*rows_per_key).
- Bloom size: b* = ln(3.844*(s_rows-M)*row_width/(wire_factor*n))/0.4805 (0 = skip Bloom), m = max(8, ceil(n*b*)), k = clamp(round(0.693*b*), 1, 32).
- All formulas live in sjbloom/costmodel.c (pure C, no SQL); sjb_estimate_all makes both decisions in ONE place.
- Constants come from bench/calibrate.py (results/calibration.json), always with max_parallel_workers_per_gather = 0 (postgres_fdw's cursor never runs in parallel), each on its own calibration workload (never the benchmark queries or orders keys). Never tune constants to the validation numbers.
- The correctness test for every strategy: (baseline EXCEPT ALL new) and (new EXCEPT ALL baseline) must both return 0 rows.

## Coding style (VERY IMPORTANT: the team are beginners)
- Prefer simple, readable code over clever or fast code.
- C: one small function per job; plain for/while loops; descriptive names (num_keys, not nk); no macro tricks; no clever pointer arithmetic.
- Every C function has a comment block above it saying, in plain English, what it does, its inputs and output, and why it exists.
- Every file starts with a header comment explaining its role in the project.
- The first time a file uses a PostgreSQL-specific macro or function (PG_GETARG_INT32, palloc, SET_VARSIZE, ereport, ...), add a short comment explaining it.
- Python: plain scripts with functions, no classes; use psycopg (version 3) and matplotlib only.
- SQL: commented, one step per block.
- After each batch, append a section to docs/WALKTHROUGH.md explaining in simple words what was built, file by file, and how to test it.

## How to work
- Do only the batch you are asked for. Run the checks listed in the batch, show me the real outputs, commit, then stop.
- NEVER run sudo yourself. Print the exact sudo command, ask me to run it, and wait.
- Never write passwords into committed files.
- If a check fails, do not hack around it. Show me the output and explain what you think is wrong.

## Current status and lessons learned
- Batches 0, 1, 2, 3, 4, 5 and 5b are done and committed. The false-positive work is FINISHED: filter version 3 = enhanced double hashing, step h2 not forced odd, early exit in sjb_bloom_test, SJB_VERSION 3 (details and v1/v2/v3 tables in docs/fp_investigation.md; branch fp-investigation is kept as a record). The mid-sem charts and CSVs in docs/midsem/ are from version 3; the original Batch 4 (v1) charts are kept in docs/midsem/v1/. results/ stays in .gitignore. Batch 5 added the EXACT and PASS modes (SJB_VERSION stays 3: BLOOM is unchanged), sjb_build_auto, sjb_choice_report, sjb_plan_choice, calibrate.py and validate_model.py. Batch 6 is done too (see below).
- Batch 5b is done: per_message_ms, per_row_us, end-to-end t_lookup_us and t_build_us were added (the open decision from Batch 5 is closed). Validation (covered part = Foreign Scan time): exact_keys ratio 0.76-1.08 for n >= 1000; Bloom 0.89-1.08 at n <= 1000 but 1.16-1.32 at n = 10000-50000. Break-even index vs filter: predicted 35,000 keys at 1000 Mbit (measured ~65,000) and 8,500 at 1 Mbit (simulated measured ~11,800); see docs/WALKTHROUGH.md, Batch 5b.
- Batch 6 is done: planner hook in postgres_fdw (contrib/postgres_fdw/semijoin.c, branch cs631-semijoin of ~/postgres/postgresql, base a85b5523e6; commit 5bd76f4ebc), exported to patches/postgres_fdw-semijoin.patch (also applies to plain REL_18_6). GUCs postgres_fdw.enable_semijoin / semijoin_force_path / semijoin_verbose / semijoin_network_mbit / semijoin_max_filter_kb / the five calibrated constants; server/table option semijoin_filter (opt-in, table wins). queries/correctness_auto.sql: 18/18 comparisons 0. Next is Batch 7 (better estimates).
- Batch 6 setup: the hook only works if postgres_fdw is in site_a's session_preload_libraries (otherwise the library is loaded mid-planning of the first query, too late). setup/enable_semijoin.sql (step 6 of setup_all.sh) sets it and semijoin_filter. It needs the MODIFIED postgres_fdw (the original rejects the option semijoin_filter).
- Run psql with `source setup/env.example.sh` (PGUSER=postgres): the user mapping exists only for postgres, so as anuragsoliya queries on customers fail with "user mapping not found".
- Batch 6 rewrite: steps 2-5 run in an internal subtransaction (try_plan_semijoin), so an error there leaves the query unchanged instead of failing it (query cancel is re-thrown). CREATE TABLE AS <select> IS rewritten; a set operation (EXCEPT ALL) is not, so correctness tests use temp tables. GROUP BY is supported (PG18's RTE_GROUP is ignored like RTE_JOIN; row width also reads its groupexprs).
- FOR BATCH 7: (1) the planner guesses 10 rows for the Foreign Scan with `= ANY($1)` (real 999), so n = 100 and 1000 get a Nested Loop with a Join Filter (263 ms at n=1000). (2) Cardenas underestimates n at large r: n=150000 gives n_est 99,363 vs 139,294 true (ANALYZE's n_distinct 168,392 vs true 181,329, and Cardenas assumes picking with replacement; Yao 1977 is the without-replacement version). (3) sjb_plan_choice always assumes max_filter_bytes = 1 MB; semijoin_max_filter_kb only reaches sjb_build_auto. (4) foreign columns used only in shipped conditions are counted in row_width.
- NOTE FOR BATCH 6 (done in semijoin.c): pg_stats.avg_width counts an int4 as 4 bytes, but on the wire (postgres_fdw sends rows as text) it is its text length, about 7 bytes for our ids (6-7 digits). The planner hook's row_width estimate must use TEXT widths (+ 7 + 4 * columns), not avg_width.
- postgres_fdw was originally installed by the user from the source tree. Since Batch 6 it is rebuilt from branch cs631-semijoin (see Environment); a backup of the original library is ~/postgres_fdw.so.orig.
- `= ANY` with a sub-SELECT needs an `::int[]` cast: `= ANY ((SELECT array_agg(...) ...)::int[])`. Without it, PostgreSQL treats the subquery as a row set and fails with "operator does not exist: integer = integer[]".
- In test queries, build filters in a `WITH ... AS MATERIALIZED` CTE so they are built once, not once per row.
- setup/setup_all.sh drops both sites, then (since Batch 3) creates sjbloom in both and runs setup/enable_sjbloom.sql. The extension must be installed first (make -C sjbloom install).
- Bloom false positives scale with the size of S, not R': at b=10 (fp ~0.8%) about 8,000 of the 999k non-matching customers pass. Use sjb_best_bits to pick b.
- The planner underestimates foreign scan rows for `= ANY` (it guesses 10; the real number was 999) and picks a Nested Loop with a Join Filter for exact_keys at small n. When comparing times, use the exact_keys runs with nestloop_off = True (SET enable_nestloop = off): 134 ms vs 7 ms at n=1000.
- Python venv on this WSL: `python3 -m venv` fails (ensurepip missing, python3.12-venv not installed). Workaround without sudo: `python3 -m venv --without-pip .venv` then `pip --python .venv/bin/python install -r bench/requirements.txt`. Run all benchmarks with `bench/run_all.sh`.
- Locally, Bloom is slower than exact_keys (about 150 ms minimum even for tiny n): site B must run sjb_contains on all 1,000,000 rows and cannot use its index, while `customer_id = ANY ($1)` can use the primary-key index at B. Bloom only wins on bytes, not on local time.
- Our byte estimates ignore PostgreSQL protocol overhead: each returned row is a DataRow message of about 7 + 4*columns bytes plus the values (15 bytes for 2 columns, about as much as a narrow row itself). The cost model will need this added to row_width.
- A change to sjbloom/sjb_filter.h needs a FULL rebuild of the extension: PGXS does not track header files, so without help it recompiled only the .c file that changed, and sjbloom.so silently kept the old SJB_VERSION. sjbloom/Makefile now enforces the full rebuild with `$(OBJS): sjb_filter.h`. After bumping SJB_VERSION, check `SELECT get_byte(sjb_build(ARRAY[1], 64, 1), 0);`.
- sjb_bloom_test computes positions one at a time and stops at the first 0 bit (same positions as sjb_bloom_add). bench/time_contains.py times site B's full scan to compare two builds.
- Calibrated on this machine (no parallel workers, Batch 5b run): t_check_ns = 158.19, t_lookup_us = 4.7956 (end to end through postgres_fdw; B-only lookup is ~1.7), per_message_ms = 0.06194, per_row_us = 0.54819, t_build_us = 0.245, fetch_size = 100. WSL timing is noisy: t_check_ns has been 150-188 across runs. t_check_ns/t_lookup_us are the psql defaults in queries/auto_manual.sql and choice_demo.sql (override with -v); the other four are the SQL function defaults.
- per_message_ms and per_row_us can NOT be fitted with one fetch_size: messages = 1 + R/100 is collinear with R and plain least squares gave negative constants. calibrate.py varies fetch_size (100/1000/10000) on a throwaway foreign table calib_tmp.customers, dropped at the end.
- An InitPlan whose result is a Foreign Scan parameter runs INSIDE the Foreign Scan's time (postgres_fdw evaluates params when opening the cursor; verified with pg_sleep). Never add InitPlan time to Foreign Scan time.
- To time an IMMUTABLE function with constant args (e.g. sjb_build_auto), pass the input as (SELECT $1): otherwise it is folded at PLAN time and execution time is ~0.
- At site B, EXPLAIN (ANALYZE) DECLARE c CURSOR FOR ... (inside a transaction) gives the same cursor plan postgres_fdw gets.
- Parallel workers: the row-returning query "SELECT customer_id, name ... WHERE sjb_contains(...)" never gets a parallel plan (the planner guesses 1/3 of rows pass and Gather is too expensive), so parallel on/off makes no difference for it. Only "SELECT count(*)" goes parallel (141 -> 55 ms). So time_contains.py's ~75 ms (count(*), parallel) understates site B's real work under postgres_fdw (~180 ms).
- Never test with a filter stored in a table and read back with (SELECT filter FROM t): the value is TOASTed and sjb_contains re-reads it for every row (5.5 s instead of 0.18 s). Build it inline or in a CTE. Through postgres_fdw the filter arrives as a parameter, so this does not happen there.
- pg_stats.avg_width is the STORED width (int4 = 4 bytes), not the text width (6-7 digits), so queries/catalog_stats.sql gives row_width 30 while the measured wire width is 31.78.
- In the filter path, BLOOM wins for n = 10000 and 150000 (b* is 12-18 bits per key, below EXACT's 32). Since Batch 5b, n = 100 picks EXACT: Bloom's ~2 false positives cost one extra FETCH round trip. EXACT also wins when b* > ~32 (huge S or wide rows); PASS only when n is close to s_rows. b* still minimises BYTES; with per_row_us, a time-optimal b would be somewhat larger (not changed).
