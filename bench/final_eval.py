"""
final_eval.py: Batch 8, the final evaluation (measurements only).

It compares five strategies for the join orders ⋈ customers, for the
orders with order_id <= n:

  original   plain postgres_fdw (postgres_fdw.enable_semijoin = off):
             site B sends all of customers. This is the baseline.
  fdw_param  plain postgres_fdw with the table option use_remote_estimate
             on, so the planner may pick postgres_fdw's own parameterized
             plan (a Nested Loop that sends one remote query per key).
             Only for n <= 10000, because it is slow beyond that.
  auto       our system: the planner hook picks the path (cost model)
  index      our rewrite, path forced to "index"  (customer_id = ANY(keys))
  filter     our rewrite, path forced to "filter" (sjb_contains(filter, ...))

How one strategy is measured (function run_strategy):
  1. A FRESH connection to site_a, with the strategy's settings.
  2. One warm-up run: EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON) of
     CREATE UNLOGGED TABLE final_chk_<strategy> AS <query>. It opens
     postgres_fdw's connection to site B, gives us the plan details (join
     type, Foreign Scan rows, our Semijoin lines), and stores the result
     for the correctness check (the "temp-table method": a set operation
     such as EXCEPT ALL is never rewritten, so each result is stored first).
  3. Three timed runs of the plain SELECT. time_ms is the median wall-clock
     time measured here in Python, from sending the query until the whole
     result has arrived (planning + execution + sending the rows).
  4. Real network bytes: the counters of the loopback interface "lo" in
     /proc/net/dev, read just before and just after each timed run. Our
     own connection to site_a uses a Unix socket, so it does NOT go
     through "lo"; the only traffic there is postgres_fdw's connection
     from site_a to site_b (localhost, TCP). measured_bytes is the median.
  5. Correctness: (original EXCEPT ALL new) and (new EXCEPT ALL original)
     must both have 0 rows, and the timed runs must return as many rows as
     the original.

Experiments (one factor at a time, defaults in brackets):
  E1  n x width (narrow, wide)                        [s_rows 1M, match 1.0,
  E2  s_rows in 100000, 1000000, 3000000                uniform, 1000 Mbit,
  E3  match fraction in 1.0, 0.5, 0.1                   binary filter off]
  E4  skewed keys
  E5  network bandwidth (tc netem on "lo", or simulated)
  E6  binary filter on
The default point of E2, E3, E4, E5 and E6 is the same configuration as an
E1 narrow run, so it is measured once, in E1 (final_report.py reuses it).

Results: one CSV per experiment in results/final/ (e1.csv, e2.csv, ...).
final_report.py turns them into the decision-quality table and the charts.

Run from the project folder:
    source setup/env.example.sh
    .venv/bin/python bench/final_eval.py              # E1, E6, E2, E3, E4,
                                                      # then restores the data
                                                      # and checks correctness
    .venv/bin/python bench/final_eval.py --only E1    # one experiment
    .venv/bin/python bench/final_eval.py --e5 100     # E5 at 100 Mbit, while
                                                      # tc netem is active

E5 tells the cost model about the emulated network: semijoin_network_mbit =
the netem rate, and semijoin_per_message_ms = the round trip measured on it
(measure_fdw_round_trip). At 10 Mbit it also runs "auto_loopback": auto with
the calibrated loopback numbers, to show what a wrong network assumption
costs.
    .venv/bin/python bench/final_eval.py --e5 100 --simulated
                                                      # E5 without tc
"""

import argparse
import csv
import os
import re
import statistics
import subprocess
import time

import psycopg

import common
import strategies


# ---------------------------------------------------------------------------
# Settings of the evaluation
# ---------------------------------------------------------------------------

PROJECT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
RESULTS_DIR = os.path.join(PROJECT_DIR, "results", "final")
MAKE_DATA = os.path.join(PROJECT_DIR, "setup", "make_data.sh")
CORRECTNESS_SQL = os.path.join(PROJECT_DIR, "queries", "correctness_auto.sql")

N_GRID = [100, 1000, 10000, 50000, 150000]     # E1 and E5
N_SMALL = [1000, 10000, 50000]                 # E2, E3, E4
N_BINARY = [10000, 50000, 150000]              # E6
S_ROWS_VALUES = [100000, 1000000, 3000000]     # E2
MATCH_FRACTIONS = [1.0, 0.5, 0.1]              # E3

DEFAULT_S_ROWS = 1000000
DEFAULT_MATCH_FRACTION = 1.0
DEFAULT_SKEW = "uniform"
DEFAULT_MBIT = 1000.0

TIMED_RUNS = 3              # after 1 warm-up run; we keep the median
FDW_PARAM_MAX_N = 10000     # fdw_param only up to this n

STRATEGY_NAMES = ["original", "fdw_param", "auto", "index", "filter"]
# The strategies that use our planner hook, and the force_path each one sets.
# auto_loopback (E5 at 10 Mbit only) is auto with the cost model left at
# the calibrated LOOPBACK network numbers, to show what a wrong network
# assumption costs.
FORCE_PATH = {"auto": "auto", "index": "index", "filter": "filter", "auto_loopback": "auto"}

# The network numbers the cost model uses unless E5 changes them (the GUC
# defaults = our loopback calibration).
CALIBRATED_PER_MESSAGE_MS = 0.06194

