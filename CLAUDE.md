# CS631 Project: Cost-Based Adaptive Semi-Join Filtering for postgres_fdw

## What we are building
Site A joins a local table R (orders) with a table S (customers) that lives at site B and is visible at A as a postgres_fdw foreign table. Normally A fetches all of S over the network. Instead:
1. At A, collect the distinct non-null join keys of the filtered local rows R'.
2. Build a compact filter from those keys: a Bloom filter (mode BLOOM), an exact sorted key list (mode EXACT), or a pass-everything filter (mode PASS).
3. Send the filter to B as a query parameter. B keeps only rows of S whose key passes the filter. This is the semijoin S ⋉ R' (approximate, i.e. a superset, when using Bloom).
4. A runs the normal exact join, which removes Bloom false positives, so the final result is ALWAYS exactly correct.
A cost model chooses the mode and the Bloom filter size m (dynamic sizing, chosen per query).

## Environment
- PostgreSQL 18 is installed from packages (not built from source). Our C extension is built with PGXS using pg_config.
- The two "sites" are two databases, site_a and site_b, on the same local PostgreSQL server. postgres_fdw connects from site_a to site_b over localhost, so data really goes through a network connection.
- From Batch 6 on, the PostgreSQL 18 source (same minor version as the installed one) is in ./pg-src. Only contrib/postgres_fdw is modified and rebuilt with `make USE_PGXS=1`.

## Fixed design decisions (do not change without asking the user)
- Our extension is named `sjbloom`. It is installed in BOTH site_a and site_b, in schema public. It stays at version 1.0. After changing its SQL file, recreate it with DROP EXTENSION sjbloom CASCADE; CREATE EXTENSION sjbloom; (no upgrade scripts).
- Version 1 supports only: INNER JOIN, one equality join condition, int4 join keys.
- Hashing: always use PostgreSQL's own hash function hashint4extended(key, seed) via DirectFunctionCall2. Never hash raw bytes.
- The k bit positions come from double hashing: h1 = low 32 bits of the hash, h2 = (high 32 bits) | 1, position_i = (h1 + i*h2) mod m, for i = 0..k-1.
- A filter is a bytea: a 24-byte header, then a payload. The header holds version uint8, mode uint8, k uint16, nkeys uint32, m uint64, seed uint64. Always read/write the header with memcpy, never by casting pointers.
- Modes: 1 = BLOOM (payload = ceil(m/8) bytes of bits), 2 = EXACT (payload = sorted, unique int4 keys), 3 = PASS (no payload; every key passes).
- The filter SQL functions are IMMUTABLE STRICT PARALLEL SAFE. postgres_fdw only ships immutable functions to the remote server, and STRICT gives correct NULL handling automatically.
- postgres_fdw sends query parameters as TEXT: a bytea filter travels as hex text (2 bytes per filter byte, plus "\x"), and an int4[] travels as text like {12,345}. Rows also come back as text. All byte estimates must use these text sizes.
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
