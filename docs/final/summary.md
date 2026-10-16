# Final evaluation: cost-based adaptive semi-join filtering for postgres_fdw

**Setup.** Site A joins its local table `orders` (200,000 rows) with `customers`, a table at site B that A reads through postgres_fdw. A query keeps the orders with `order_id <= n` and joins them with their customers.

We compare five strategies:

- **original**: plain postgres_fdw. Site B sends all of `customers`, and A does the join.
- **fdw_param**: plain postgres_fdw with `use_remote_estimate`. The planner may then pick postgres_fdw's own plan that sends one remote query per key. Measured only for n ≤ 10,000.
- **index**: our rewrite, forced to the index path. A sends B the list of needed keys, and B uses its primary-key index.
- **filter**: our rewrite, forced to the filter path. A sends a filter (a Bloom filter, an exact key list, or a pass-everything filter), and B checks every row against it.
- **auto**: our system. The cost model chooses the path, and for the filter path also the mode and the Bloom size.

**How we measured.** Each strategy ran in a fresh connection: one warm-up run, then the median of 3 timed runs, measured end to end from Python. Network bytes are the real byte counters of the loopback interface (`/proc/net/dev`).

**Correctness.** Every result was compared with the original result using EXCEPT ALL in both directions.

The defaults are 1,000,000 remote customers, every order key present at B, uniform keys, localhost (1000 Mbit/s) and the filter sent as hex text. Each experiment changes one of these factors.

## Headline numbers (38 configurations, 180 strategy runs)

| | |
|---|---|
| Correct results | **180 of 180** runs return exactly the original rows |
| Speedup of auto over plain postgres_fdw | **36× on average** (geometric mean), median 25×, from **1.35×** to **2,597×** |
| Network bytes | **96.6% less**: 63 MB instead of 1,839 MB over all configurations |
| Correct choice (auto picked the fastest of original / index / filter) | **34 of 38 (89%)** |
| Regret = auto time / fastest time | mean **1.07**, median **1.00**, worst **2.38** (E3, n = 50,000, only 10% of the keys exist at B) |

**What regret means.** Regret is how much slower our choice was than the best choice, measured afterwards: 1.00 means we were as fast as the best strategy. We report it next to "% correct" because a wrong choice is not always a costly one. Near the break-even point the two paths take almost the same time, so picking the "wrong" one costs only a few percent: E2 with n = 10,000 was a wrong choice with regret 1.08. Regret measures what a mistake actually costs.

Some regrets are slightly below 1.00 (0.59 at worst). Auto and the forced path run the same plan, but in separate sessions. Runs under about 50 ms vary by up to ±30% between sessions on this WSL machine, so these values are noise. If we compare the forced run of the path auto chose with the best path instead, the mean regret is 1.08.

Auto was never slower than plain postgres_fdw.

## The experiments

**E1: number of local keys (n) and row width** (`time_vs_n_*.png`, `bytes_vs_n_*.png`)
- Plain postgres_fdw always moves all 1,000,000 customers: 1.2–1.8 s, and 34 MB for narrow rows or 119 MB for wide rows. Auto is **591× faster at n = 100** and still **2.9× faster at n = 150,000**.
- Auto chose correctly in all 10 cases. It uses the index path up to n = 10,000, and a Bloom filter from n = 50,000 on.
- At n = 50,000 the two paths are almost tied: index 310 ms vs filter 297 ms.
- Row width changes the bytes (17 MB instead of 5 MB at n = 150,000) but not the choice.
- postgres_fdw's own per-key plan (fdw_param) is 10–25× slower than auto at n = 100–1,000. At n = 10,000 the planner falls back to fetching the whole table.

**E2: size of the remote table** (`e2_s_rows.png`)
- **3,000,000 customers:** the filter path must scan 3× more rows at B, so the index path wins even at n = 50,000. Auto chose it every time: 8.5–473× faster than plain postgres_fdw, which takes 3.6–4.2 s.
- **100,000 customers:** plain postgres_fdw costs only 110–160 ms, so there is less to win: **1.35×** at n = 50,000, the smallest speedup of the whole evaluation. At n = 10,000 index (34.6 ms) and filter (38.7 ms) are within 12% of each other, and auto picked the filter path (regret 1.08).

