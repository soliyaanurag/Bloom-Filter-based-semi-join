"""
calibrate.py: measures the two constants of the time-based cost model.

The cost model (sjbloom/costmodel.c) says

    time = bytes moved / bandwidth + work at site B

and the work at site B needs two numbers that depend on the machine:

  t_check_ns   filter path: nanoseconds site B needs to scan ONE row of
               customers and run sjb_contains on it.
  t_lookup_us  index path: microseconds site B needs for ONE key of
               "customer_id = ANY(key list)" (one index lookup + fetching
               the row).

Both are measured directly in site_b with EXPLAIN (ANALYZE, FORMAT JSON),
not through postgres_fdw, so only site B's own work is timed.

Parallel workers: postgres_fdw runs the remote query through a CURSOR, and
PostgreSQL never uses parallel workers for a cursor. So the real numbers
are measured with SET max_parallel_workers_per_gather = 0. For comparison,
everything is measured a second time with the server's default setting
(parallel workers allowed), and both are printed and saved.

Method:
  t_check_ns:  a Bloom filter of 10,000 random existing keys (b = 16),
               then  SELECT customer_id, name FROM customers
                     WHERE sjb_contains(filter, customer_id)
               5 times; median execution time / rows of customers.
               The same is done with an EXACT and a PASS filter of the same
               keys, to see whether one constant is enough for all modes.
               For reference only, the Bloom scan is also timed as
               SELECT count(*) ...: that form CAN use parallel workers,
               while the row-returning form above does not (see
               docs/WALKTHROUGH.md, Batch 5).
  t_lookup_us: for n = 1000, 5000, 10000, 20000 random existing keys,
               SELECT customer_id, name FROM customers
               WHERE customer_id = ANY(keys)
               5 times each (median); then a straight line
               time = a + n * slope is fitted by least squares, and
               t_lookup_us = slope in microseconds.
Before the 5 timed runs, each query runs once untimed, so the table is in
memory (cache warm-up) for every measurement.

Results go to results/calibration.json; the suggested values are the ones
without parallel workers.

Run from the project folder:
    source setup/env.example.sh
    .venv/bin/python bench/calibrate.py
"""

import json
import math
import os
import statistics
import time

import common


RUNS = 5
CHECK_NUM_KEYS = 10000
CHECK_BITS_PER_KEY = 16
LOOKUP_N_VALUES = [1000, 5000, 10000, 20000]
# Seed for PostgreSQL's random(), so every run picks the same keys.
RANDOM_SEED = 0.42

# The filter modes, as stored in byte 1 of a filter (see sjb_filter.h).
MODE_NUMBERS = {"bloom": 1, "exact": 2, "pass": 3}

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results")
CALIBRATION_JSON = os.path.join(RESULTS_DIR, "calibration.json")


def set_parallel(conn, parallel_allowed):
    """
    Turn parallel workers off (like postgres_fdw's cursor) or back to the
    server's default.

    Input:  conn, a connection to site_b; parallel_allowed, True or False.
    Output: the value of max_parallel_workers_per_gather now in effect.
    """
    if parallel_allowed:
        conn.execute("RESET max_parallel_workers_per_gather")
    else:
        conn.execute("SET max_parallel_workers_per_gather = 0")
    return conn.execute("SHOW max_parallel_workers_per_gather").fetchone()[0]


def random_keys(conn, n):
    """
    n random customer_ids that exist in site_b. setseed makes random()
    repeatable, so every run of this script uses the same keys.

    Input:  conn, a connection to site_b; n.
    Output: a Python list of n ints.
    """
    conn.execute("SELECT setseed(%s)", (RANDOM_SEED,))
    rows = conn.execute("SELECT customer_id FROM customers ORDER BY random() LIMIT %s",
                        (n,)).fetchall()
    return [row[0] for row in rows]