E5_LABEL = "real network emulation (tc netem)"

# Each strategy stores its result in final_chk_<strategy> (in site_a).
CHECK_TABLE_PREFIX = "final_chk_"

CSV_COLUMNS = [
    # the configuration
    "experiment", "network_method", "n", "width", "s_rows", "match_fraction", "skew",
    "network_mbit", "binary_filter", "strategy",
    # what the cost model was told about the network (E5 changes these)
    "model_network_mbit", "model_per_message_ms",
    # times
    "time_ms", "local_time_ms", "time_runs_ms", "foreign_scan_ms",
    # bytes
    "measured_bytes", "measured_rx", "measured_tx", "estimated_bytes",
    # the plan
    "result_rows", "foreign_rows", "join_type", "path", "mode", "keys_sent",
    "bloom_m", "bloom_k", "param_bytes_sent", "n_est", "pred_index_ms", "pred_filter_ms",
    # the check
    "correct", "run_at",
]


# ---------------------------------------------------------------------------
# Small helpers: network counters, connections, progress
# ---------------------------------------------------------------------------

def read_loopback_bytes():
    """
    Read the byte counters of the loopback interface "lo".

    /proc/net/dev has one line per network interface, like
        lo: 3241472 13643 0 0 0 0 0 0 3241472 13643 0 0 0 0 0 0
    After the colon come 8 "receive" numbers and then 8 "transmit" numbers;
    the first of each group is the number of bytes. Reading this file needs
    no special rights (no sudo).

    Input:  none.
    Output: (bytes received, bytes sent) since the machine started.
    """
    with open("/proc/net/dev") as dev_file:
        for line in dev_file:
            name, separator, numbers = line.partition(":")
            if separator and name.strip() == "lo":
                fields = numbers.split()
                return int(fields[0]), int(fields[8])
    raise RuntimeError("no 'lo' interface in /proc/net/dev")


def measure_background_noise(seconds_per_window=1.0, windows=10):
    """
    Measure the loopback traffic when WE send no query at all, to know how
    much of measured_bytes might come from other programs on this machine.

    Input:  seconds_per_window, length of one window; windows, how many.
    Output: a dict with the bytes received in each window and the median
            and maximum per second.
    """
    window_bytes = []
    for window in range(windows):
        rx_before, tx_before = read_loopback_bytes()
        time.sleep(seconds_per_window)
        rx_after, tx_after = read_loopback_bytes()
        window_bytes.append(rx_after - rx_before)
    per_second = [value / seconds_per_window for value in window_bytes]
    return {
        "window_seconds": seconds_per_window,
        "window_rx_bytes": window_bytes,
        "median_bytes_per_s": statistics.median(per_second),
        "max_bytes_per_s": max(per_second),
    }


def unix_socket_dir():
    """
    Find the folder of the server's Unix socket (for example /tmp).

    We connect to site_a through this socket instead of TCP, so that our
    own client traffic never shows up in the "lo" counters.

    Input:  none (uses the normal PG* environment variables once).
    Output: the folder, as a string.
    """
    conn = common.connect("site_a")
    setting = conn.execute("SHOW unix_socket_directories").fetchone()[0]
    conn.close()
    # On a cluster with encoding SQL_ASCII (initdb under a C/POSIX locale),
    # psycopg cannot decode text and returns bytes instead of str. Turn it
    # into a str first, otherwise split(",") fails with a TypeError.
    if isinstance(setting, bytes):
        setting = setting.decode("ascii", errors="replace")
    return setting.split(",")[0].strip()


SOCKET_DIR = None   # set in main()


def connect_site_a():
    """
    Open a NEW connection to site_a through the Unix socket.

    autocommit: every statement runs on its own, SET stays for the session.
    prepare_threshold=None: psycopg never turns our query into a prepared
    statement, so every run is planned again (by our hook, too).

    Input:  none.
    Output: the connection.
    """
    return psycopg.connect(dbname="site_a", host=SOCKET_DIR, autocommit=True,
                           prepare_threshold=None)


START_TIME = time.time()


def progress(message):
    """
    Print one progress line with the time since the start, e.g. "[03:12]".

    Input:  message, the text.
    Output: nothing.
    """
    elapsed = int(time.time() - START_TIME)
    print(f"[{elapsed // 60:02d}:{elapsed % 60:02d}] {message}", flush=True)


# ---------------------------------------------------------------------------
# Checks before measuring
# ---------------------------------------------------------------------------

def check_rewrite_is_active():
    """
    Step 0 of Batch 8: in a NEW site_a session, check that postgres_fdw is
    preloaded and that EXPLAIN VERBOSE of the plain join shows our
    "Semijoin Path" line. Stops the program if not.

    Input:  none.
    Output: nothing (raises SystemExit on failure).
    """
    conn = connect_site_a()
    preload = conn.execute("SHOW session_preload_libraries").fetchone()[0]
    plan_lines = conn.execute("EXPLAIN VERBOSE " + strategies.join_sql(1000, "narrow")).fetchall()
    conn.close()
    plan_text = "\n".join(line[0] for line in plan_lines)
    if "postgres_fdw" not in preload:
        raise SystemExit(f"STOP: session_preload_libraries = '{preload}', expected postgres_fdw")
    if "Semijoin Path" not in plan_text:
        raise SystemExit("STOP: EXPLAIN VERBOSE shows no Semijoin lines:\n" + plan_text)
    progress(f"check: session_preload_libraries = '{preload}', EXPLAIN shows the Semijoin lines")