**E3: match fraction, i.e. how many local keys exist at B** (`e3_match_fraction.png`)
- When only 50% or 10% of the keys exist at B, the index path gets much faster. At n = 50,000 it drops from 310 ms to 151 ms and then 80 ms. A lookup of a missing key stops in the index: no table row is read and nothing is sent back.
- The cost model assumes **every key matches**, and its per-key lookup time was calibrated with existing keys. So it predicts about 312 ms for the index path, and at n = 50,000 it picks the filter path. This gives the two **worst regrets: 1.82 (50%) and 2.38 (10%)**.
- The byte estimates are too high here by the same factor (measured / estimated = 0.21–0.60).
- Auto is still 5–7× faster than plain postgres_fdw.

**E4: skewed keys** (`e4_skew.png`)
- With `customer_id = 1 + floor(s_rows · random()³)`, most orders point to small customer ids.
- At n = 1,000 and 10,000 auto chose correctly.
- At n = 50,000 the index path took 227 ms against 270 ms predicted. The keys lie close together, so their index and table pages are mostly in cache. The filter path took 305 ms against 232 ms predicted; this filter-path underestimate is the known 20–30% from Batch 5b. Auto picked the filter path (**regret 1.27**).

**E5: network bandwidth, real network emulation (tc netem)** (`e5_bandwidth.png`)
- We slowed down the loopback interface with `tc qdisc ... netem rate 100mbit delay 1ms`, and then `rate 10mbit`. We told the cost model about this network:
  - `semijoin_network_mbit` = the netem rate;
  - `semijoin_per_message_ms` = the measured round trip through postgres_fdw: **2.26 ms** at 100 Mbit and **2.42 ms** at 10 Mbit, against 0.062 ms on plain localhost.
- Plain postgres_fdw then takes **25.6 s** (100 Mbit) and **50.5 s** (10 Mbit). It needs 10,001 FETCH round trips, and at 10 Mbit also 27 s just for its 34 MB.
- Auto chose correctly in **all 10 cases** and was **2,597× faster** at n = 100 (19 ms) and 6.4× faster at n = 150,000.
- With the right network numbers the model's predictions are very close: the measured time is 0.96–1.05 × the predicted time for n ≥ 10,000.
- The filter mode adapts to the network:
  - At 100 Mbit with slow round trips, the model prefers **EXACT** key lists. Bloom false positives are extra rows, and extra rows cost FETCH round trips.
  - At 10 Mbit bytes are expensive, and it prefers the smaller **Bloom** filter from n = 10,000 on.
- **Model not told about the network (auto_loopback, 10 Mbit):** with the loopback numbers, auto still made the same 5 decisions (regret ≤ 1.02). Its predicted times, however, were **10–20× too low**: 244 ms predicted for a query that took 2,894 ms, and 392 ms for 7,921 ms (red crosses in `predicted_vs_measured.png`). The choice survived only because both of our paths send back the same rows: a slow network makes both slower by about the same amount, so their order does not change. Anything that compares our predicted time with a different kind of plan would be misled.
- **fdw_param** is hit hardest by the slow network: one remote query per key costs 6.7 s at n = 1,000.

**E6: binary filter** (`e6_binary_filter.png`)
- With `semijoin_binary_filter = on` the filter is sent as raw bytes instead of hex text. The filter on the wire shrinks from 181 KB to 99 KB at n = 50,000, and from 433 KB to 242 KB at n = 150,000.
- Because bits became cheaper, the model also chose a slightly bigger Bloom filter (n = 150,000: m = 1.93 M bits, k = 10, instead of 1.73 M bits, k = 9). At n = 10,000 it chose an exact key list instead of Bloom.
- On localhost the time hardly changes at n = 50,000 and 150,000 (297 → 306 ms, 519 → 523 ms), because the filter path is dominated by B checking 1,000,000 rows (≈ 160 ns each). At n = 10,000 it fell from 207 to 169 ms, because an exact key list is also faster to check at B than a Bloom filter. At 10 Mbit, 190 KB saved would be about 150 ms.
- Choices: 3 of 3 correct.

