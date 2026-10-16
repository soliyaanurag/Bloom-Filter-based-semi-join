"""
break_even.py: from how many keys on is the filter path faster than the
index path? The cost model's answer versus the measured answer.

  index path   exact_keys with SET enable_nestloop = off
               (B gets "customer_id = ANY(key list)", n index lookups)
  filter path  auto_filter: sjb_contains(sjb_build_auto(...)), the filter
               path as the system really runs it (B checks all rows)

PREDICTED break-even: the smallest n (distinct keys) for which
sjb_plan_choice says 'filter', searched in steps of 500 keys, with the
calibrated constants (results/calibration.json).

MEASURED break-even: both paths are run for several numbers of orders
(N_VALUES), and the times are compared. Two measured times are used:
  covered_ms     the Foreign Scan node's time (what the model covers;
                 it includes the InitPlan that builds the key list or
                 filter, see validate_model.py)
  end_to_end_ms  planning + execution of the whole query
Where the difference (index - filter) changes from negative to positive,
the break-even is found by a straight line between the two points.

Networks:
  1000 Mbit/s  site A and site B talk over localhost, which is at least
               this fast, so the measured times are used as they are.
  1 Mbit/s     cannot be measured here (slowing down localhost needs
               sudo). Its "measured" time is SIMULATED: the measured
               time + the measured bytes (A->B text + rows back * wire
               row width) / 125,000 bytes per second.

Run from the project folder:
    source setup/env.example.sh
    .venv/bin/python bench/break_even.py
"""

import common
import run_experiments
import strategies
import validate_model


# Numbers of orders (order_id <= n); the distinct keys are a bit fewer.
N_VALUES = [10000, 25000, 50000, 75000, 100000, 150000, 200000]
NETWORKS_MBIT = [1000, 1]
WIDTH = "narrow"
MODE_NAMES = {1: "bloom", 2: "exact", 3: "pass"}


def predicted_break_even(conn, s_rows, row_width, network_mbit, constants):
    """
    The smallest n (in steps of 500, up to 1,000,000) for which the cost
    model picks the filter path, and how many n above it go back to the
    index path (0 means the choice switches only once).

    Input:  conn, a connection to site_a; s_rows; row_width; network_mbit;
            constants, from validate_model.load_calibration().
    Output: (first n where 'filter' wins or None, number of n above it
            where 'index' wins).
    """
    sql = """
        WITH choices AS (
            SELECT n, sjb_plan_choice(n, %s, 1, %s, %s, %s, %s, %s, %s, %s, %s) AS choice
            FROM generate_series(500, 1000000, 500) AS n
        ),
        first_filter AS (
            SELECT min(n) AS n FROM choices WHERE choice = 'filter'
        )
        SELECT f.n,
               (SELECT count(*) FROM choices c WHERE c.n > f.n AND c.choice = 'index')
        FROM first_filter f
    """
    params = (s_rows, row_width, network_mbit, constants["t_check_ns"],
              constants["t_lookup_us"], constants["per_message_ms"],
              constants["per_row_us"], constants["t_build_us"], constants["fetch_size"])
    return conn.execute(sql, params).fetchone()


def measure_path(conn, sql_text, bytes_sql, nestloop_off, row_width):
    """
    Measure one path (median of 3 runs, like run_experiments.py).

    Input:  conn; sql_text; bytes_sql (bytes A->B as text); nestloop_off;
            row_width, the wire width of one returned row.
    Output: a dict with covered_ms, end_to_end_ms and total_bytes.
    """
    result = validate_model.measure(conn, sql_text, nestloop_off)
    bytes_a_to_b = conn.execute(bytes_sql).fetchone()[0]
    return {
        "covered_ms": result["covered_ms"],
        "end_to_end_ms": result["end_to_end_ms"],
        "total_bytes": bytes_a_to_b + result["remote_rows"] * row_width,
    }


def time_at_network(measured, which_time, network_mbit):
    """
    The measured time at a given network speed: as measured for
    1000 Mbit/s (localhost), plus the bytes / bandwidth for slower ones.

    Input:  measured, from measure_path; which_time, "covered_ms" or
            "end_to_end_ms"; network_mbit.
    Output: the time in ms.
    """
    if network_mbit >= 1000:
        return measured[which_time]
    bytes_per_ms = network_mbit * 125000 / 1000
    return measured[which_time] + measured["total_bytes"] / bytes_per_ms


