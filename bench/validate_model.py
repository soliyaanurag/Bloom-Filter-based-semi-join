"""
validate_model.py: does the time-based cost model predict real times?

For narrow rows and n in N_VALUES, it compares, for two strategies of the
benchmark (bench/strategies.py):

  model line         measured strategy
  -----------------  ------------------------------------------------
  index path         exact_keys with SET enable_nestloop = off
  filter path BLOOM  bloom_dynamic (b* from sjb_best_bits)

  predicted_ms  the "total_ms" of that line of sjb_choice_report, with the
                calibrated constants (results/calibration.json) and
                network_mbit = 1000
  measured_ms   the end-to-end time of the query at site A (planning +
                execution, median of 3 runs, as in run_experiments.py)
  ratio         measured_ms / predicted_ms

To help explain the ratio, it also prints:
  fscan_ms      the time EXPLAIN ANALYZE reports for the Foreign Scan node
                at site A: waiting for site B, receiving the rows and
                turning them from text back into values. It does NOT
                include scanning orders, building the filter, or the join.
  pred_rows / remote_rows   rows the model expects back / rows that came

The model only has network time + work at site B. It has no work at
site A and no cost per returned row. If the ratio is far from 1, those
missing parts are the first suspects; the numbers here are NOT tuned to
make it fit.

Needs results/calibration.json (run bench/calibrate.py first).

Run from the project folder:
    source setup/env.example.sh
    .venv/bin/python bench/validate_model.py
"""

import json
import os

import common
import run_experiments
import strategies


N_VALUES = [100, 1000, 10000, 50000]
WIDTH = "narrow"
NETWORK_MBIT = 1000
# customer_id is the primary key of customers: one row per key value.
ROWS_PER_KEY = 1.0
# Columns in the Remote SQL for the narrow width: customer_id, name.
NUM_REMOTE_COLUMNS = 2

RESULTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "results")
CALIBRATION_JSON = os.path.join(RESULTS_DIR, "calibration.json")


def load_calibration():
    """
    Read the suggested constants from results/calibration.json.

    Input:  none.
    Output: (t_check_ns, t_lookup_us).
    """
    with open(CALIBRATION_JSON) as json_file:
        calibration = json.load(json_file)
    return calibration["suggested"]["t_check_ns"], calibration["suggested"]["t_lookup_us"]


def wire_row_width(text_width):
    """
    The width of one returned row ON THE WIRE: the text of the values plus
    PostgreSQL's DataRow message overhead, 7 bytes per row + 4 bytes per
    column (see CLAUDE.md).

    Input:  text_width, the average text width of the row's values.
    Output: the wire width in bytes.
    """
    return text_width + 7 + 4 * NUM_REMOTE_COLUMNS


def parse_report(report):
    """
    Read the table lines of sjb_choice_report into a dict.

    A table line looks like
      "filter  bloom     18.14  45390  10163  325223  2.965  150.000  152.965"
    i.e. path, mode, bits/key, bytes_A_to_B, rows_back, bytes_B_to_A,
    network_ms, work_at_B_ms, total_ms. The index line has mode "-".
    Skipped modes ("filter  bloom  skipped: ...") are left out.

    Input:  report, the text returned by sjb_choice_report.
    Output: {"index": {...}, "bloom": {...}, ...}, each with the keys
            rows_back, network_ms, work_at_b_ms, total_ms.
    """
    lines = {}
    for line in report.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[0] not in ("index", "filter"):
            continue
        if parts[2] == "skipped:":
            continue
        if parts[0] == "index":
            name = "index"
        else:
            name = parts[1]
        lines[name] = {
            "rows_back": float(parts[4]),
            "network_ms": float(parts[6]),
            "work_at_b_ms": float(parts[7]),
            "total_ms": float(parts[8]),
        }
    return lines