def count_customers(conn):
    """
    The exact number of rows of customers in site_b.

    Input:  conn, a connection to site_b.
    Output: the count, as an int.
    """
    return conn.execute("SELECT count(*) FROM customers").fetchone()[0]


def build_filters(conn, keys):
    """
    Build one filter of each mode from the same keys.

      bloom: sjb_build with m = n * 16 bits and k = round(0.693 * 16) = 11
      exact: sjb_build_auto with s_rows = 10^12. False positives would be so
             expensive that Bloom needs more than 32 bits per key, so the
             cost model picks EXACT.
      pass:  sjb_build_auto with s_rows = number of keys: every row of S
             matches, so the cost model picks PASS.

    Input:  conn, a connection to site_b; keys, a list of ints.
    Output: a dict {mode name: filter as Python bytes}. Stops with an error
            if a filter does not have the expected mode.
    """
    m = math.ceil(len(keys) * CHECK_BITS_PER_KEY)
    k = round(0.693 * CHECK_BITS_PER_KEY)
    filters = {
        "bloom": conn.execute("SELECT sjb_build(%s::int4[], %s::int8, %s::int4)",
                              (keys, m, k)).fetchone()[0],
        "exact": conn.execute("SELECT sjb_build_auto(%s::int4[], 1e12, 1, 30, 1000, 0)",
                              (keys,)).fetchone()[0],
        "pass": conn.execute("SELECT sjb_build_auto(%s::int4[], %s, 1, 30, 1000, 0)",
                             (keys, len(keys))).fetchone()[0],
    }
    for mode_name, filter_bytes in filters.items():
        # Byte 0 is the version, byte 1 the mode.
        if filter_bytes[1] != MODE_NUMBERS[mode_name]:
            raise RuntimeError(f"expected a {mode_name} filter, got mode {filter_bytes[1]}")
    return filters


def describe_plan(plan):
    """
    A short text for the plan: the node types from the top down, plus the
    number of parallel workers launched, for example
    "Gather > Seq Scan (2 workers)" or "Index Scan".

    Input:  plan, the dict from EXPLAIN (FORMAT JSON).
    Output: the text.
    """
    node_types = []
    workers = 0
    node = plan["Plan"]
    while node is not None:
        node_types.append(node["Node Type"])
        workers = workers + node.get("Workers Launched", 0)
        children = node.get("Plans", [])
        if children:
            node = children[0]
        else:
            node = None
    text = " > ".join(node_types)
    if workers > 0:
        text = text + f" ({workers} workers)"
    return text


def median_execution_ms(conn, sql, params):
    """
    Run a query under EXPLAIN (ANALYZE, FORMAT JSON): once untimed to warm
    the cache, then RUNS times.

    Input:  conn, a connection to site_b; sql, the query; params, its
            parameters.
    Output: (median "Execution Time" in ms, plan description of the last
            run).
    """
    explain_sql = "EXPLAIN (ANALYZE, FORMAT JSON) " + sql
    conn.execute(explain_sql, params).fetchone()

    times = []
    plan = None
    for run_number in range(RUNS):
        plan = conn.execute(explain_sql, params).fetchone()[0][0]
        times.append(plan["Execution Time"])
    return statistics.median(times), describe_plan(plan)


def fit_line(x_values, y_values):
    """
    Least-squares straight line y = intercept + slope * x, written out by
    hand:
        slope     = (N * sum(x*y) - sum(x) * sum(y)) / (N * sum(x*x) - sum(x)^2)
        intercept = (sum(y) - slope * sum(x)) / N

    Input:  x_values, y_values, two lists of the same length (at least 2,
            with different x values).
    Output: (intercept, slope).
    """
    count = len(x_values)
    sum_x = 0.0
    sum_y = 0.0
    sum_xy = 0.0
    sum_xx = 0.0
    for index in range(count):
        x = x_values[index]
        y = y_values[index]
        sum_x = sum_x + x
        sum_y = sum_y + y
        sum_xy = sum_xy + x * y
        sum_xx = sum_xx + x * x

    slope = (count * sum_xy - sum_x * sum_y) / (count * sum_xx - sum_x * sum_x)
    intercept = (sum_y - slope * sum_x) / count
    return intercept, slope


