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
- The source tree it was built from is ~/postgres/postgresql. Do NOT build, configure or change anything in that folder.
- `make install` writes into the user's home folder, so it needs NO sudo.
- Data directory: ~/postgres/postgresql/install/data (exported as PGDATA). The server runs as the user, not as a system service. WSL does not start it automatically: start it with `pg_ctl -l ~/pg.log start` and check it with `pg_ctl status`.
- Superuser roles: anuragsoliya and postgres. Port 5432.
- pg_hba.conf uses trust for local connections, so no password is checked on this machine. Still keep the password option in the postgres_fdw user mapping, because teammates' setups may check passwords. Pass the password in as a psql variable (for example `psql -v fdw_password=...`). Never hard-code it.
- The two "sites" are two databases, site_a and site_b, on the same local PostgreSQL server. postgres_fdw connects from site_a to site_b over localhost, so data really goes through a network connection.
- From Batch 6 on, a separate copy of the PostgreSQL 18.6 source (the same version as the installed one) is in ./pg-src. Only contrib/postgres_fdw in ./pg-src is modified and rebuilt with `make USE_PGXS=1`. The original ~/postgres/postgresql tree is never touched.

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
- The cost model's GOAL is estimated TIME, not bytes: time = bytes / bandwidth + work at site B (communication cost + processing cost, as in Ozsu & Valduriez, "Principles of Distributed Database Systems"). Details come in Batch 5.
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
- Batches 0, 1, 2, 3 and 4 are done and committed. The false-positive work is FINISHED: filter version 3 = enhanced double hashing, step h2 not forced odd, early exit in sjb_bloom_test, SJB_VERSION 3 (details and v1/v2/v3 tables in docs/fp_investigation.md; branch fp-investigation is kept as a record). The mid-sem charts and CSVs in docs/midsem/ are from version 3; the original Batch 4 (v1) charts are kept in docs/midsem/v1/. results/ stays in .gitignore. Next is Batch 5.
- postgres_fdw was installed by the user from the source tree. Do not reinstall it, and never touch ~/postgres/postgresql.
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