## Where our method loses, and why

| case | regret | why |
|---|---:|---|
| E3, n = 50,000, 10% of keys at B | 2.38 | the model assumes every key exists at B; lookups of missing keys are much cheaper than calibrated |
| E3, n = 50,000, 50% of keys at B | 1.82 | same reason |
| E4, n = 50,000, skewed keys | 1.27 | clustered keys make index lookups cache-friendly (not modelled), and the filter path is under-predicted by 20–30% at large n (known since Batch 5b) |
| E2, n = 10,000, 100,000 customers | 1.08 | a near tie (34.6 vs 38.7 ms): a wrong choice at the break-even point costs little |

Two more cases are worth knowing:

- **The filter path at small n.** At n = 100 the filter path is about 50× slower than the index path (100 ms vs 2 ms), because B checks all 1,000,000 rows. The model never chose it there.
- **Measurement noise.** E3 with n = 10,000 and 50% of the keys at B shows regret 1.32, although auto chose the right path. Auto and index ran the identical plan; the difference is session-to-session noise.

## Known limitations

- **Version 1 scope:** only INNER JOIN, one equality join condition, int4 keys. Set operations (such as EXCEPT ALL) and queries planned in parallel mode are not rewritten.
- **Not in the cost model:**
  - the share of keys that exist at B (E3);
  - how close together the keys lie (E4);
  - the about 4 fixed round trips postgres_fdw makes per query (START TRANSACTION, DECLARE, CLOSE, COMMIT). On localhost they cost 0.25 ms; with 1 ms delay they cost about 9 ms, which is why small-n predictions in E5 are 8–11 ms too low.
- **The network must be calibrated.** `semijoin_network_mbit` and `semijoin_per_message_ms` are settings, not measured automatically. E5 shows that with the wrong values the predicted times are 10–20× off.
- **Other conditions on the foreign table:** the path decision ignores conditions on the foreign table alone (such as `c.city = 'X'`). Only the row estimate accounts for them.
- **Join-size estimate:** the join size is still PostgreSQL's own estimate, so n = 100 gets a Nested Loop. It is just as fast as a Hash Join there.
- **Bloom false positives:** at small m the measured false-positive rate is above the textbook formula. This is documented in `docs/fp_investigation.md` and not modelled.
- **Timing noise:** this WSL machine is noisy. t_check_ns varied 150–188 ns between calibration runs. The filter path is predicted 20–30% too fast at large n, so on localhost the model moves to the filter path a little too early.
- **The machine's state matters.** Before the final run the host was in a power-saving state and every round trip was 2–3× slower than at calibration time. We waited until the round trip was back at ~50 µs (charger plugged in, Best performance mode) before measuring.

## How the bytes were measured

We read the byte counters of the loopback interface `lo` in `/proc/net/dev` just before and just after each query. On loopback every packet sent is also received, so received = sent; we report one of them. Our own client connects to site A through a Unix socket, which does not use `lo`. So the only traffic counted is postgres_fdw's TCP connection from A to B: the filter or key list, the rows, the protocol messages and the TCP/IP headers.

Background traffic from other programs, measured with no query running, was a median of 0.5–1.9 KB/s, with one 42 KB/s burst under netem. That is negligible next to the 5 KB–119 MB per query.

Our byte estimates were within ±6% of the measured bytes for every transfer above 1 MB, except E3, where the model assumes all keys match.

## Charts

| chart | shows |
|---|---|
| `time_vs_n_narrow.png`, `time_vs_n_wide.png` | E1: end-to-end time vs n, per strategy |
| `bytes_vs_n_narrow.png`, `bytes_vs_n_wide.png` | E1: real bytes on the network vs n |
| `regret.png` | regret per configuration (purple = a wrong choice), line at 1.0 |
| `speedup.png` | speedup of auto over plain postgres_fdw, per configuration |
| `predicted_vs_measured.png` | the model's predicted time vs the measured time, with y = x; red = model not told about the network |
| `e2_s_rows.png` … `e6_binary_filter.png` | one chart per experiment |

The full decision-quality table (one row per configuration) is in `decision_quality.md` and `decision_quality.csv`.