def has_use_remote_estimate(admin):
    """
    True if the foreign table customers has the option use_remote_estimate.

    Input:  admin, a connection to site_a.
    Output: True or False.
    """
    sql = ("SELECT count(*) FROM pg_foreign_table t, pg_options_to_table(t.ftoptions) "
           "WHERE t.ftrelid = 'public.customers'::regclass "
           "AND option_name = 'use_remote_estimate'")
    return admin.execute(sql).fetchone()[0] > 0


def check_tc_netem(network_mbit):
    """
    For E5 with a real slow network: check that tc netem is active on "lo"
    with the expected rate. "tc qdisc show" only reads, so it needs no sudo.

    Input:  network_mbit, the rate we expect (for example 100).
    Output: the qdisc line (raises SystemExit if it does not match).
    """
    output = subprocess.run(["tc", "qdisc", "show", "dev", "lo"],
                            capture_output=True, text=True, check=True).stdout.strip()
    expected_rate = f"rate {int(network_mbit)}Mbit"
    if "netem" not in output or expected_rate not in output:
        raise SystemExit(f"STOP: expected tc netem with '{expected_rate}' on lo, found: {output}")
    return output


def median_time_ms(fetch_size, sql, runs=5):
    """
    Median wall-clock time of a plain postgres_fdw query (no rewrite) when
    the foreign table customers has the option fetch_size = fetch_size.
    The option is removed again right after (also on an error).

    Input:  fetch_size; sql, the query; runs, timed runs after 1 warm-up.
    Output: the median time in ms.
    """
    admin = connect_site_a()
    admin.execute(f"ALTER FOREIGN TABLE customers OPTIONS (ADD fetch_size '{int(fetch_size)}')")
    try:
        conn = connect_site_a()
        conn.execute("SET postgres_fdw.enable_semijoin = off")
        conn.execute(sql)     # warm-up: opens the connection to site B
        times = [timed_run(conn, sql)[0] for run_number in range(runs)]
        conn.close()
    finally:
        admin.execute("ALTER FOREIGN TABLE customers OPTIONS (DROP fetch_size)")
        admin.close()
    return statistics.median(times)


def measure_fdw_round_trip():
    """
    E5: measure the real time of ONE round trip through postgres_fdw on the
    current network (e.g. with tc netem active), which is what the cost
    model calls per_message_ms (one FETCH round trip).

    Method: the same foreign scan of 4,950 rows, once with fetch_size 100
    and once with fetch_size 10000. The rows, the bytes and site B's work
    are the same; only the number of round trips differs. With the cost
    model's count, messages = 1 + ceil(rows / fetch_size):
        fetch_size 100:   1 + 50 = 51 messages
        fetch_size 10000: 1 + 1  =  2 messages
    so one round trip = (time_100 - time_10000) / 49.

    We also time a tiny remote query (1 row) minus its execution time at
    site B, as a cross-check. It is NOT one round trip: postgres_fdw sends
    START TRANSACTION, DECLARE, FETCH, CLOSE and COMMIT, about 5 round trips.

    Input:  none.
    Output: a dict with per_message_ms and the numbers behind it.
    """
    scan_sql = "SELECT customer_id, name FROM customers WHERE customer_id <= 4950"
    time_100 = median_time_ms(100, scan_sql)
    time_10000 = median_time_ms(10000, scan_sql)
    per_message_ms = (time_100 - time_10000) / (51 - 2)

    tiny_sql = "SELECT customer_id, name FROM customers WHERE customer_id = 1"
    conn = connect_site_a()
    conn.execute("SET postgres_fdw.enable_semijoin = off")
    conn.execute(tiny_sql)
    tiny_ms = statistics.median(timed_run(conn, tiny_sql)[0] for run_number in range(21))
    conn.close()
    site_b = psycopg.connect(dbname="site_b", host=SOCKET_DIR, autocommit=True)
    plan = site_b.execute("EXPLAIN (ANALYZE, FORMAT JSON) " + tiny_sql).fetchone()[0][0]
    site_b.close()
    tiny_minus_b_ms = tiny_ms - plan["Planning Time"] - plan["Execution Time"]

    return {
        "per_message_ms": round(per_message_ms, 5),
        "scan_4950_rows_fetch100_ms": round(time_100, 3),
        "scan_4950_rows_fetch10000_ms": round(time_10000, 3),
        "tiny_query_minus_b_ms": round(tiny_minus_b_ms, 3),
    }


# ---------------------------------------------------------------------------
# Reading our lines out of EXPLAIN and out of the hook's NOTICE
# ---------------------------------------------------------------------------

def parse_predicted_times(text):
    """
    Read "index=6.071 ms filter=159.934 ms" (EXPLAIN's Semijoin Predicted
    Time line).

    Input:  text, the line's value, or None.
    Output: (index_ms, filter_ms), or (None, None).
    """
    if text is None:
        return None, None
    match = re.search(r"index=([\d.]+) ms filter=([\d.]+) ms", text)
    if match is None:
        return None, None
    return float(match.group(1)), float(match.group(2))


