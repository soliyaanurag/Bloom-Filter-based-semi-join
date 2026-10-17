# Verification report: Bloom-filter semijoin for postgres_fdw

Repository: `soliyaanurag/Bloom-Filter-based-semi-join`, commit `b6f4f3a` (master). Verified on 2026-10-17.

## 1. Verdict

**The project works.** I built it from scratch exactly as the README says, ran every test and benchmark that can run without `sudo tc`, and checked each claim in the docs against the real behaviour.

- **Correctness: confirmed, with no exceptions.** Every query I ran returned exactly the rows plain postgres_fdw returns: 180 of 180 benchmark runs, 18 of 18 `correctness_auto.sql` comparisons, and 24 of 24 extra edge-case checks I wrote myself (NULL keys, duplicates, int4 limits, empty inputs, missing keys, different query shapes). A Bloom filter never lost a key (0 false negatives in 200,000 checks).
- **Performance: confirmed in shape.** Auto was never slower than plain postgres_fdw. It was 30× faster on average (the doc says 36×) and sent 96.6% fewer bytes (the doc says 96.6%). The exact numbers differ because this is a different machine.
- **Decision quality: mostly confirmed.** Auto picked the fastest path in 30 of 38 cases (the doc says 34 of 38). Every extra miss is a near-tie at n ≥ 50,000, where the two paths are within about 5 to 20% of each other and this machine's run-to-run noise is about as large. The documented weak spots (E2 near-tie, E3 missing keys, E4 skewed keys) failed here too, for the documented reasons.
- **Bugs: none in the extension or the postgres_fdw patch.** I found one small portability bug in a benchmark script (section 5.1, now fixed). It does not affect results.
- **Not checkable here:** the real slow-network experiment E5. This sandbox's kernel has no `netem`, so I ran the script's own `--simulated` mode instead. Simulation adds bandwidth delay but no latency, so the documented 25.6 s / 50.5 s / 2,597× numbers cannot be confirmed or refuted.

Overall I would call the project a **success**: the core claims (always exact, much faster, far fewer bytes, a sensible cost model) hold. The "important finding" in the docs, that always-index is about as good as auto on int4 keys, also reproduces.

## 2. What I ran, and where

