#!/usr/bin/env python3
"""
Latency visualization for the Lock-Free Order Book.

Generates three charts from benchmark output:
  1. Latency histogram (log-scale X) per operation
  2. CDF (cumulative distribution function) — shows tail clearly
  3. Per-stage breakdown heatmap (from tracer CSV)

Usage:
  # First run benchmarks to produce JSON:
  ./build/latency_bench --benchmark_format=json --benchmark_out=docs/benchmarks/bench.json

  # Then plot:
  python3 scripts/plot_latency.py

  # Or with explicit paths:
  python3 scripts/plot_latency.py \
      --bench  docs/benchmarks/bench.json \
      --stages docs/benchmarks/stages.csv \
      --outdir docs/benchmarks/
"""

import argparse
import json
import csv
import os
import sys
from pathlib import Path
from collections import defaultdict

# ── Optional matplotlib import ────────────────────────────────────────────────
try:
    import matplotlib
    matplotlib.use("Agg")          # headless — no display required
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import numpy as np
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("matplotlib not found. Install with: pip3 install matplotlib numpy")
    print("Falling back to text-only output.\n")


# ── Parsing ───────────────────────────────────────────────────────────────────

def parse_bench_json(path: str) -> dict[str, list[float]]:
    """Return {benchmark_base_name: [cpu_time_ns, ...]} from Google Benchmark JSON."""
    with open(path) as f:
        data = json.load(f)

    raw: dict[str, list[float]] = defaultdict(list)
    for b in data.get("benchmarks", []):
        name: str = b["name"]
        # Strip /iterations:NNNN suffix and aggregate suffixes (_mean etc.)
        base = name.split("/")[0]
        if any(base.endswith(s) for s in ("_mean", "_median", "_stddev", "_cv")):
            continue
        raw[base].append(b.get("cpu_time", b.get("real_time", 0.0)))
    return dict(raw)


def parse_stages_csv(path: str) -> dict[str, list[float]]:
    """Return {stage_name: [ns, ...]} from the tracer dump_csv output."""
    stages: dict[str, list[float]] = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            stages[row["stage"]].append(float(row["ns"]))
    return dict(stages)


# ── Text fallback ─────────────────────────────────────────────────────────────

def percentile(data: list[float], p: float) -> float:
    if not data:
        return 0.0
    s = sorted(data)
    i = int(p / 100.0 * (len(s) - 1))
    return s[i]


def print_text_summary(bench: dict[str, list[float]],
                        stages: dict[str, list[float]]) -> None:
    print("=== Benchmark Summary ===")
    print(f"{'Name':<30}  {'p50':>7}  {'p99':>7}  {'p99.9':>8}  {'samples':>8}")
    print(f"{'----':<30}  {'---':>7}  {'---':>7}  {'-----':>8}  {'-------':>8}")
    for name, samples in sorted(bench.items()):
        p50  = percentile(samples, 50)
        p99  = percentile(samples, 99)
        p999 = percentile(samples, 99.9)
        print(f"{name:<30}  {p50:>6.0f}ns  {p99:>6.0f}ns  {p999:>7.0f}ns  {len(samples):>8}")

    if stages:
        print("\n=== Per-Stage Latency ===")
        print(f"{'Stage':<18}  {'p50':>7}  {'p99':>7}  {'samples':>8}")
        for stage, samples in sorted(stages.items()):
            p50 = percentile(samples, 50)
            p99 = percentile(samples, 99)
            print(f"{stage:<18}  {p50:>6.0f}ns  {p99:>6.0f}ns  {len(samples):>8}")


# ── Plotting ──────────────────────────────────────────────────────────────────

PALETTE = [
    "#2196F3", "#F44336", "#4CAF50", "#FF9800",
    "#9C27B0", "#00BCD4", "#FF5722", "#607D8B",
]


def plot_histogram(bench: dict[str, list[float]], outdir: str) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle("Order Book Operation Latency Distribution", fontsize=14, fontweight="bold")

    ax_hist, ax_cdf = axes

    for idx, (name, samples) in enumerate(sorted(bench.items())):
        color = PALETTE[idx % len(PALETTE)]
        label = name.replace("BM_", "").replace("_", " ")
        arr = np.array(samples, dtype=float)

        # ── Histogram (log X) ──
        bins = np.logspace(np.log10(max(arr.min(), 0.1)), np.log10(arr.max() + 1), 60)
        ax_hist.hist(arr, bins=bins, alpha=0.55, color=color, label=label, density=True)

        # ── CDF ──
        s = np.sort(arr)
        cdf = np.arange(1, len(s) + 1) / len(s)
        ax_cdf.plot(s, cdf * 100, color=color, label=label, linewidth=1.5)

    # histogram formatting
    ax_hist.set_xscale("log")
    ax_hist.set_xlabel("Latency (ns) — log scale", fontsize=11)
    ax_hist.set_ylabel("Density", fontsize=11)
    ax_hist.set_title("Latency Histogram")
    ax_hist.legend(fontsize=9)
    ax_hist.grid(True, which="both", alpha=0.3)

    # CDF formatting
    ax_cdf.set_xscale("log")
    ax_cdf.set_xlabel("Latency (ns) — log scale", fontsize=11)
    ax_cdf.set_ylabel("Percentile (%)", fontsize=11)
    ax_cdf.set_title("Cumulative Distribution (CDF)")
    ax_cdf.set_yticks([50, 90, 95, 99, 99.9, 100])
    ax_cdf.axhline(99,  color="gray", linestyle="--", alpha=0.5, linewidth=0.8)
    ax_cdf.axhline(99.9, color="gray", linestyle=":",  alpha=0.5, linewidth=0.8)
    ax_cdf.legend(fontsize=9)
    ax_cdf.grid(True, which="both", alpha=0.3)

    plt.tight_layout()
    out = os.path.join(outdir, "latency_histogram.png")
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Saved: {out}")