def choice_report(conn, n_keys, s_rows, row_width, t_check_ns, t_lookup_us):
    """
    Call sjb_choice_report in site_a.

    Input:  conn, a connection to site_a; the model inputs.
    Output: the report text.
    """
    sql = "SELECT sjb_choice_report(%s, %s, %s, %s, %s, %s, %s)"
    params = (n_keys, s_rows, ROWS_PER_KEY, row_width, NETWORK_MBIT, t_check_ns, t_lookup_us)
    return conn.execute(sql, params).fetchone()[0]


def measure(conn, sql_text, nestloop_off):
    """
    Measure one query like run_experiments.py does (median of 3 runs).

    Input:  conn, a connection to site_a; sql_text; nestloop_off, True to
            run with SET enable_nestloop = off.
    Output: a dict with measured_ms, fscan_ms, remote_rows, join_type.
    """
    if nestloop_off:
        conn.execute("SET enable_nestloop = off")
    plan = run_experiments.run_median(conn, sql_text)
    if nestloop_off:
        conn.execute("RESET enable_nestloop")

    foreign_scan = common.find_foreign_scan(plan)
    return {
        "measured_ms": common.total_time_ms(plan),
        "fscan_ms": foreign_scan["Actual Total Time"] * foreign_scan["Actual Loops"],
        "remote_rows": common.remote_rows(foreign_scan),
        "join_type": common.join_type(plan),
    }


def main():
    t_check_ns, t_lookup_us = load_calibration()
    conn = common.connect("site_a")
    run_experiments.warm_up(conn)

    text_width = common.avg_row_width(WIDTH)
    row_width = wire_row_width(text_width)
    s_rows = strategies.foreign_table_rows(conn)
    print(f"t_check_ns = {t_check_ns}, t_lookup_us = {t_lookup_us}, network_mbit = {NETWORK_MBIT}")
    print(f"s_rows = {s_rows:,.0f}, row width: text {text_width:.2f} + 7 + 4*{NUM_REMOTE_COLUMNS} "
          f"= {row_width:.2f} bytes on the wire")
    print()

    rows = []
    for n in N_VALUES:
        n_keys = strategies.distinct_keys(conn, n)
        report = choice_report(conn, n_keys, s_rows, row_width, t_check_ns, t_lookup_us)
        predicted = parse_report(report)

        # index path <-> exact_keys with nested loops off
        exact_sql, unused_bytes_sql = strategies.exact_keys(n, WIDTH)
        cases = [("index", "exact_keys (nestloop off)", exact_sql, True)]

        # filter path BLOOM <-> bloom_dynamic, with b* for the wire width
        if "bloom" in predicted:
            b = strategies.dynamic_bits(conn, n, row_width)
            bloom_sql, unused_bytes_sql = strategies.bloom_dynamic(n, b, WIDTH)
            cases.append(("bloom", f"bloom_dynamic (b={b})", bloom_sql, False))

        for model_line, strategy_name, sql_text, nestloop_off in cases:
            result = measure(conn, sql_text, nestloop_off)
            model = predicted[model_line]
            row = {
                "n": n,
                "n_keys": n_keys,
                "strategy": strategy_name,
                "pred_network_ms": model["network_ms"],
                "pred_work_b_ms": model["work_at_b_ms"],
                "predicted_ms": model["total_ms"],
                "measured_ms": round(result["measured_ms"], 2),
                "ratio": round(result["measured_ms"] / model["total_ms"], 2),
                "fscan_ms": round(result["fscan_ms"], 2),
                "fscan_ratio": round(result["fscan_ms"] / model["total_ms"], 2),
                "pred_rows": round(model["rows_back"]),
                "remote_rows": result["remote_rows"],
                "join_type": result["join_type"],
            }
            rows.append(row)
            print(f"  n={n:<6} {strategy_name:28} predicted={row['predicted_ms']:9.2f} ms  "
                  f"measured={row['measured_ms']:9.2f} ms  ratio={row['ratio']}")

    conn.close()
    print()
    common.print_table(rows, ["n", "n_keys", "strategy", "pred_network_ms", "pred_work_b_ms",
                              "predicted_ms", "measured_ms", "ratio", "fscan_ms", "fscan_ratio",
                              "pred_rows", "remote_rows", "join_type"])


if __name__ == "__main__":
    main()