| item | value |
|---|---|
| Machine | cloud Linux VM, 4 vCPU, 15 GB RAM (not WSL, not the authors' laptop) |
| PostgreSQL | 18.6, built from source from tag `REL_18_6` (GitHub mirror of postgresql.git), `--enable-debug --without-icu` |
| Patch | `patches/postgres_fdw-semijoin.patch`, applied with `git apply` |
| Sites | one server on port 5432, two databases `site_a` and `site_b`, as in the README (postgres_fdw connects over TCP localhost) |
| Data | `setup/setup_all.sh`: 1,000,000 customers at B, 200,000 orders at A (181,329 distinct keys) |
| Cluster | `initdb -E UTF8 --locale=C.UTF-8`; server run as an unprivileged OS user |

The raw outputs (logs, CSV-derived tables, charts) were kept in the project folder, not in this repository. The step-by-step guide to repeat everything is [manual_test_guide.md](manual_test_guide.md).

## 3. Build and test results

| check | result | evidence |
|---|---|---|
| Patch applies to plain REL_18_6 (`git apply --check`) | **confirmed**: no output, applied cleanly | |
| PostgreSQL + patched postgres_fdw build | **confirmed**: no compiler warnings | |
| sjbloom builds with PGXS | **confirmed**: no warnings | |
| `make -C sjbloom installcheck` | **confirmed**: `ok 1 - sjbloom_basic`, "All 1 tests passed" | |
| `setup/setup_all.sh` | **confirmed**: all 6 steps finish; server options end as `{host=localhost,port=5432,dbname=site_b,extensions=sjbloom,semijoin_filter=true}`. It took 8 s here (README says about 30 s). | |
| EXPLAIN shows the rewrite (README step 6) | **confirmed**: `Remote SQL: ... WHERE ((customer_id = ANY ($1::integer[])))`, `Semijoin Path: index`, `Estimated Keys: 980`, `Predicted Time: index=6.229 ms filter=159.965 ms` | |
| `queries/correctness_auto.sql`: 18/18 zero | **confirmed**: 18 rows, every `off_except_on` and `on_except_off` is 0 | |
| `make installcheck REGRESS=semijoin` (the project's own test) | **confirmed**: `ok 1 - semijoin` | |
| postgres_fdw's own `make installcheck` "still passes" | **confirmed as "no regression"**: `query_cancel` passes; `postgres_fdw` shows 2 diff hunks, both `user mapping not found for user "claude"`. **The unmodified postgres_fdw from the same tag gives the identical 2 hunks** on this machine, so this comes from the test setup (the regression user differs from the OS user the server runs as), not from the patch. | `postgres_fdw_installcheck.diffs` |

## 4. Every documented claim, checked

Legend: **Confirmed** = I measured the same thing (timings within normal machine differences). **Partly** = the claim's direction holds but a number or count differs. **Contradicted** = my measurement says otherwise. **Not checkable** = needs something this sandbox lacks.

### 4.1 Correctness and design claims

| claim (source) | verdict | what I saw |
|---|---|---|
| Final result is always exactly correct (README, CLAUDE.md) | **Confirmed** | 180/180 benchmark runs, 18/18 correctness_auto, 24/24 own edge cases, all EXCEPT ALL both directions = 0 |
| Bloom has no false negatives | **Confirmed** | 100,000 random int4 keys in a tiny filter (m = 50,000, k = 3) and in an auto-sized filter: 0 keys failed |
| NULL keys are excluded, and STRICT gives NULL for NULL inputs | **Confirmed** | NULL order key never joins in any path; `sjb_contains(NULL, 1)` and `sjb_contains(f, NULL)` are both NULL |
| Empty R' gives an empty join (EXACT filter with 0 keys) | **Confirmed** | `order_id > 100` on a 10-row table: 0 rows in every path |
| Negative keys, 0, 2147483647 and −2147483648 work | **Confirmed** | correct in index and filter paths (EXACT search compares with < and >, no overflow) |
| A filter of another version is rejected with a clear error | **Confirmed** | `ERROR: sjbloom: filter has version 2, but this sjbloom understands only version 3` + HINT |
| Damaged filters give errors, not crashes | **Confirmed** | too short: `filter is too small (2 bytes, need at least 24)`; bad mode: `unknown filter mode 9` |
| LEFT JOIN, OR across both tables, volatile functions, two join conditions, `$n` parameters are not rewritten | **Confirmed** | plans show the plain `Remote SQL: SELECT customer_id, name FROM public.customers` |
| Conditions on the foreign table alone are allowed; comma joins, reversed `=`, no alias, DISTINCT/ORDER BY/LIMIT, OR/NOT IN on the local table, aggregates, GROUP BY all rewrite correctly | **Confirmed** | all 0/0 differences |
| The InitPlan builds the filter once and it is sent as `$1` | **Confirmed** | EXPLAIN ANALYZE shows one InitPlan and `Semijoin Filter: mode=bloom keys=139294 m=1730968 k=9 bytes_sent=432792` |
| Without the Batch 7 hint the planner guessed 10 rows; now n = 1000 gets a Hash Join, 117 ms → 6.7 ms | **Confirmed** | Foreign Scan estimate 980 vs 999 real; Hash Join; auto 9.4 ms |
| n = 100 still gets a Nested Loop, and that is as fast | **Confirmed** | Nested Loop at n = 100; auto 2.9 ms |
| Key estimate n_est at n = 150,000 ≈ 136,067 vs 139,294 true (Yao/Dell'Era) | **Confirmed** | true distinct = 139,294 exactly; n_est = 137,326 / 135,969 (varies with ANALYZE's random sample) |
| Filter SQL functions are IMMUTABLE STRICT PARALLEL SAFE, and are shipped to B | **Confirmed** | Remote SQL contains `public.sjb_contains($1::bytea, customer_id)` |

### 4.2 False-positive study (`docs/fp_investigation.md`)

These numbers are deterministic (they depend only on the hash function), so they must match exactly. **They all match exactly.**

| table in the doc | verdict | this run |
|---|---|---|
| fp_rate.sql, b = 4/8/12/16/22 | **Confirmed** | 0.145933, 0.021308, 0.003179, 0.000445, 0.000020: identical |
| v3 synthetic keys n = 100 / 1,000 / 10,000 / 10,000 (b = 10) | **Confirmed** | 14, 35, 300, 8,363: identical |
| v3 real keys n = 100 / 1,000 | **Confirmed** | 23, 36: identical |
| Pairs with exactly a member's positions: 13 (exp. 14.3) and 2 (exp. 2.1) | **Confirmed** | 13 and 2: identical |
| Measured FP at large filters matches the formula | **Confirmed** | n = 150,000 run: 2,244 false positives (141,538 rows back − 139,294 keys); formula predicts about 2,200 |

I did not re-run the v1/v2 columns (they need the old `fp-investigation` branch).

### 4.3 Final evaluation (`docs/final/summary.md`)

I ran `bench/final_eval.py` (E1, E2, E3, E4, E6) and `--e5 100 --simulated` / `--e5 10 --simulated`, then `bench/final_report.py`. Full table: `decision_quality_this_run.md`.

**Headline numbers**

| headline | doc | this run | verdict |
|---|---|---|---|
| Correct results | 180 / 180 | **180 / 180** | **Confirmed** |
| Auto never slower than plain postgres_fdw | yes | yes (0 of 38) | **Confirmed** |
| Network bytes saved | 96.6% | **96.6%** (1,837.6 MB → 62.6 MB) | **Confirmed** |
| Speedup of auto (geometric mean) | 36× (1.35× to 2,597×) | **30×** (1.42× to 3,837×) | **Partly**: same order. Without E5, which differs in method, it is 24.7× (doc) vs 21.8× (here) |
| Correct choice | 34 / 38 | **30 / 38** | **Partly**: see "Decisions" below |
| Regret mean / worst | 1.07 / 2.38 | **1.10 / 2.07** | **Confirmed** within noise |
| Byte estimates within ±6% above 1 MB, except E3 | yes | yes: 0.97 to 1.05 everywhere; E3 0.57 to 0.61 | **Confirmed** |

**Per experiment**

| claim | verdict | this run |
|---|---|---|
| E1: plain postgres_fdw takes 1.2 to 1.8 s and moves 34 MB (narrow) / 119 MB (wide) | **Confirmed** | 1.22 to 1.85 s; 33.86 MB / 119.16 MB |
| E1: auto 591× faster at n = 100, 2.9× at n = 150,000 | **Partly** | 439× and 2.6× (same shape, different machine) |
| E1: index path up to n = 10,000, Bloom from 50,000 | **Confirmed** | exactly that pattern |
| E1: auto right in all 10 cases | **Partly** | 9 of 10; n = 50,000 wide picked filter (433 ms) where index took 369 ms. The doc itself calls n = 50,000 "almost tied". |
| E1: fdw_param 10 to 25× slower than auto at n = 100 to 1,000; falls back to full fetch at 10,000 | **Confirmed** | 205 ms vs 9.4 ms at n = 1,000 (22×) |
| E2 (3M customers): index wins even at n = 50,000; auto always picks it; plain fdw 3.6 to 4.2 s | **Confirmed** | 3.7 to 4.3 s; auto picked index all 3 times |
| E2 (100k customers): smallest speedup ~1.35×; n = 10,000 near-tie, auto picks filter (wrong, regret 1.08) | **Confirmed** | 1.42×; auto picked filter at n = 10,000 again (31 ms index vs 37 ms filter) |
| E3: index path gets faster when keys are missing at B; auto picks filter at n = 50,000; worst regrets 1.82 / 2.38 | **Confirmed** | index 167 ms (50%) and 80 ms (10%); auto picked filter; regrets 1.76 / 2.07 |
| E4: skewed keys, n = 50,000, auto picks filter (wrong, 1.27) | **Confirmed** | wrong again, regret 1.49 |
| E5 (netem 100/10 Mbit + 1 ms delay): 25.6 s / 50.5 s plain, 2,597×, predictions within 0.96 to 1.05 | **Not checkable** | no `netem` in this kernel ("Specified qdisc kind is unknown"). The simulated mode ran (51 runs, all correct): plain fdw 3.9 s / 28.3 s simulated, auto up to 3,837× faster, auto_loopback made the same choices as auto at 10 Mbit. Simulation has no latency, so it cannot test the round-trip claims or the EXACT-at-100 Mbit claim. |
| E6: binary filter shrinks wire bytes 181 → 99 KB (n = 50,000) and 433 → 242 KB (n = 150,000) | **Confirmed** | 180,630 → 99,103 and 432,792 → 241,513 bytes: identical |
| E6: binary makes a bigger Bloom (1.93 M bits, k = 10 vs 1.73 M, k = 9) and EXACT at n = 10,000 | **Confirmed** | m = 1,931,907 k = 10 vs 1,730,968 k = 9; EXACT at n = 10,000: identical |

**Policy comparison (Batch 8b)**

| claim | doc | this run | verdict |
|---|---|---|---|
| always-index mean / worst regret | 1.03 / 1.22 | 1.02 / 1.30 | **Confirmed** |
| auto mean / worst regret | 1.08 / 2.51 | 1.08 / 2.37 | **Confirmed** |
| always-filter is poor | 8.08 mean | 6.72 mean | **Confirmed** |
| "Always index is slightly better than auto on this workload" | yes | yes | **Confirmed** |
| "The big win is the semijoin itself, not the choice between paths" | yes | yes: plain fdw is never within 5% of the best | **Confirmed** |
| Wide keys (UUID) would make Bloom matter more | calculation | not measurable (v1 is int4 only) | **Not checkable** (the arithmetic is right: the filter's size depends on the number of keys, not their width) |

### 4.4 Cost model and calibration

| claim | verdict | this run |
|---|---|---|
| Formulas in CLAUDE.md match the code | **Confirmed** | I read `costmodel.c`: bytes, messages, rows back, b\* = ln(3.844·(S−M)·w/(W·n))/0.4805 (correct minimum of bytes), k = round(0.693·b), fp = (1−e^(−kn/m))^k |
| Calibration constants are machine-specific | **Confirmed** | `calibrate.py` here: t_check_ns 154.57 (doc 158.19), t_lookup_us **3.233** (doc 4.7956), per_message_ms 0.0555 (0.06194), per_row_us 0.519 (0.548), t_build_us 0.241 (0.245) |
| The filter path is predicted 20 to 30% too fast at large n | **Confirmed, and larger here** | n = 150,000: predicted 385 to 392 ms, measured 552 to 775 ms (30 to 50% too fast) |
| Predicted break-even ~35,000 keys, measured ~65,000 | **Confirmed in direction** | around n = 50,000 the two paths swap order from run to run |

With this machine's own constants I re-ran E1 and E6: still 11 of 13 right, but the two misses moved to different near-tie cases. That shows the misses come from noise near the break-even point, not from the default constants.

## 5. Bugs and problems found

### 5.1 Bug (small, benchmark script only): `bench/final_eval.py` crashes on a SQL_ASCII cluster

`unix_socket_dir()` (line ~202) does `setting.split(",")`. If the cluster was created with `initdb` under a C/POSIX locale, its encoding is SQL_ASCII and psycopg returns `bytes`, so the script stops at once with `TypeError: a bytes-like object is required, not 'str'`. On the authors' machine the cluster is UTF8, so they never saw it. **Workaround:** create the cluster with `initdb -E UTF8 --locale=C.UTF-8`.  Fixed in the same pull request that added this report: the value is decoded to text when psycopg returns bytes (tested on a SQL_ASCII database).

### 5.2 Not bugs, but worth knowing

1. **postgres_fdw's own regression test is sensitive to the OS user.** Its `loopback` user mappings connect without a user name, so the server's OS user must exist as a role. On a fresh setup where `initdb -U postgres` was used, create a role for the OS user (`CREATE ROLE <os_user> SUPERUSER LOGIN`) before `make installcheck`, or the `semijoin` test fails at `ANALYZE ft_customers` and the main test shows 2 diff hunks. The README's own order (initdb as your user, then create `postgres`) avoids this.
2. **Only the top level of a query is looked at.** If the join is inside a subquery, a view or a `WITH` query, for example `SELECT count(*) FROM (SELECT ... FROM orders o JOIN customers c ...) x`, it is **not** rewritten, and `semijoin_verbose` prints no NOTICE explaining why (I checked: the Remote SQL has no WHERE). The same join written directly is rewritten. The docs mention "subquery" in the refusal list, but this case is easy to hit by accident.
3. **Queries with bind parameters are never rewritten.** Any condition with `$1` is refused (by design: the key query is built as SQL text). Most application drivers send parameters (psycopg 3, JDBC, prepared statements), so in real applications the feature only applies to queries with literal constants. The docs mention "parameters are refused" but not this consequence.
4. **No fallback at run time.** If site B lacks sjbloom, or has another `SJB_VERSION`, a filter-path query fails with an error instead of falling back to plain postgres_fdw. The index path does not need sjbloom at B.
5. **The index path's key list has no size limit.** `max_filter_kb` limits only BLOOM/EXACT filters. A very large R' (millions of keys) would send a multi-megabyte `int[]` parameter.
6. **Same CPU architecture on both sites.** The filter header is copied with `memcpy` in native byte order. A and B on machines with different endianness would misread m and k. Not an issue for x86/ARM-little-endian pairs, but it is an unstated assumption.
7. **Near the break-even point (about n = 50,000 here), the choice is a coin toss.** Both paths are within 5 to 20%, and run-to-run noise on this VM is about as large. "Correct choice" counts there say little; regret is the better measure, as the docs argue.
8. **Absolute timings will not match the docs on other hardware.** That is expected: on this VM an index lookup cost 3.2 µs instead of 4.8 µs. Run `bench/calibrate.py` and set the `postgres_fdw.semijoin_*` constants for serious use.

## 6. Code review notes

I read `sjbloom/bloom.c`, `sjbloom/sjbloom.c`, `sjbloom/costmodel.c` and `contrib/postgres_fdw/semijoin.c` (the safety checks, key-query building and estimate). Things that are done well:

- Every filter read checks the size, version, mode and payload length before touching the payload.
- The rewrite is conservative: only one local + one foreign table, only INNER joins, exactly one `int4 = int4` join clause, every other condition must touch one table only, local conditions are refused if volatile or parameterized, and RLS/TABLESAMPLE are refused.
- Local conditions are copied into the key query, so the filter keys are always a superset of the keys the main query can join on. This is why correctness holds.
- Planning steps run in a subtransaction, so an error there leaves the original query unchanged.
- `compare_int32` avoids overflow, and the EXACT binary search uses 64-bit indexes.

I found no correctness bug in the C code.

## 7. Evidence files (kept in the project folder, not in this repository)

| file | content |
|---|---|
| `decision_quality_this_run.md` | all 38 configurations, this run |
| `headline_this_run.md`, `policy_comparison_this_run.md` | headline and policy tables, this run |
| `final_eval_E1-E4_E6.log`, `final_eval_E5_simulated.log` | raw benchmark output |
| `final_eval_E1_E6_local_constants.log` | E1/E6 re-run with this machine's calibrated constants |
| `calibrate_this_machine.log` | calibration output |
| [queries/edge_cases.sql](../queries/edge_cases.sql), `edge_cases.out` | my extra correctness and refusal tests |
| `fp_cases.out` | false-positive study output |
| `postgres_fdw_installcheck.diffs` | the 2 environment-only diff hunks |
| `*.png` | the charts from `final_report.py` for this run |