def plot_percentile_bars(bench: dict[str, list[float]], outdir: str) -> None:
    names  = sorted(bench.keys())
    labels = [n.replace("BM_", "").replace("_", "\n") for n in names]
    pcts   = [50, 95, 99, 99.9]

    data = {p: [percentile(bench[n], p) for n in names] for p in pcts}

    x = np.arange(len(names))
    width = 0.18
    fig, ax = plt.subplots(figsize=(max(10, len(names) * 2), 6))

    for i, p in enumerate(pcts):
        offset = (i - len(pcts) / 2 + 0.5) * width
        bars = ax.bar(x + offset, data[p], width, label=f"p{p}",
                      color=PALETTE[i], alpha=0.85)
        ax.bar_label(bars, fmt="%.0f", fontsize=7, padding=2)

    ax.set_yscale("log")
    ax.set_ylabel("Latency (ns) — log scale", fontsize=11)
    ax.set_title("Latency Percentiles by Operation", fontsize=13, fontweight="bold")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=9)
    ax.legend(fontsize=10)
    ax.grid(True, axis="y", alpha=0.3)
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f"{v:.0f}ns"))

    plt.tight_layout()
    out = os.path.join(outdir, "percentile_bars.png")
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Saved: {out}")


def plot_stage_heatmap(stages: dict[str, list[float]], outdir: str) -> None:
    if not stages:
        return

    stage_names = sorted(stages.keys())
    pcts = [50, 75, 90, 95, 99, 99.9]
    matrix = np.array([[percentile(stages[s], p) for s in stage_names] for p in pcts])

    fig, ax = plt.subplots(figsize=(max(8, len(stage_names) * 1.4), 4))
    im = ax.imshow(matrix, aspect="auto", cmap="YlOrRd")

    ax.set_xticks(range(len(stage_names)))
    ax.set_xticklabels([s.replace("_", "\n") for s in stage_names], fontsize=9)
    ax.set_yticks(range(len(pcts)))
    ax.set_yticklabels([f"p{p}" for p in pcts], fontsize=9)
    ax.set_title("Per-Stage Latency Heatmap (ns)", fontsize=12, fontweight="bold")

    for i in range(len(pcts)):
        for j in range(len(stage_names)):
            ax.text(j, i, f"{matrix[i, j]:.0f}",
                    ha="center", va="center", fontsize=8,
                    color="white" if matrix[i, j] > matrix.max() * 0.6 else "black")

    plt.colorbar(im, ax=ax, label="nanoseconds")
    plt.tight_layout()
    out = os.path.join(outdir, "stage_heatmap.png")
    plt.savefig(out, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"Saved: {out}")


# ── main ─────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Order book latency plotter")
    parser.add_argument("--bench",  default="docs/benchmarks/bench.json",
                        help="Google Benchmark JSON output")
    parser.add_argument("--stages", default="docs/benchmarks/stages.csv",
                        help="LatencyTracer CSV (from tracer.dump_csv)")
    parser.add_argument("--outdir", default="docs/benchmarks/",
                        help="Output directory for PNG files")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    bench: dict[str, list[float]] = {}
    if os.path.exists(args.bench):
        bench = parse_bench_json(args.bench)
        print(f"Loaded {sum(len(v) for v in bench.values())} benchmark samples "
              f"from {args.bench}")
    else:
        print(f"Benchmark JSON not found: {args.bench}")
        print("Run:  ./build/latency_bench "
              "--benchmark_format=json --benchmark_out=docs/benchmarks/bench.json")

    stages: dict[str, list[float]] = {}
    if os.path.exists(args.stages):
        stages = parse_stages_csv(args.stages)
        print(f"Loaded {sum(len(v) for v in stages.values())} stage samples "
              f"from {args.stages}")
    else:
        print(f"Stage CSV not found: {args.stages}  "
              f"(run main binary with -DENABLE_TRACING)")

    # Always print text summary
    print_text_summary(bench, stages)

    if not HAS_MPL:
        return

    if bench:
        plot_histogram(bench, args.outdir)
        plot_percentile_bars(bench, args.outdir)

    if stages:
        plot_stage_heatmap(stages, args.outdir)

    print("\nDone. Add these PNGs to your README for maximum resume impact.")


if __name__ == "__main__":
    main()