def parse_key_values(text):
    """
    Turn "mode=bloom keys=9955 m=207672 k=14 bytes_sent=51968" (EXPLAIN
    ANALYZE's Semijoin Filter line) into a dict of strings.

    Input:  text, the line's value.
    Output: a dict, for example {"mode": "bloom", "keys": "9955", ...}.
    """
    values = {}
    for name, value in re.findall(r"(\w+)=(\w+)", text):
        values[name] = value
    return values


# The hook's NOTICE (semijoin_verbose = on), for example:
#   postgres_fdw semijoin: index path (cost model): n_est=10300.0 (r=10351,
#   N=200000, D_local=168392), s_rows=1000000, rows_per_key=1.000,
#   row_width=33.00 (2 columns), fetch_size=100, max_filter_bytes=1048576
HOOK_NOTICE = re.compile(
    r"postgres_fdw semijoin: (\w+) path \(([^)]*)\): n_est=([\d.]+) .*"
    r"s_rows=([\d.]+), rows_per_key=([\d.]+), row_width=([\d.]+) \((\d+) columns\), "
    r"fetch_size=(\d+), max_filter_bytes=(\d+)")


def parse_hook_notice(messages):
    """
    Find the hook's NOTICE among the messages of a session and read the
    cost model's inputs from it.

    Input:  messages, a list of NOTICE texts.
    Output: a dict (path, n_est, s_rows, rows_per_key, row_width,
            fetch_size, max_filter_bytes), or None if there was none.
    """
    for message in messages:
        match = HOOK_NOTICE.search(message)
        if match:
            return {
                "path": match.group(1),
                "n_est": float(match.group(3)),
                "s_rows": float(match.group(4)),
                "rows_per_key": float(match.group(5)),
                "row_width": float(match.group(6)),
                "fetch_size": int(match.group(8)),
                "max_filter_bytes": int(match.group(9)),
            }
    return None


def plan_details(plan):
    """
    Read everything we record from the warm-up's EXPLAIN ANALYZE plan.

    Input:  plan, the plan dict (from FORMAT JSON).
    Output: a dict with foreign_rows, foreign_scan_ms, join_type, path,
            mode, keys_sent, bloom_m, bloom_k, param_bytes_sent, n_est,
            pred_index_ms, pred_filter_ms.
    """
    scan = common.find_foreign_scan(plan)
    pred_index_ms, pred_filter_ms = parse_predicted_times(scan.get("Semijoin Predicted Time"))
    details = {
        "foreign_rows": common.remote_rows(scan),
        "foreign_scan_ms": round(scan["Actual Total Time"] * scan["Actual Loops"], 3),
        "join_type": common.join_type(plan),
        "path": scan.get("Semijoin Path", "none"),
        "mode": None,
        "keys_sent": None,
        "bloom_m": None,
        "bloom_k": None,
        "param_bytes_sent": None,
        "n_est": scan.get("Semijoin Estimated Keys"),
        "pred_index_ms": pred_index_ms,
        "pred_filter_ms": pred_filter_ms,
    }

    if "Semijoin Filter" in scan:
        # Filter path: "mode=bloom keys=9955 m=207672 k=14 bytes_sent=51968"
        values = parse_key_values(scan["Semijoin Filter"])
        details["mode"] = values.get("mode")
        details["keys_sent"] = values.get("keys")
        details["bloom_m"] = values.get("m")
        details["bloom_k"] = values.get("k")
        details["param_bytes_sent"] = values.get("bytes_sent")
    elif "Semijoin Keys Sent" in scan:
        # Index path: "999 bytes_sent=6887"
        text = scan["Semijoin Keys Sent"]
        details["mode"] = "keylist"
        details["keys_sent"] = text.split()[0]
        details["param_bytes_sent"] = parse_key_values(text).get("bytes_sent")
    return details


# ---------------------------------------------------------------------------
# Estimated bytes (from sjbloom's cost model, never re-computed in Python)
# ---------------------------------------------------------------------------

def cost_model_rows(admin, hook_inputs, config):
    """
    Ask sjb_choice_report for the cost model's estimates, with exactly the
    inputs the planner hook used (read from its NOTICE), and read the
    bytes of every path and mode out of the report.

    A report line looks like:
      path  mode  bits/key bytes_A_to_B rows_back bytes_B_to_A messages ...
      index -     -        7002         1000      31780        11 ...
      filter bloom 22.93   5784         1016      32301        12 ...

    Input:  admin, a connection to site_a; hook_inputs, from
            parse_hook_notice; config, the configuration.
    Output: a dict like {"index": {...}, "bloom": {...}, "exact": {...},
            "pass": {...}}, each with bytes_a_to_b, rows_back, bytes_b_to_a.
    """
    # The calibrated constants, as the hook reads them (the GUC values).
    constants = {}
    for name in ["t_check_ns", "t_lookup_us", "per_row_us", "t_build_us"]:
        constants[name] = float(admin.execute(f"SHOW postgres_fdw.semijoin_{name}").fetchone()[0])
    # the network numbers of the auto run (E5 changes them)
    model_mbit, constants["per_message_ms"] = model_network("auto", config)
    wire_factor = 1.0 if config["binary_filter"] else 2.0

    report = admin.execute(
        "SELECT sjb_choice_report(%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)",
        (hook_inputs["n_est"], hook_inputs["s_rows"], hook_inputs["rows_per_key"],
         hook_inputs["row_width"], model_mbit,
         constants["t_check_ns"], constants["t_lookup_us"], constants["per_message_ms"],
         constants["per_row_us"], constants["t_build_us"], hook_inputs["fetch_size"],
         hook_inputs["max_filter_bytes"], wire_factor)).fetchone()[0]

    rows = {}
    for line in report.splitlines():
        parts = line.split()
        if len(parts) != 13 or parts[0] not in ("index", "filter"):
            continue
        name = "index" if parts[0] == "index" else parts[1]
        try:
            rows[name] = {"bytes_a_to_b": float(parts[3]),
                          "rows_back": float(parts[4]),
                          "bytes_b_to_a": float(parts[5])}
        except ValueError:
            pass    # a line with "-" instead of numbers (e.g. Bloom skipped)
    return rows


