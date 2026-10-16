"""
final_report.py: Batch 8, the analysis of the final evaluation.

It reads the CSV files written by final_eval.py (results/final/e1.csv,
e2.csv, ..., e5_*.csv) and writes, also into results/final/:
  decision_quality.csv / .md  one row per configuration: the times of
                              original, index, filter and auto, the best of
                              them, regret, correct_choice and speedup
  headline.md                 the headline numbers
  *.png                       the charts

Decision quality, per configuration:
  best            = the fastest of original, index and filter (measured)
  regret          = auto_time / best_time   (1.00 = a perfect choice)
  correct_choice  = auto picked the strategy that was actually fastest
  speedup         = original_time / auto_time

The default configuration of E2-E6 (s_rows 1M, match 1.0, uniform,
1000 Mbit, binary off) is the same as an E1 narrow run, so it is measured
once in E1 and only REUSED in the E2-E6 charts. In the decision-quality
table every configuration appears once.

Run from the project folder (after final_eval.py):
    .venv/bin/python bench/final_report.py
"""

import csv
import glob
import math
import os
import statistics

import matplotlib
matplotlib.use("Agg")   # draw into files, no window
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

import common


PROJECT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
RESULTS_DIR = os.path.join(PROJECT_DIR, "results", "final")

# Fixed color and marker per strategy, so a strategy looks the same in every
# chart (colors from the dataviz reference palette; the marker is a second
# cue for readers who cannot tell the colors apart).
STYLE = {
    "original":  {"color": "#2a78d6", "marker": "o", "label": "original (plain postgres_fdw)"},
    "auto":      {"color": "#eb6834", "marker": "s", "label": "auto (our system)"},
    "index":     {"color": "#1baf7a", "marker": "^", "label": "index path (forced)"},
    "filter":    {"color": "#eda100", "marker": "D", "label": "filter path (forced)"},
    "fdw_param": {"color": "#e87ba4", "marker": "v", "label": "fdw_param (use_remote_estimate)"},
    "auto_loopback": {"color": "#e34948", "marker": "X",
                      "label": "auto, model NOT told about the network"},
}
PLOT_ORDER = ["original", "fdw_param", "index", "filter", "auto", "auto_loopback"]

NUMBER_COLUMNS = ["n", "s_rows", "match_fraction", "network_mbit", "time_ms", "local_time_ms",
                  "foreign_scan_ms", "measured_bytes", "measured_rx", "measured_tx",
                  "estimated_bytes", "result_rows", "foreign_rows", "keys_sent", "bloom_m",
                  "bloom_k", "param_bytes_sent", "n_est", "pred_index_ms", "pred_filter_ms",
                  "model_network_mbit", "model_per_message_ms"]

# The loopback calibration's round trip: what the cost model used in runs
# whose CSV has no model_per_message_ms column (those runs never changed it).
CALIBRATED_PER_MESSAGE_MS = 0.06194

DEFAULTS = {"s_rows": 1000000, "match_fraction": 1.0, "skew": "uniform",
            "network_mbit": 1000.0, "binary_filter": False, "width": "narrow"}


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def to_number(text):
    """
    Turn a CSV text into a float (or None for an empty field).

    Input:  text.
    Output: float or None.
    """
    if text is None or text == "":
        return None
    return float(text)


def load_rows():
    """
    Read every results/final/e*.csv into one list of dicts, with numbers as
    floats and True/False as booleans.

    Input:  none.
    Output: the list of rows.
    """
    rows = []
    for path in sorted(glob.glob(os.path.join(RESULTS_DIR, "e*.csv"))):
        with open(path, newline="") as csv_file:
            for row in csv.DictReader(csv_file):
                # older CSVs (before E5 was added) lack the model columns
                if not row.get("model_network_mbit"):
                    row["model_network_mbit"] = row["network_mbit"]
                    row["model_per_message_ms"] = str(CALIBRATED_PER_MESSAGE_MS)
                for column in NUMBER_COLUMNS:
                    row[column] = to_number(row[column])
                row["n"] = int(row["n"])
                row["binary_filter"] = row["binary_filter"] == "True"
                row["correct"] = row["correct"] == "True"
                rows.append(row)
    return rows


def config_key(row):
    """
    The configuration a row belongs to (everything except the strategy).

    Input:  row.
    Output: a tuple.
    """
    return (row["experiment"], row["network_method"], row["n"], row["width"], row["s_rows"],
            row["match_fraction"], row["skew"], row["network_mbit"], row["binary_filter"])