def crossing(points, index_key, filter_key):
    """
    Where the filter path becomes faster than the index path.

    Input:  points, a list of dicts sorted by n_keys; index_key and
            filter_key, the names of the two times in each dict.
    Output: the interpolated n_keys where (index - filter) goes from
            <= 0 to > 0, or None if it never does in these points (or
            the filter path already wins at the first point: then the
            text "< first n" is returned).
    """
    previous = None
    for point in points:
        difference = point[index_key] - point[filter_key]
        if difference > 0:
            if previous is None:
                return f"< {point['n_keys']:,}"
            previous_difference = previous[index_key] - previous[filter_key]
            # straight line between the two points, where it crosses 0
            fraction = -previous_difference / (difference - previous_difference)
            n_keys = previous["n_keys"] + fraction * (point["n_keys"] - previous["n_keys"])
            return f"{n_keys:,.0f}"
        previous = point
    return None


def main():
    constants = validate_model.load_calibration()
    conn = common.connect("site_a")
    run_experiments.warm_up(conn)
    row_width = validate_model.wire_row_width(common.avg_row_width(WIDTH))
    s_rows = strategies.foreign_table_rows(conn)

    points = []
    for n in N_VALUES:
        n_keys = strategies.distinct_keys(conn, n)
        exact_sql, exact_bytes_sql = strategies.exact_keys(n, WIDTH)
        index_result = measure_path(conn, exact_sql, exact_bytes_sql, True, row_width)
        point = {"n": n, "n_keys": n_keys}
        for network_mbit in NETWORKS_MBIT:
            filter_sql_text, filter_bytes_sql, filter_sql = strategies.auto_filter(
                n, WIDTH, s_rows, 1, row_width, network_mbit, constants["t_check_ns"])
            mode = conn.execute(f"SELECT get_byte({filter_sql}, 1)").fetchone()[0]
            filter_result = measure_path(conn, filter_sql_text, filter_bytes_sql, False, row_width)
            report = validate_model.choice_report(conn, n_keys, s_rows, row_width,
                                                  network_mbit, constants)
            predicted = validate_model.parse_report(report)
            best_filter = MODE_NAMES[mode]
            suffix = f"_{network_mbit}"
            point["mode" + suffix] = best_filter
            point["pred_index" + suffix] = predicted["index"]["total_ms"]
            point["pred_filter" + suffix] = predicted[best_filter]["total_ms"]
            for which_time in ["covered_ms", "end_to_end_ms"]:
                short = which_time.split("_")[0]
                point[f"index_{short}{suffix}"] = round(
                    time_at_network(index_result, which_time, network_mbit), 2)
                point[f"filter_{short}{suffix}"] = round(
                    time_at_network(filter_result, which_time, network_mbit), 2)
        points.append(point)
        print(f"  n={n:<7} keys={n_keys:<7} mode(1000)={point['mode_1000']:5} "
              f"index={point['index_covered_1000']:8.1f} ms  "
              f"filter={point['filter_covered_1000']:8.1f} ms (covered, 1000 Mbit/s)")

    for network_mbit in NETWORKS_MBIT:
        suffix = f"_{network_mbit}"
        label = "localhost" if network_mbit >= 1000 else "simulated"
        print()
        print(f"=== {network_mbit} Mbit/s ({label}) ===")
        common.print_table(points, ["n", "n_keys", "mode" + suffix,
                                    "pred_index" + suffix, "pred_filter" + suffix,
                                    "index_covered" + suffix, "filter_covered" + suffix,
                                    "index_end" + suffix, "filter_end" + suffix])
        first_filter_n, flips_back = predicted_break_even(conn, s_rows, row_width,
                                                          network_mbit, constants)
        print(f"  predicted break-even (sjb_plan_choice): n = {first_filter_n} keys "
              f"(index wins again above it at {flips_back} of the tested n)")
        print(f"  predicted, from these points:           n = "
              f"{crossing(points, 'pred_index' + suffix, 'pred_filter' + suffix)}")
        print(f"  measured (covered: Foreign Scan):       n = "
              f"{crossing(points, 'index_covered' + suffix, 'filter_covered' + suffix)}")
        print(f"  measured (end to end):                  n = "
              f"{crossing(points, 'index_end' + suffix, 'filter_end' + suffix)}")
    conn.close()


if __name__ == "__main__":
    main()