def estimated_bytes(row, model_rows):
    """
    Our estimate of the bytes on the wire for one measured strategy.

      original   all of S comes back: the PASS line's bytes B -> A
      index      key list + matching rows (the index line)
      filter     the line of the mode that was really built
      fdw_param  no estimate (our model does not cover it)

    Input:  row, the measured row (strategy, path, mode); model_rows, from
            cost_model_rows (or None).
    Output: the estimate in bytes, or None.
    """
    if model_rows is None:
        return None
    if row["strategy"] == "original":
        line = model_rows.get("pass")
        return round(line["bytes_b_to_a"]) if line else None
    if row["path"] == "index":
        line = model_rows.get("index")
    elif row["path"] == "filter":
        line = model_rows.get(row["mode"])
    else:
        line = None
    if line is None:
        return None
    return round(line["bytes_a_to_b"] + line["bytes_b_to_a"])


# ---------------------------------------------------------------------------
# Measuring one strategy, one configuration
# ---------------------------------------------------------------------------

def model_network(strategy, config):
    """
    What the cost model is told about the network for one strategy run.

    Normally: the configuration's bandwidth and the calibrated loopback
    round trip. Under tc netem (E5) the configuration carries the measured
    round trip in "per_message_ms". auto_loopback ignores both on purpose
    and keeps the loopback numbers (1000 Mbit, calibrated round trip).

    Input:  strategy; config.
    Output: (network_mbit, per_message_ms) for the cost model.
    """
    if strategy == "auto_loopback":
        return DEFAULT_MBIT, CALIBRATED_PER_MESSAGE_MS
    return float(config["network_mbit"]), config.get("per_message_ms", CALIBRATED_PER_MESSAGE_MS)


def strategy_settings(strategy, config):
    """
    The SET commands a strategy's session runs before its queries.

    Input:  strategy, one of STRATEGY_NAMES; config, the configuration.
    Output: a list of SQL strings.
    """
    binary = "on" if config["binary_filter"] else "off"
    model_mbit, model_per_message_ms = model_network(strategy, config)
    settings = [
        f"SET postgres_fdw.semijoin_network_mbit = {model_mbit}",
        f"SET postgres_fdw.semijoin_per_message_ms = {model_per_message_ms}",
        f"SET postgres_fdw.semijoin_binary_filter = {binary}",
    ]
    if strategy in FORCE_PATH:
        settings.append("SET postgres_fdw.enable_semijoin = on")
        settings.append(f"SET postgres_fdw.semijoin_force_path = {FORCE_PATH[strategy]}")
    else:
        settings.append("SET postgres_fdw.enable_semijoin = off")
    return settings


def timed_run(conn, sql):
    """
    Run the query once and measure its wall-clock time and the loopback
    bytes.

    psycopg's cursor.execute() returns only when the server has sent the
    WHOLE result (it is kept in the client library), so the time covers
    planning, execution and sending all rows. We do not turn the rows into
    Python objects, which would only add Python's own time.

    Input:  conn, a connection to site_a; sql, the query.
    Output: (time in ms, bytes received on lo, bytes sent on lo, rows).
    """
    cursor = conn.cursor()
    rx_before, tx_before = read_loopback_bytes()
    start = time.perf_counter()
    cursor.execute(sql)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    rx_after, tx_after = read_loopback_bytes()
    rows = cursor.rowcount
    cursor.close()
    return elapsed_ms, rx_after - rx_before, tx_after - tx_before, rows


