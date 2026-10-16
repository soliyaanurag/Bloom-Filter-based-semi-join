<!-- bench/: Python (psycopg 3 + matplotlib) scripts that run the benchmarks and draw the result plots. -->

# bench/

| File | Job |
|---|---|
| `requirements.txt` | Python packages (psycopg 3, matplotlib) |
| `common.py` | helpers: connect, EXPLAIN JSON, find the Foreign Scan, row width, correctness check, table printing |
| `strategies.py` | SQL of each strategy (baseline, exact_keys, bloom_fixed10, bloom_dynamic) and of its bytes A->B |
| `run_experiments.py` | main benchmark, appends to `results/results.csv` |
| `b_sweep.py` | total bytes vs Bloom bits per key b, writes `results/b_sweep.csv` |
| `plot.py` | draws the PNG charts in `results/` |
| `run_all.sh` | runs the three scripts above in order |
| `time_contains.py` | times site B's full scan with `sjb_contains` (median of 5), to compare two builds of the C code |
| `calibrate.py` | Batch 5: measures site B's `t_check_ns` and `t_lookup_us` (parallel workers off, and once on for comparison), writes `results/calibration.json` |
| `validate_model.py` | Batch 5: predicted time (`sjb_choice_report`) vs measured time for exact_keys and bloom_dynamic, narrow rows |
| `capture_bytes.sh` | prints (does not run) tcpdump/capinfos commands for a packet-capture cross-check |

Rerun everything: `source setup/env.example.sh && bench/run_all.sh`. See docs/WALKTHROUGH.md, Batch 4.
