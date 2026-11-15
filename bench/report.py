#!/usr/bin/env python3
"""Turns bench/results/raw.csv (written by run.sh) into results.md.

    report.py [RESULTS_DIR=bench/results]

One Markdown table per scenario. If matplotlib is importable it also draws
throughput.png and p99.png for the keep-alive sweep; otherwise those are
skipped without a message. Standard library only apart from that optional
import."""

import csv
import os
import sys

# (scenario, path, heading, one factual line describing the runs)
TABLES = [
    ("keepalive", "/health", "Keep-alive, `GET /health`",
     "Each connection sends requests back to back on one keep-alive connection; the response is a small JSON body."),
    ("keepalive", "/file16k.bin", "Keep-alive, `GET /file16k.bin`",
     "Same as above, serving a 16 KiB static file from memory."),
    ("no-keepalive", "/health", "New connection per request, `GET /health`",
     "Every request opens a new TCP connection; latency includes connect and accept."),
    ("idle", "/health", "Idle clients, `GET /health`",
     "64 active keep-alive connections, plus idle connections that are opened before the run and never send anything."),
]


def ms(text):
    value = float(text)
    return "%.3f" % value if value < 10 else "%.1f" % value


def errors_cell(row):
    cell = row["errors"]
    if row["error_kinds"]:
        cell += " (%s)" % row["error_kinds"].replace(";", ",")
    return cell


def idle_cell(row):
    closed = int(row["idle_closed"] or 0)
    if closed:
        return "%s (%d closed by server)" % (row["idle"], closed)
    return row["idle"]


def has_numbers(row):
    """An ok row normally carries loadgen's numbers; one without them (loadgen
    exited 0 but printed nothing) is reported as failed instead of crashing."""
    return all(row[k] for k in ("rps", "p50_ms", "p95_ms", "p99_ms", "max_ms"))


def table_rows(rows):
    out = ["| mode | connections | idle | req/s | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) | errors |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    for r in rows:
        head = [r["mode"], r["connections"], idle_cell(r)]
        if r["status"] == "skipped":
            cells = ["", "", "", "", "", "skipped: " + r["note"]]
        elif r["status"] == "failed" or not has_numbers(r):
            reason = r["note"] or "loadgen printed no result"
            if r["errors"]:
                reason += " (%s errors%s)" % (r["errors"], ": " + r["error_kinds"].replace(";", ",")
                                             if r["error_kinds"] else "")
            cells = ["", "", "", "", "", "failed: " + reason]
        else:
            cells = ["%.0f" % float(r["rps"]), ms(r["p50_ms"]), ms(r["p95_ms"]), ms(r["p99_ms"]),
                     ms(r["max_ms"]), errors_cell(r)]
            if r["server_exit"] not in ("", "0"):
                cells[-1] += "; server exit status " + r["server_exit"]
            if r["note"]:
                cells[-1] += "; " + r["note"]
        out.append("| " + " | ".join(head + cells) + " |")
    return out


def environment_field(env_text, name):
    for line in env_text.splitlines():
        if line.startswith(name + ":"):
            return line.split(":", 1)[1].strip()
    return "unknown"


def sort_key(r):
    return (0 if r["mode"] == "pool" else 1, int(r["connections"]), int(r["idle"]))


def write_markdown(results_dir, rows, env_text):
    durations = sorted({r["duration_s"] for r in rows if r["duration_s"]})
    threads = sorted({r["server_threads"] for r in rows})
    client_threads = sorted({r["loadgen_threads"] for r in rows})
    lines = [
        "# Benchmark results",
        "",
        "Date: %s. Commit: %s. Machine, kernel, compiler and flags: [environment.txt](environment.txt)."
        % (environment_field(env_text, "date"), environment_field(env_text, "git commit")),
        "",
        "Server `--threads %s`, loadgen `-t %s` on the same machine over loopback; %s s measured after 1 s "
        "of warm-up; closed loop (each connection waits for its response before sending the next request). "
        "Pool mode runs with `--queue-capacity 4096 --keepalive-timeout 5`. Raw data: [raw.csv](raw.csv)."
        % ("/".join(threads), "/".join(client_threads), "/".join(d.rstrip("0").rstrip(".") for d in durations)),
    ]
    for scenario, path, heading, description in TABLES:
        selected = sorted((r for r in rows if r["scenario"] == scenario and r["path"] == path), key=sort_key)
        if not selected:
            continue
        lines += ["", "## " + heading, "", description, ""]
        lines += table_rows(selected)
    with open(os.path.join(results_dir, "results.md"), "w") as f:
        f.write("\n".join(lines) + "\n")


def write_charts(results_dir, rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return
    # Fixed colours per mode (identity, never by rank) plus distinct markers,
    # so the two lines are not told apart by colour alone.
    style = {"pool": ("#2a78d6", "o"), "epoll": ("#eb6834", "s")}
    paths = ["/health", "/file16k.bin"]

    def points(path, mode, field):
        return sorted((int(r["connections"]), float(r[field])) for r in rows
                      if r["scenario"] == "keepalive" and r["path"] == path
                      and r["mode"] == mode and r["status"] == "ok" and has_numbers(r))

    # p99 alone would hide a wait that hits fewer than 1 % of requests (pool
    # mode's wait for a free worker), so the latency chart also draws max.
    charts = [("throughput.png", [("rps", "", "-")], "requests/s", False,
               "Failed runs are not plotted; see results.md."),
              ("p99.png", [("p99_ms", " p99", "-"), ("max_ms", " max", "--")], "latency (ms, log scale)", True,
               "Solid: p99. Dashed: max. Failed runs are not plotted; see results.md.")]
    for filename, series, ylabel, log_y, footnote in charts:
        fig, axes = plt.subplots(1, len(paths), figsize=(10, 4), sharey=True)
        for ax, path in zip(axes, paths):
            ticks = set()
            for mode, (color, marker) in style.items():
                for field, suffix, line in series:
                    data = points(path, mode, field)
                    if not data:
                        continue
                    xs, ys = zip(*data)
                    ticks.update(xs)
                    ax.plot(xs, ys, color=color, marker=marker, markersize=6, linewidth=2, linestyle=line,
                            label=mode + suffix)
            ax.set_xscale("log", base=2)
            ax.set_xticks(sorted(ticks))
            ax.set_xticklabels([str(t) for t in sorted(ticks)])
            ax.minorticks_off()
            if log_y:
                ax.set_yscale("log")
            ax.set_title("keep-alive, GET " + path, fontsize=10)
            ax.set_xlabel("connections")
            ax.grid(True, color="#e0e0e0", linewidth=0.8)
            for side in ("top", "right"):
                ax.spines[side].set_visible(False)
        axes[0].set_ylabel(ylabel)
        axes[0].legend(frameon=False, fontsize=8)
        fig.text(0.01, 0.01, footnote, fontsize=8, color="#666666")
        fig.tight_layout(rect=(0, 0.03, 1, 1))
        fig.savefig(os.path.join(results_dir, filename), dpi=120)
        plt.close(fig)


def main():
    results_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "results")
    with open(os.path.join(results_dir, "raw.csv"), newline="") as f:
        rows = list(csv.DictReader(f))
    env_path = os.path.join(results_dir, "environment.txt")
    env_text = open(env_path).read() if os.path.exists(env_path) else ""
    write_markdown(results_dir, rows, env_text)
    write_charts(results_dir, rows)


if __name__ == "__main__":
    main()