def run_strategy(config, strategy):
    """
    Measure one strategy for one configuration (see the top of this file).

    Input:  config, the configuration dict; strategy, its name.
    Output: (row dict, hook_inputs). hook_inputs are the cost model's
            inputs from the hook's NOTICE (None if the hook did not run).
    """
    sql = strategies.join_sql(config["n"], config["width"])
    check_table = CHECK_TABLE_PREFIX + strategy

    conn = connect_site_a()
    for setting in strategy_settings(strategy, config):
        conn.execute(setting)

    # Collect NOTICE messages (the hook prints its inputs when verbose).
    notices = []
    conn.add_notice_handler(lambda diagnostic: notices.append(diagnostic.message_primary or ""))

    # 1 warm-up run, which also gives the plan and stores the result.
    conn.execute(f"DROP TABLE IF EXISTS {check_table}")
    if strategy in FORCE_PATH:
        conn.execute("SET postgres_fdw.semijoin_verbose = on")
    plan = conn.execute(f"EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON) "
                        f"CREATE UNLOGGED TABLE {check_table} AS {sql}").fetchone()[0][0]
    conn.execute("SET postgres_fdw.semijoin_verbose = off")
    hook_inputs = parse_hook_notice(notices)

    # The timed runs.
    times = []
    rx_values = []
    tx_values = []
    row_counts = []
    for run_number in range(TIMED_RUNS):
        elapsed_ms, rx_bytes, tx_bytes, rows = timed_run(conn, sql)
        times.append(elapsed_ms)
        rx_values.append(rx_bytes)
        tx_values.append(tx_bytes)
        row_counts.append(rows)
    conn.close()

    row = dict(config)
    row["strategy"] = strategy
    row["model_network_mbit"], row["model_per_message_ms"] = model_network(strategy, config)
    row["local_time_ms"] = round(statistics.median(times), 3)
    row["time_ms"] = row["local_time_ms"]
    row["time_runs_ms"] = ";".join(f"{value:.1f}" for value in times)
    # On loopback every packet that is sent is also received by the same
    # interface, so rx and tx are the same number; we keep both to show it.
    row["measured_rx"] = int(statistics.median(rx_values))
    row["measured_tx"] = int(statistics.median(tx_values))
    row["measured_bytes"] = row["measured_rx"]
    row["result_rows"] = row_counts[0] if len(set(row_counts)) == 1 else -1
    row.update(plan_details(plan))
    row["run_at"] = time.strftime("%Y-%m-%d %H:%M:%S")

    # Simulated network (E5 without tc): add the time the measured bytes
    # would need on a link of network_mbit Mbit/s (Mbit/s * 125000 =
    # bytes per second).
    if config["network_method"] == "simulated":
        transfer_ms = row["measured_bytes"] / (config["network_mbit"] * 125000.0) * 1000.0
        row["time_ms"] = round(row["local_time_ms"] + transfer_ms, 3)
    return row, hook_inputs


def results_match(admin, strategy):
    """
    The correctness test: compare final_chk_<strategy> with
    final_chk_original. EXCEPT ALL keeps duplicates, so both counts are 0
    only when the two results are the same multiset of rows.

    Input:  admin, a connection to site_a; strategy, its name.
    Output: True or False.
    """
    original = CHECK_TABLE_PREFIX + "original"
    table = CHECK_TABLE_PREFIX + strategy
    missing = admin.execute(f"SELECT count(*) FROM (SELECT * FROM {original} "
                            f"EXCEPT ALL SELECT * FROM {table}) AS d").fetchone()[0]
    extra = admin.execute(f"SELECT count(*) FROM (SELECT * FROM {table} "
                          f"EXCEPT ALL SELECT * FROM {original}) AS d").fetchone()[0]
    return missing == 0 and extra == 0


def strategies_for(config):
    """
    The strategies measured for a configuration: all five, except
    fdw_param when n > FDW_PARAM_MAX_N. "original" comes first, because
    every other result is compared with it.

    Input:  config.
    Output: a list of strategy names.
    """
    names = []
    for name in STRATEGY_NAMES:
        if name == "fdw_param" and config["n"] > FDW_PARAM_MAX_N:
            continue
        names.append(name)
    if config.get("with_auto_loopback"):
        names.append("auto_loopback")
    return names


def run_config(admin, config):
    """
    Measure all strategies of one configuration and check each result
    against the original.

    Input:  admin, a connection to site_a; config.
    Output: a list of row dicts (one per strategy).
    """
    label = config_label(config)
    rows = []
    hook_inputs = None
    for strategy in strategies_for(config):
        if strategy == "fdw_param":
            # postgres_fdw's own parameterized plan needs remote estimates.
            # The option is removed again right after (also on an error).
            admin.execute("ALTER FOREIGN TABLE customers OPTIONS (ADD use_remote_estimate 'true')")
            try:
                row, inputs = run_strategy(config, strategy)
            finally:
                admin.execute("ALTER FOREIGN TABLE customers OPTIONS (DROP use_remote_estimate)")
        else:
            row, inputs = run_strategy(config, strategy)
        if strategy == "auto":
            hook_inputs = inputs

        if strategy == "original":
            row["correct"] = row["result_rows"] >= 0
            original_rows = row["result_rows"]
        else:
            row["correct"] = (results_match(admin, strategy)
                              and row["result_rows"] == original_rows)
            admin.execute(f"DROP TABLE {CHECK_TABLE_PREFIX}{strategy}")
        rows.append(row)

        extra = ""
        if row["path"] != "none":
            extra = f" path={row['path']}/{row['mode']}"
            if row["mode"] == "bloom":
                extra += f" m={row['bloom_m']} k={row['bloom_k']}"
        progress(f"  {label:44} {strategy:9} {row['time_ms']:9.1f} ms  "
                 f"bytes={row['measured_bytes']:>11,}  rows_B={row['foreign_rows']:>9,}  "
                 f"{row['join_type']:11}{extra}  correct={row['correct']}")
    admin.execute(f"DROP TABLE {CHECK_TABLE_PREFIX}original")

    # Our byte estimates, with the inputs the hook used in the auto run.
    model_rows = None
    if hook_inputs is not None:
        model_rows = cost_model_rows(admin, hook_inputs, config)
    for row in rows:
        row["estimated_bytes"] = estimated_bytes(row, model_rows)
    return rows