def measure_check(conn, filters, num_customers):
    """
    t_check_ns for each filter mode: median time of the full scan with
    sjb_contains, divided by the number of rows of customers.

    Input:  conn, a connection to site_b; filters, from build_filters;
            num_customers.
    Output: a dict {mode name: {"median_ms", "t_check_ns", "plan"}}.
    """
    sql = "SELECT customer_id, name FROM customers WHERE sjb_contains(%s::bytea, customer_id)"
    results = {}
    for mode_name, filter_bytes in filters.items():
        median_ms, plan_text = median_execution_ms(conn, sql, (filter_bytes,))
        results[mode_name] = {
            "median_ms": round(median_ms, 3),
            "t_check_ns": round(median_ms * 1e6 / num_customers, 2),
            "plan": plan_text,
        }
    return results


def measure_count_star(conn, filter_bytes):
    """
    For reference only: the same Bloom scan written as SELECT count(*).
    With parallel workers allowed, PostgreSQL runs this form in parallel
    (each worker counts part of the table), while the row-returning form
    in measure_check stays serial. This shows where the speed-up of
    parallel workers would come from.

    Input:  conn, a connection to site_b; filter_bytes, the Bloom filter.
    Output: a dict {"median_ms", "plan"}.
    """
    sql = "SELECT count(*) FROM customers WHERE sjb_contains(%s::bytea, customer_id)"
    median_ms, plan_text = median_execution_ms(conn, sql, (filter_bytes,))
    return {"median_ms": round(median_ms, 3), "plan": plan_text}


def measure_lookup(conn, keys_by_n):
    """
    t_lookup_us: time of "customer_id = ANY(keys)" for each n, and the
    straight line through those points.

    Input:  conn, a connection to site_b; keys_by_n, {n: list of n keys}.
    Output: a dict with the points, the intercept (ms), the slope (ms per
            key) and t_lookup_us (= slope * 1000).
    """
    sql = "SELECT customer_id, name FROM customers WHERE customer_id = ANY(%s::int4[])"
    points = []
    for n in LOOKUP_N_VALUES:
        median_ms, plan_text = median_execution_ms(conn, sql, (keys_by_n[n],))
        points.append({"n": n, "median_ms": round(median_ms, 3), "plan": plan_text})

    intercept_ms, slope_ms = fit_line([point["n"] for point in points],
                                      [point["median_ms"] for point in points])
    return {
        "points": points,
        "intercept_ms": round(intercept_ms, 3),
        "slope_ms_per_key": slope_ms,
        "t_lookup_us": round(slope_ms * 1000, 4),
    }


def measure_all(conn, parallel_allowed, filters, keys_by_n, num_customers):
    """
    One full calibration (check + lookup) with parallel workers off or on.

    Input:  conn; parallel_allowed; filters; keys_by_n; num_customers.
    Output: a dict with the setting used and both measurements.
    """
    setting = set_parallel(conn, parallel_allowed)
    print(f"Measuring with max_parallel_workers_per_gather = {setting} ...")
    return {
        "max_parallel_workers_per_gather": setting,
        "check": measure_check(conn, filters, num_customers),
        "count_star_bloom": measure_count_star(conn, filters["bloom"]),
        "lookup": measure_lookup(conn, keys_by_n),
    }