def group_by_config(rows):
    """
    Group rows into configurations: {config_key: {strategy: row}}, in the
    order the configurations first appear.

    Input:  rows.
    Output: the dict.
    """
    configs = {}
    for row in rows:
        configs.setdefault(config_key(row), {})[row["strategy"]] = row
    return configs


def config_label(row):
    """
    A short name for a configuration, for tables and bar charts, e.g.
    "E2 n=1000 s=3M".

    Input:  any row of the configuration.
    Output: a string.
    """
    label = f"{row['experiment']} n={row['n']}"
    if row["experiment"] == "E1":
        label += " " + row["width"]
    if row["s_rows"] != DEFAULTS["s_rows"]:
        label += f" s={row['s_rows'] / 1e6:g}M"
    if row["match_fraction"] != DEFAULTS["match_fraction"]:
        label += f" match={row['match_fraction']:g}"
    if row["skew"] != DEFAULTS["skew"]:
        label += " skewed"
    if row["network_mbit"] != DEFAULTS["network_mbit"]:
        label += f" {row['network_mbit']:g}Mbit"
        if row["network_method"] == "simulated":
            label += " (sim)"
    if row["binary_filter"]:
        label += " binary"
    return label


# ---------------------------------------------------------------------------
# Decision quality
# ---------------------------------------------------------------------------

def decision_quality(configs):
    """
    Compute best, regret, correct_choice and speedup for each configuration.

    Input:  configs, from group_by_config.
    Output: a list of dicts, one per configuration.
    """
    table = []
    for key, by_strategy in configs.items():
        auto = by_strategy["auto"]
        candidates = ["original", "index", "filter"]
        best = min(candidates, key=lambda name: by_strategy[name]["time_ms"])
        best_time = by_strategy[best]["time_ms"]
        auto_choice = auto["path"]      # "index" or "filter" ("none" = no rewrite)
        if auto_choice == "none":
            auto_choice = "original"
        auto_text = auto["path"]
        if auto["path"] == "filter":
            auto_text += "/" + auto["mode"]
        table.append({
            "config": config_label(auto),
            "experiment": auto["experiment"],
            "n": auto["n"],
            "original_ms": by_strategy["original"]["time_ms"],
            "index_ms": by_strategy["index"]["time_ms"],
            "filter_ms": by_strategy["filter"]["time_ms"],
            "auto_ms": auto["time_ms"],
            "auto_choice": auto_text,
            "pred_index_ms": auto["pred_index_ms"],
            "pred_filter_ms": auto["pred_filter_ms"],
            "best": best,
            "regret": auto["time_ms"] / best_time,
            # regret of the path itself: the forced run of the chosen path
            # against the best (removes the run-to-run noise between the
            # auto run and the forced run of the same plan)
            "path_regret": by_strategy[auto_choice]["time_ms"] / best_time,
            "correct_choice": auto_choice == best,
            "speedup": by_strategy["original"]["time_ms"] / auto["time_ms"],
            "original_bytes": by_strategy["original"]["measured_bytes"],
            "auto_bytes": auto["measured_bytes"],
            "all_correct": all(row["correct"] for row in by_strategy.values()),
        })
        # E5 at 10 Mbit: the same auto, but with the loopback network numbers
        loopback = by_strategy.get("auto_loopback")
        if loopback is not None:
            table[-1]["loopback_choice"] = loopback["path"]
            table[-1]["loopback_ms"] = loopback["time_ms"]
            table[-1]["loopback_regret"] = loopback["time_ms"] / best_time
    return table


def geometric_mean(values):
    """
    The geometric mean (the right average for ratios such as speedups).

    Input:  a list of positive numbers.
    Output: the geometric mean.
    """
    return math.exp(sum(math.log(value) for value in values) / len(values))