# ---------------------------------------------------------------------------
# Configurations of each experiment
# ---------------------------------------------------------------------------

def make_config(experiment, n, width="narrow", s_rows=DEFAULT_S_ROWS,
                match_fraction=DEFAULT_MATCH_FRACTION, skew=DEFAULT_SKEW,
                network_mbit=DEFAULT_MBIT, binary_filter=False, network_method="real"):
    """
    One configuration: every factor, with the defaults for the ones the
    experiment does not vary.

    Input:  the factors (see the top of this file).
    Output: a dict.
    """
    return {"experiment": experiment, "network_method": network_method, "n": n,
            "width": width, "s_rows": s_rows, "match_fraction": match_fraction,
            "skew": skew, "network_mbit": network_mbit, "binary_filter": binary_filter}


def config_label(config):
    """
    A short readable name for a configuration, for progress lines.

    Input:  config.
    Output: a string like "E2 n=1000 narrow s_rows=3000000".
    """
    label = f"{config['experiment']} n={config['n']} {config['width']}"
    if config["s_rows"] != DEFAULT_S_ROWS:
        label += f" s_rows={config['s_rows']}"
    if config["match_fraction"] != DEFAULT_MATCH_FRACTION:
        label += f" match={config['match_fraction']}"
    if config["skew"] != DEFAULT_SKEW:
        label += f" {config['skew']}"
    if config["network_mbit"] != DEFAULT_MBIT:
        label += f" {config['network_mbit']:g}Mbit"
    if config["binary_filter"]:
        label += " binary"
    return label


def experiment_configs(name, e5_mbit=None, e5_method=E5_LABEL, e5_per_message_ms=None):
    """
    The configurations of one experiment. The default point of E2-E6 is
    left out: it is the same as an E1 narrow configuration.

    Input:  name, "E1".."E6"; e5_mbit, e5_method and e5_per_message_ms
            (the measured round trip), only for E5.
    Output: a list of configs.
    """
    configs = []
    if name == "E1":
        for width in ["narrow", "wide"]:
            for n in N_GRID:
                configs.append(make_config("E1", n, width=width))
    elif name == "E2":
        for s_rows in S_ROWS_VALUES:
            if s_rows != DEFAULT_S_ROWS:
                for n in N_SMALL:
                    configs.append(make_config("E2", n, s_rows=s_rows))
    elif name == "E3":
        for fraction in MATCH_FRACTIONS:
            if fraction != DEFAULT_MATCH_FRACTION:
                for n in N_SMALL:
                    configs.append(make_config("E3", n, match_fraction=fraction))
    elif name == "E4":
        for n in N_SMALL:
            configs.append(make_config("E4", n, skew="skewed"))
    elif name == "E5":
        for n in N_GRID:
            config = make_config("E5", n, network_mbit=float(e5_mbit), network_method=e5_method)
            if e5_per_message_ms is not None:
                config["per_message_ms"] = e5_per_message_ms
            # at 10 Mbit also run auto with the loopback network numbers
            config["with_auto_loopback"] = float(e5_mbit) <= 10
            configs.append(config)
    elif name == "E6":
        for n in N_BINARY:
            configs.append(make_config("E6", n, binary_filter=True))
    return configs


# ---------------------------------------------------------------------------
# Data, CSV files, final correctness check
# ---------------------------------------------------------------------------

CURRENT_DATA = None   # (s_rows, match_fraction, skew) now in the databases


def ensure_data(s_rows, match_fraction, skew):
    """
    Regenerate the data with setup/make_data.sh, unless it is already
    what we need.

    Input:  s_rows, match_fraction, skew.
    Output: nothing.
    """
    global CURRENT_DATA
    wanted = (s_rows, match_fraction, skew)
    if CURRENT_DATA == wanted:
        return
    progress(f"data: make_data.sh {s_rows} {match_fraction} {skew}")
    subprocess.run(["bash", MAKE_DATA, str(s_rows), str(match_fraction), skew],
                   check=True, stdout=subprocess.DEVNULL)
    CURRENT_DATA = wanted


def csv_path(experiment, e5_mbit=None, e5_method=None):
    """
    The CSV file of an experiment: results/final/e1.csv, ...; E5 gets one
    file per bandwidth, e.g. e5_100.csv (e5_sim_100.csv when simulated).

    Output: the path.
    """
    if experiment == "E5":
        prefix = "e5_sim_" if e5_method == "simulated" else "e5_"
        return os.path.join(RESULTS_DIR, f"{prefix}{int(e5_mbit)}.csv")
    return os.path.join(RESULTS_DIR, experiment.lower() + ".csv")