def print_results(calibration):
    """
    Print both calibrations side by side.

    Input:  calibration, the dict that is also saved as JSON.
    Output: nothing (prints).
    """
    no_parallel = calibration["no_parallel"]
    with_parallel = calibration["with_parallel"]

    print()
    print(f"t_check_ns: full scan of {calibration['customers_rows']:,} rows with sjb_contains "
          f"({CHECK_NUM_KEYS} keys)")
    rows = []
    for mode_name in ["bloom", "exact", "pass"]:
        off = no_parallel["check"][mode_name]
        on = with_parallel["check"][mode_name]
        rows.append({
            "mode": mode_name,
            "ms_no_parallel": off["median_ms"],
            "t_check_ns_no_parallel": off["t_check_ns"],
            "ms_parallel": on["median_ms"],
            "t_check_ns_parallel": on["t_check_ns"],
            "speedup": round(off["median_ms"] / on["median_ms"], 2),
            "plan_parallel": on["plan"],
        })
    common.print_table(rows, ["mode", "ms_no_parallel", "t_check_ns_no_parallel",
                              "ms_parallel", "t_check_ns_parallel", "speedup", "plan_parallel"])
    off = no_parallel["count_star_bloom"]
    on = with_parallel["count_star_bloom"]
    print(f"  reference, SELECT count(*) with the Bloom filter: {off['median_ms']:.2f} ms "
          f"({off['plan']}) vs {on['median_ms']:.2f} ms ({on['plan']}), "
          f"speedup {off['median_ms'] / on['median_ms']:.2f}")

    print()
    print("t_lookup_us: customer_id = ANY(keys)")
    rows = []
    for index, n in enumerate(LOOKUP_N_VALUES):
        off = no_parallel["lookup"]["points"][index]
        on = with_parallel["lookup"]["points"][index]
        rows.append({
            "n": n,
            "ms_no_parallel": off["median_ms"],
            "plan_no_parallel": off["plan"],
            "ms_parallel": on["median_ms"],
            "plan_parallel": on["plan"],
        })
    common.print_table(rows, ["n", "ms_no_parallel", "plan_no_parallel",
                              "ms_parallel", "plan_parallel"])
    for label, result in [("no parallel", no_parallel), ("parallel", with_parallel)]:
        lookup = result["lookup"]
        print(f"  fitted line ({label}): time_ms = {lookup['intercept_ms']:.3f} + n * "
              f"{lookup['slope_ms_per_key']:.6f}  ->  t_lookup_us = {lookup['t_lookup_us']:.4f}")

    print()
    print("Suggested values (no parallel workers, as postgres_fdw's cursor runs):")
    print(f"  t_check_ns  = {calibration['suggested']['t_check_ns']}")
    print(f"  t_lookup_us = {calibration['suggested']['t_lookup_us']}")


def main():
    conn = common.connect("site_b")
    num_customers = count_customers(conn)

    # The same keys for both calibrations.
    check_keys = random_keys(conn, CHECK_NUM_KEYS)
    filters = build_filters(conn, check_keys)
    keys_by_n = {}
    for n in LOOKUP_N_VALUES:
        keys_by_n[n] = random_keys(conn, n)

    no_parallel = measure_all(conn, False, filters, keys_by_n, num_customers)
    with_parallel = measure_all(conn, True, filters, keys_by_n, num_customers)
    conn.close()

    calibration = {
        "measured_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "customers_rows": num_customers,
        "runs_per_measurement": RUNS,
        "check_filter": {"num_keys": CHECK_NUM_KEYS, "bits_per_key": CHECK_BITS_PER_KEY},
        "no_parallel": no_parallel,
        "with_parallel": with_parallel,
        "suggested": {
            "t_check_ns": no_parallel["check"]["bloom"]["t_check_ns"],
            "t_lookup_us": no_parallel["lookup"]["t_lookup_us"],
        },
    }

    os.makedirs(RESULTS_DIR, exist_ok=True)
    with open(CALIBRATION_JSON, "w") as json_file:
        json.dump(calibration, json_file, indent=2)

    print_results(calibration)
    print()
    print(f"Saved {os.path.normpath(CALIBRATION_JSON)}")


if __name__ == "__main__":
    main()