def headline(table, rows):
    """
    The headline numbers of the whole evaluation.

    Input:  table, from decision_quality; rows, all measured rows.
    Output: a dict.
    """
    regrets = [entry["regret"] for entry in table]
    worst = max(table, key=lambda entry: entry["regret"])
    speedups = [entry["speedup"] for entry in table]
    slowest = min(table, key=lambda entry: entry["speedup"])
    bytes_original = sum(entry["original_bytes"] for entry in table)
    bytes_auto = sum(entry["auto_bytes"] for entry in table)
    return {
        "configurations": len(table),
        "measured_rows": len(rows),
        "rows_correct": sum(1 for row in rows if row["correct"]),
        "correct_choices": sum(1 for entry in table if entry["correct_choice"]),
        "percent_correct": 100.0 * sum(1 for e in table if e["correct_choice"]) / len(table),
        "mean_regret": statistics.mean(regrets),
        "median_regret": statistics.median(regrets),
        "mean_path_regret": statistics.mean(entry["path_regret"] for entry in table),
        "worst_regret": worst["regret"],
        "worst_regret_config": worst["config"],
        "speedup_geomean": geometric_mean(speedups),
        "speedup_median": statistics.median(speedups),
        "speedup_min": slowest["speedup"],
        "speedup_min_config": slowest["config"],
        "speedup_max": max(speedups),
        "bytes_original": bytes_original,
        "bytes_auto": bytes_auto,
        "bytes_saved_percent": 100.0 * (1 - bytes_auto / bytes_original),
    }