def write_csv(path, rows):
    """
    Write result rows to a CSV file (replacing it).

    Input:  path; rows, a list of dicts with the CSV_COLUMNS keys.
    Output: nothing.
    """
    os.makedirs(RESULTS_DIR, exist_ok=True)
    with open(path, "w", newline="") as csv_file:
        # extrasaction="ignore": config keys that are not CSV columns (such
        # as with_auto_loopback) are simply not written
        writer = csv.DictWriter(csv_file, fieldnames=CSV_COLUMNS, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def run_experiment(admin, name, e5_mbit=None, e5_method=None, e5_per_message_ms=None):
    """
    Run all configurations of one experiment and write its CSV file
    (after every configuration, so a crash loses little).

    Input:  admin, a connection to site_a; name; e5_mbit, e5_method,
            e5_per_message_ms (only for E5).
    Output: the list of rows.
    """
    configs = experiment_configs(name, e5_mbit, e5_method, e5_per_message_ms)
    path = csv_path(name, e5_mbit, e5_method)
    progress(f"=== {name}: {len(configs)} configurations -> {os.path.relpath(path, PROJECT_DIR)} ===")
    rows = []
    for config in configs:
        ensure_data(config["s_rows"], config["match_fraction"], config["skew"])
        rows.extend(run_config(admin, config))
        write_csv(path, rows)
    return rows


def run_correctness_auto():
    """
    Run queries/correctness_auto.sql and check that all 18 comparisons
    print 0 and 0. psql -A -t prints each result row as "a|b|c|...".

    Input:  none.
    Output: True if all comparisons are 0.
    """
    output = subprocess.run(["psql", "-X", "-A", "-t", "-h", SOCKET_DIR, "-d", "site_a",
                             "-f", CORRECTNESS_SQL],
                            capture_output=True, text=True, check=True).stdout
    comparisons = []
    for line in output.splitlines():
        fields = line.split("|")
        if len(fields) == 7:
            comparisons.append(fields)
            print("    " + line)
    all_zero = all(fields[5] == "0" and fields[6] == "0" for fields in comparisons)
    progress(f"correctness_auto.sql: {len(comparisons)} comparisons, all 0: {all_zero}")
    return all_zero and len(comparisons) == 18


def main():
    global SOCKET_DIR
    parser = argparse.ArgumentParser(description="Batch 8 final evaluation")
    parser.add_argument("--only", help="comma-separated experiments, e.g. E1,E6")
    parser.add_argument("--e5", type=float, metavar="MBIT",
                        help="run E5 at this bandwidth (tc netem must be active)")
    parser.add_argument("--simulated", action="store_true",
                        help="E5 without tc: measured time + measured bytes / bandwidth")
    args = parser.parse_args()

    SOCKET_DIR = unix_socket_dir()
    progress(f"client connects through the Unix socket in {SOCKET_DIR} (not through lo)")
    check_rewrite_is_active()

    admin = connect_site_a()
    if has_use_remote_estimate(admin):
        progress("note: removing a leftover use_remote_estimate option")
        admin.execute("ALTER FOREIGN TABLE customers OPTIONS (DROP use_remote_estimate)")

    os.makedirs(RESULTS_DIR, exist_ok=True)

    if args.e5 is not None:
        method = "simulated" if args.simulated else E5_LABEL
        info = {"network_mbit": args.e5, "method": method}
        if method == E5_LABEL:
            info["qdisc"] = check_tc_netem(args.e5)
            progress("tc: " + info["qdisc"])
        info["noise"] = measure_background_noise()
        progress(f"background noise on lo: median {info['noise']['median_bytes_per_s']:.0f} B/s, "
                 f"max {info['noise']['max_bytes_per_s']:.0f} B/s")
        ensure_data(DEFAULT_S_ROWS, DEFAULT_MATCH_FRACTION, DEFAULT_SKEW)

        # Tell the cost model about this network: its bandwidth, and the
        # round trip measured on it (only with real emulation; the
        # simulated method adds bytes / bandwidth but no extra latency).
        per_message_ms = None
        if method == E5_LABEL:
            info["round_trip"] = measure_fdw_round_trip()
            per_message_ms = info["round_trip"]["per_message_ms"]
            progress(f"measured round trip through postgres_fdw: {info['round_trip']}")
            progress(f"cost model for E5: network_mbit = {args.e5:g}, "
                     f"per_message_ms = {per_message_ms}")
        with open(os.path.join(RESULTS_DIR, f"e5_{int(args.e5)}_network.txt"), "w") as info_file:
            info_file.write(repr(info) + "\n")
        rows = run_experiment(admin, "E5", args.e5, method, per_message_ms)
    else:
        noise = measure_background_noise()
        progress(f"background noise on lo (no query, 10 x 1 s): median "
                 f"{noise['median_bytes_per_s']:.0f} B/s, max {noise['max_bytes_per_s']:.0f} B/s")
        with open(os.path.join(RESULTS_DIR, "noise.txt"), "w") as noise_file:
            noise_file.write(repr(noise) + "\n")

        # Grouped so that the data is regenerated as few times as possible.
        names = ["E1", "E6", "E2", "E3", "E4"]
        if args.only:
            names = [name.strip().upper() for name in args.only.split(",")]
        rows = []
        for name in names:
            rows.extend(run_experiment(admin, name))

        ensure_data(DEFAULT_S_ROWS, DEFAULT_MATCH_FRACTION, DEFAULT_SKEW)
        progress("default data restored; running queries/correctness_auto.sql")
        run_correctness_auto()

    admin.close()
    wrong = [row for row in rows if not row["correct"]]
    progress(f"done: {len(rows)} rows, {len(wrong)} not correct")


if __name__ == "__main__":
    main()