def write_decision_quality(table):
    """
    Write decision_quality.csv and decision_quality.md.

    Input:  table, from decision_quality.
    Output: nothing.
    """
    # every column that appears in any row (only E5 rows have the loopback ones)
    columns = []
    for entry in table:
        for column in entry:
            if column not in columns:
                columns.append(column)
    with open(os.path.join(RESULTS_DIR, "decision_quality.csv"), "w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=columns, restval="")
        writer.writeheader()
        for entry in table:
            writer.writerow(entry)

    lines = ["| configuration | original ms | index ms | filter ms | auto ms | auto chose | "
             "fastest | regret | correct choice | speedup |",
             "|---|---:|---:|---:|---:|---|---|---:|---|---:|"]
    for entry in table:
        lines.append(f"| {entry['config']} | {entry['original_ms']:,.1f} | {entry['index_ms']:,.1f} "
                     f"| {entry['filter_ms']:,.1f} | {entry['auto_ms']:,.1f} | {entry['auto_choice']} "
                     f"| {entry['best']} | {entry['regret']:.2f} "
                     f"| {'yes' if entry['correct_choice'] else '**no**'} | {entry['speedup']:.1f}x |")
    loopback_entries = [entry for entry in table if "loopback_ms" in entry]
    if loopback_entries:
        lines.append("")
        lines.append("E5 at 10 Mbit: auto with the network numbers measured under netem, and "
                     "auto_loopback with the calibrated loopback numbers (1000 Mbit, 0.062 ms):")
        lines.append("")
        lines.append("| configuration | fastest | auto chose | auto ms | regret "
                     "| auto_loopback chose | auto_loopback ms | regret |")
        lines.append("|---|---|---|---:|---:|---|---:|---:|")
        for entry in loopback_entries:
            lines.append(f"| {entry['config']} | {entry['best']} | {entry['auto_choice']} "
                         f"| {entry['auto_ms']:,.1f} | {entry['regret']:.2f} "
                         f"| {entry['loopback_choice']} | {entry['loopback_ms']:,.1f} "
                         f"| {entry['loopback_regret']:.2f} |")
    with open(os.path.join(RESULTS_DIR, "decision_quality.md"), "w") as md_file:
        md_file.write("\n".join(lines) + "\n")


def write_headline(numbers):
    """
    Write headline.md with the headline numbers.

    Input:  numbers, from headline().
    Output: nothing.
    """
    text = f"""# Headline numbers (generated by bench/final_report.py)

- Configurations: {numbers['configurations']}; measured rows (strategy runs): {numbers['measured_rows']}; correct: {numbers['rows_correct']} of {numbers['measured_rows']}
- Correct choices: {numbers['correct_choices']} of {numbers['configurations']} ({numbers['percent_correct']:.0f}%)
- Regret (auto / fastest): mean {numbers['mean_regret']:.3f}, median {numbers['median_regret']:.3f}, worst {numbers['worst_regret']:.2f} ({numbers['worst_regret_config']})
- Regret of the chosen path's forced run (no auto-vs-forced noise): mean {numbers['mean_path_regret']:.3f}
- Speedup of auto over original: geometric mean {numbers['speedup_geomean']:.1f}x, median {numbers['speedup_median']:.1f}x, range {numbers['speedup_min']:.2f}x ({numbers['speedup_min_config']}) to {numbers['speedup_max']:.1f}x
- Network bytes, summed over all configurations: original {numbers['bytes_original'] / 1e6:,.1f} MB, auto {numbers['bytes_auto'] / 1e6:,.1f} MB ({numbers['bytes_saved_percent']:.1f}% saved)
"""
    with open(os.path.join(RESULTS_DIR, "headline.md"), "w") as md_file:
        md_file.write(text)


# ---------------------------------------------------------------------------
# Batch 8b: policy comparison (no new measurements)
# ---------------------------------------------------------------------------

# A policy is a rule that picks a strategy for every query. We compare our
# cost model with three simple rules, using only the FORCED-path times, so
# the session noise between the auto run and the forced run of the same
# plan does not count.
POLICIES = ["original", "always_index", "always_filter", "auto"]
POLICY_LABELS = {
    "original": "original\n(plain postgres_fdw)",
    "always_index": "always index",
    "always_filter": "always filter",
    "auto": "auto\n(our cost model)",
}
WITHIN_BEST = 1.05      # "within 5% of the best"


def policy_time(entry, policy):
    """
    The time a policy gets in one configuration (forced-path times).

    Input:  entry, a row of the decision-quality table; policy.
    Output: the time in ms.
    """
    if policy == "original":
        return entry["original_ms"]
    if policy == "always_index":
        return entry["index_ms"]
    if policy == "always_filter":
        return entry["filter_ms"]
    # auto: the forced run of the path the cost model chose
    if entry["auto_choice"].startswith("filter"):
        return entry["filter_ms"]
    return entry["index_ms"]


def policy_regrets(table):
    """
    The regret of every policy in every configuration, relative to the
    fastest of original, index and filter.

    Input:  table, from decision_quality.
    Output: a list of dicts: config, then one regret column per policy.
    """
    result = []
    for entry in table:
        best_time = min(entry["original_ms"], entry["index_ms"], entry["filter_ms"])
        row = {"config": entry["config"]}
        for policy in POLICIES:
            row[policy] = policy_time(entry, policy) / best_time
        result.append(row)
    return result


def summarize_policies(regret_rows):
    """
    Mean, median and worst regret of each policy, which configuration was
    the worst, and in how many configurations the policy was within 5% of
    the best.

    Input:  regret_rows, from policy_regrets.
    Output: a list of dicts, one per policy.
    """
    summary = []
    for policy in POLICIES:
        values = [row[policy] for row in regret_rows]
        worst_row = max(regret_rows, key=lambda row: row[policy])
        summary.append({
            "policy": policy,
            "mean": statistics.mean(values),
            "median": statistics.median(values),
            "worst": worst_row[policy],
            "worst_config": worst_row["config"],
            "within_5_percent": sum(1 for value in values if value <= WITHIN_BEST),
            "configurations": len(values),
        })
    return summary


def write_policy_comparison(regret_rows, summary):
    """
    Write policy_comparison.csv (one row per configuration) and
    policy_comparison.md (the summary table).

    Input:  regret_rows; summary.
    Output: nothing.
    """
    with open(os.path.join(RESULTS_DIR, "policy_comparison.csv"), "w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=["config"] + POLICIES)
        writer.writeheader()
        for row in regret_rows:
            writer.writerow(row)

    lines = ["| policy | mean regret | median regret | worst regret (configuration) "
             "| within 5% of the best |",
             "|---|---:|---:|---|---:|"]
    for entry in summary:
        name = POLICY_LABELS[entry["policy"]].replace("\n", " ")
        lines.append(f"| {name} | {entry['mean']:,.2f} | {entry['median']:,.2f} "
                     f"| {entry['worst']:,.2f} ({entry['worst_config']}) "
                     f"| {entry['within_5_percent']} of {entry['configurations']} |")
    with open(os.path.join(RESULTS_DIR, "policy_comparison.md"), "w") as md_file:
        md_file.write("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------
# Charts
# ---------------------------------------------------------------------------

def style_axes(axes, title, xlabel, ylabel):
    """
    Common look of every chart: title, axis labels, a light grid behind the
    data.

    Input:  axes, the matplotlib axes; title, xlabel, ylabel.
    Output: nothing.
    """
    axes.set_title(title, fontsize=11)
    axes.set_xlabel(xlabel)
    axes.set_ylabel(ylabel)
    axes.grid(True, which="major", color="#e4e3df", linewidth=0.8)
    axes.set_axisbelow(True)
    for side in ["top", "right"]:
        axes.spines[side].set_visible(False)


def plot_lines(axes, rows, x_column, y_column, strategies=PLOT_ORDER):
    """
    One line per strategy: y_column against x_column.

    Input:  axes; rows, the rows to draw; x_column, y_column; strategies.
    Output: nothing.
    """
    for strategy in strategies:
        points = sorted((row[x_column], row[y_column]) for row in rows
                        if row["strategy"] == strategy and row[y_column] is not None)
        if not points:
            continue
        style = STYLE[strategy]
        axes.plot([p[0] for p in points], [p[1] for p in points], color=style["color"],
                  marker=style["marker"], markersize=6, linewidth=2, label=style["label"])


def save(figure, name, bottom=0.0):
    """
    Save a figure as results/final/<name> and close it.

    Input:  figure; name, the file name; bottom, the share of the figure
            height kept free at the bottom (for a legend placed there).
    Output: nothing.
    """
    figure.tight_layout(rect=[0, bottom, 1, 1])
    figure.savefig(os.path.join(RESULTS_DIR, name), dpi=130)
    plt.close(figure)
    print("  wrote results/final/" + name)


def chart_e1(rows, y_column, ylabel, file_prefix, title_prefix):
    """
    E1: y_column against n, one chart per row width (log-log).

    Input:  rows, all rows; y_column; ylabel; file_prefix (e.g. "time_vs_n");
            title_prefix.
    Output: nothing.
    """
    for width in ["narrow", "wide"]:
        e1_rows = [row for row in rows if row["experiment"] == "E1" and row["width"] == width]
        if not e1_rows:
            continue
        figure, axes = plt.subplots(figsize=(7.5, 5))
        plot_lines(axes, e1_rows, "n", y_column)
        axes.set_xscale("log")
        axes.set_yscale("log")
        columns = "o.order_id, c.name" + (", c.padding" if width == "wide" else "")
        style_axes(axes, f"{title_prefix} ({width} rows: {columns})\n"
                         "1,000,000 remote customers, 1000 Mbit/s (localhost)",
                   "n = orders kept (order_id <= n), log scale", ylabel)
        axes.legend(fontsize=8)
        save(figure, f"{file_prefix}_{width}.png")


def legend_below(figure, axes):
    """
    One legend for a figure with several panels, centered below them, so it
    never covers data. It uses the lines/bars of the first panel.

    Input:  figure; axes, the first panel.
    Output: nothing.
    """
    handles, labels = axes.get_legend_handles_labels()
    figure.legend(handles, labels, loc="lower center", ncol=min(len(labels), 4), fontsize=8,
                  frameon=False)


def chart_bars(table, value_column, ylabel, title, file_name, log_scale=False, reference=None):
    """
    One bar per configuration (decision-quality charts).

    Input:  table; value_column ("regret" or "speedup"); ylabel; title;
            file_name; log_scale; reference, y value of a dashed line.
    Output: nothing.
    """
    figure, axes = plt.subplots(figsize=(max(9, 0.32 * len(table)), 5.5))
    positions = list(range(len(table)))
    colors = []
    for entry in table:
        # wrong choices stand out in a second color
        colors.append(STYLE["auto"]["color"] if entry["correct_choice"] else "#6250d6")
    axes.bar(positions, [entry[value_column] for entry in table], color=colors, width=0.75)
    if reference is not None:
        axes.axhline(reference, color="#52514e", linewidth=1.2, linestyle="--")
    axes.set_xticks(positions)
    axes.set_xticklabels([entry["config"] for entry in table], rotation=90, fontsize=7)
    if log_scale:
        axes.set_yscale("log")
    style_axes(axes, title, "configuration", ylabel)
    # legend for the two bar colors (Patch = a colored square for the legend)
    axes.legend(handles=[Patch(color=STYLE["auto"]["color"], label="auto chose the fastest path"),
                         Patch(color="#6250d6", label="auto chose a slower path")], fontsize=8)
    save(figure, file_name)


def chart_policies(summary):
    """
    Batch 8b: mean and worst regret of each policy, side by side (log
    scale, because "original" is hundreds of times slower), with a line at
    1.0 (= always as fast as the best strategy).

    Input:  summary, from summarize_policies.
    Output: nothing.
    """
    figure, axes = plt.subplots(figsize=(8, 5))
    positions = list(range(len(summary)))
    bar_width = 0.38
    axes.bar([p - bar_width / 2 for p in positions], [entry["mean"] for entry in summary],
             width=bar_width * 0.92, color="#2a78d6", label="mean regret")
    axes.bar([p + bar_width / 2 for p in positions], [entry["worst"] for entry in summary],
             width=bar_width * 0.92, color="#eb6834", label="worst regret")
    # print the values on the bars, so the small differences are readable
    for position, entry in zip(positions, summary):
        axes.text(position - bar_width / 2, entry["mean"] * 1.08, f"{entry['mean']:.2f}",
                  ha="center", fontsize=8, color="#0b0b0b")
        axes.text(position + bar_width / 2, entry["worst"] * 1.08, f"{entry['worst']:.2f}",
                  ha="center", fontsize=8, color="#0b0b0b")
    axes.axhline(1.0, color="#52514e", linewidth=1.2, linestyle="--")
    axes.set_yscale("log")
    axes.set_xticks(positions)
    axes.set_xticklabels([POLICY_LABELS[entry["policy"]] for entry in summary], fontsize=9)
    style_axes(axes, f"Regret of four policies over {summary[0]['configurations']} configurations\n"
                     "(forced-path times; 1.00 = always as fast as the best strategy)",
               "", "regret = policy time / fastest time (log scale)")
    axes.legend(fontsize=8)
    save(figure, "policy_regret.png")


def chart_predicted_vs_measured(rows):
    """
    Scatter: the time our model predicted for a path (EXPLAIN's Semijoin
    Predicted Time) against the measured end-to-end time of the forced run
    of that path, with the line y = x.

    Input:  rows, all rows.
    Output: nothing.
    """
    figure, axes = plt.subplots(figsize=(6.5, 6))
    all_values = []
    for strategy, predicted_column in [("index", "pred_index_ms"), ("filter", "pred_filter_ms")]:
        points = [(row[predicted_column], row["time_ms"]) for row in rows
                  if row["strategy"] == strategy and row[predicted_column] is not None
                  and row["network_method"] != "simulated"]
        if not points:
            continue
        style = STYLE[strategy]
        axes.scatter([p[0] for p in points], [p[1] for p in points], color=style["color"],
                     marker=style["marker"], s=36, label=style["label"],
                     edgecolors="white", linewidths=0.8)
        for point in points:
            all_values.extend(point)
    # E5 at 10 Mbit: auto_loopback's prediction for the path it chose, made
    # with the LOOPBACK network numbers, against its measured time
    loopback_points = []
    for row in rows:
        if row["strategy"] == "auto_loopback":
            predicted = row["pred_index_ms"] if row["path"] == "index" else row["pred_filter_ms"]
            loopback_points.append((predicted, row["time_ms"]))
    if loopback_points:
        style = STYLE["auto_loopback"]
        axes.scatter([p[0] for p in loopback_points], [p[1] for p in loopback_points],
                     color=style["color"], marker=style["marker"], s=50,
                     label="10 Mbit, model NOT told about the network", edgecolors="white",
                     linewidths=0.8)
        for point in loopback_points:
            all_values.extend(point)
    low = min(all_values) * 0.7
    high = max(all_values) * 1.4
    axes.plot([low, high], [low, high], color="#52514e", linestyle="--", linewidth=1.2,
              label="y = x (perfect prediction)")
    axes.set_xscale("log")
    axes.set_yscale("log")
    axes.set_xlim(low, high)
    axes.set_ylim(low, high)
    style_axes(axes, "Predicted vs measured time, all configurations\n"
                     "(points above the line: slower than predicted)",
               "predicted time of the path (ms, log scale)",
               "measured end-to-end time (ms, log scale)")
    axes.legend(fontsize=8)
    save(figure, "predicted_vs_measured.png")


def default_rows(rows, n_values):
    """
    The E1 narrow rows at the given n: the default point of E2-E6.

    Input:  rows; n_values.
    Output: a list of rows.
    """
    return [row for row in rows if row["experiment"] == "E1" and row["width"] == "narrow"
            and row["n"] in n_values]


def chart_factor(rows, experiment, x_column, xlabel, title, file_name, log_x=False):
    """
    E2 / E3: time against one factor, one panel per n.

    Input:  rows; experiment ("E2" or "E3"); x_column ("s_rows" or
            "match_fraction"); xlabel; title; file_name; log_x.
    Output: nothing.
    """
    n_values = sorted({row["n"] for row in rows if row["experiment"] == experiment})
    if not n_values:
        return
    chosen = [row for row in rows if row["experiment"] == experiment] + default_rows(rows, n_values)
    figure, panels = plt.subplots(1, len(n_values), figsize=(5 * len(n_values), 4.6), sharey=True)
    for axes, n in zip(panels, n_values):
        plot_lines(axes, [row for row in chosen if row["n"] == n], x_column, "time_ms")
        if log_x:
            axes.set_xscale("log")
        axes.set_yscale("log")
        style_axes(axes, f"n = {n:,}", xlabel, "end-to-end time (ms, log scale)")
    legend_below(figure, panels[0])
    figure.suptitle(title, fontsize=12)
    save(figure, file_name, bottom=0.1)


def chart_grouped(rows, groups, title, file_name, ylabel="end-to-end time (ms, log scale)",
                  value_column="time_ms", strategies=("original", "index", "filter", "auto")):
    """
    Grouped bars, one panel per n: for each group (e.g. uniform / skewed),
    one bar per strategy.

    Input:  rows; groups, a list of (group label, list of rows); title;
            file_name; ylabel; value_column; strategies.
    Output: nothing.
    """
    n_values = sorted({row["n"] for label, group_rows in groups for row in group_rows})
    figure, panels = plt.subplots(1, len(n_values), figsize=(4.6 * len(n_values), 4.6), sharey=True)
    if len(n_values) == 1:
        panels = [panels]
    bar_width = 0.8 / len(strategies)
    for axes, n in zip(panels, n_values):
        for index, strategy in enumerate(strategies):
            heights = []
            for label, group_rows in groups:
                values = [row[value_column] for row in group_rows
                          if row["n"] == n and row["strategy"] == strategy]
                heights.append(values[0] if values and values[0] is not None else 0)
            positions = [group + (index - (len(strategies) - 1) / 2) * bar_width
                         for group in range(len(groups))]
            axes.bar(positions, heights, width=bar_width * 0.92, color=STYLE[strategy]["color"],
                     label=STYLE[strategy]["label"])
        axes.set_xticks(list(range(len(groups))))
        axes.set_xticklabels([label for label, group_rows in groups])
        axes.set_yscale("log")
        style_axes(axes, f"n = {n:,}", "", ylabel)
    legend_below(figure, panels[0])
    figure.suptitle(title, fontsize=12)
    save(figure, file_name, bottom=0.08)


def chart_e5(rows):
    """
    E5: time against n, one panel per bandwidth (1000 Mbit = E1 narrow).

    Input:  rows.
    Output: nothing.
    """
    e5_rows = [row for row in rows if row["experiment"] == "E5"]
    if not e5_rows:
        return
    method = e5_rows[0]["network_method"]
    bandwidths = sorted({row["network_mbit"] for row in e5_rows}, reverse=True)
    panels_data = [("1000 Mbit/s (no tc, E1)", default_rows(rows, set(row["n"] for row in e5_rows)))]
    for mbit in bandwidths:
        panels_data.append((f"{mbit:g} Mbit/s ({'simulated' if method == 'simulated' else 'tc netem'})",
                            [row for row in e5_rows if row["network_mbit"] == mbit]))
    figure, panels = plt.subplots(1, len(panels_data), figsize=(5 * len(panels_data), 4.6),
                                  sharey=True)
    for axes, (label, panel_rows) in zip(panels, panels_data):
        plot_lines(axes, panel_rows, "n", "time_ms")
        axes.set_xscale("log")
        axes.set_yscale("log")
        style_axes(axes, label, "n = orders kept, log scale", "end-to-end time (ms, log scale)")
    # the last panel (10 Mbit) also has auto_loopback, so take the legend there
    legend_below(figure, panels[-1])
    title = "E5: network bandwidth (narrow rows, 1,000,000 customers)"
    if method == "simulated":
        title += "  [SIMULATED: measured time + measured bytes / bandwidth]"
    else:
        title += "  [real network emulation: tc netem on lo, + 1 ms delay]"
    figure.suptitle(title, fontsize=12)
    save(figure, "e5_bandwidth.png", bottom=0.12)


def chart_e6(rows):
    """
    E6: binary filter off (E1 narrow) vs on: the filter path's parameter
    bytes and time, and auto's time.

    Input:  rows.
    Output: nothing.
    """
    on_rows = [row for row in rows if row["experiment"] == "E6"]
    if not on_rows:
        return
    n_values = sorted({row["n"] for row in on_rows})
    off_rows = default_rows(rows, n_values)
    figure, panels = plt.subplots(1, 2, figsize=(11, 4.6))
    bar_width = 0.38
    for axes, column, ylabel, title in [
            (panels[0], "param_bytes_sent", "filter bytes sent A -> B", "Filter size on the wire"),
            (panels[1], "time_ms", "end-to-end time (ms)", "Filter path time")]:
        for offset, label, group_rows, color in [(-0.5, "hex text (off)", off_rows, "#2a78d6"),
                                                 (0.5, "binary (on)", on_rows, "#eb6834")]:
            heights = []
            for n in n_values:
                values = [row[column] for row in group_rows
                          if row["n"] == n and row["strategy"] == "filter"]
                heights.append(values[0] if values else 0)
            positions = [index + offset * bar_width for index in range(len(n_values))]
            axes.bar(positions, heights, width=bar_width * 0.92, color=color, label=label)
        axes.set_xticks(list(range(len(n_values))))
        axes.set_xticklabels([f"n = {n:,}" for n in n_values])
        style_axes(axes, title, "", ylabel)
        axes.legend(fontsize=8)
    figure.suptitle("E6: binary filter (postgres_fdw.semijoin_binary_filter), forced filter path",
                    fontsize=12)
    save(figure, "e6_binary_filter.png")


def main():
    rows = load_rows()
    if not rows:
        raise SystemExit("no results/final/e*.csv found: run bench/final_eval.py first")
    configs = group_by_config(rows)
    table = decision_quality(configs)
    write_decision_quality(table)
    numbers = headline(table, rows)
    write_headline(numbers)

    print("Decision quality (times in ms, end to end, median of 3):")
    common.print_table(table, ["config", "original_ms", "index_ms", "filter_ms", "auto_ms",
                               "auto_choice", "best", "regret", "correct_choice", "speedup"])
    print()
    with open(os.path.join(RESULTS_DIR, "headline.md")) as md_file:
        print(md_file.read())

    # Batch 8b: four policies, forced-path times only
    regret_rows = policy_regrets(table)
    policy_summary = summarize_policies(regret_rows)
    write_policy_comparison(regret_rows, policy_summary)
    print("Policy comparison (regret vs the fastest of original/index/filter, forced-path times):")
    with open(os.path.join(RESULTS_DIR, "policy_comparison.md")) as md_file:
        print(md_file.read())

    print("Charts:")
    chart_e1(rows, "time_ms", "end-to-end time (ms, log scale)", "time_vs_n",
             "E1: query time vs n")
    chart_e1(rows, "measured_bytes", "measured bytes on lo (log scale)", "bytes_vs_n",
             "E1: real network bytes vs n")
    # Batch 8b: the forced run of the path auto chose (path_regret), so no
    # bar falls below 1.0 because of session noise
    chart_bars(table, "path_regret", "regret = time of the chosen path / fastest time",
               "Regret per configuration: forced-path time of the path auto chose\n"
               "(1.00 = auto chose the fastest strategy)",
               "regret.png", reference=1.0)
    chart_policies(policy_summary)
    chart_bars(table, "speedup", "speedup = original time / auto time (log scale)",
               "Speedup of auto over plain postgres_fdw, per configuration",
               "speedup.png", log_scale=True, reference=1.0)
    chart_predicted_vs_measured(rows)
    chart_factor(rows, "E2", "s_rows", "rows of customers at site B (log scale)",
                 "E2: remote table size (narrow rows, match 1.0, uniform)", "e2_s_rows.png",
                 log_x=True)
    chart_factor(rows, "E3", "match_fraction", "match fraction (share of order keys at B)",
                 "E3: match fraction (narrow rows, 1,000,000 customers)", "e3_match_fraction.png")
    e4_rows = [row for row in rows if row["experiment"] == "E4"]
    if e4_rows:
        n_values = {row["n"] for row in e4_rows}
        chart_grouped(rows, [("uniform", default_rows(rows, n_values)), ("skewed", e4_rows)],
                      "E4: key skew (customer_id = 1 + floor(s_rows * random()^3))",
                      "e4_skew.png")
    chart_e5(rows)
    chart_e6(rows)


if __name__ == "__main__":
    main()
