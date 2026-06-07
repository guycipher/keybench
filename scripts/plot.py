#!/usr/bin/env python3
"""Render publication figures from keybench result directories."""
from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

TAB = "\t"
NS_PER_US = 1000.0
FIG_DPI = 150
FIG_FORMATS = ("png", "pdf", "svg")
DEFAULT_FORMAT = "png"
MAX_DIRS = 64
MAX_PANELS = 128

POINTS_FILE = "points.tsv"
TIMELINE_FILE = "timeline.tsv"

COL_WORKLOAD = "workload"
COL_ENGINE = "engine"
COL_THREADS = "threads"
COL_SWEEP_PARAM = "sweep_param"
COL_SWEEP_VALUE = "sweep_value"
COL_PHASE = "phase"
COL_OP = "op"
COL_ELAPSED = "elapsed_s"
COL_METRIC = "metric"
COL_VALUE = "value"
COL_WU = "wu_per_s"
COL_OPS = "ops_per_s"
COL_HIT = "hit_rate"

PCTL_COLUMNS = (("p50", "p50_ns"), ("p99", "p99_ns"), ("p99.9", "p999_ns"))
COMPARE_PCTLS = ("p50", "p99")
PALETTE_FILE = "BACKENDPALETTE"

THROUGHPUT_METRIC = "wu_per_s"
SEED_METRIC = "seed_keys"
THROUGHPUT_ALL = ("wu_per_s", "ops_per_s")
SYSTEM_ALL = (
    "cpu_util_pct", "proc_cpu_pct", "mem_avail_mb", "load1", "rss_mb", "peak_rss_mb",
    "minflt", "majflt", "vol_ctxsw", "invol_ctxsw", "io_read_mb", "io_write_mb",
    "threads", "disk_bytes",
)
ENGINE_PREFERRED = (
    "estimate-num-keys", "keys", "levels", "level", "sstables", "total-sst-files-size",
    "live-sst-files-size", "data_bytes", "memtable_bytes", "cur-size-all-mem-tables",
    "compaction_bytes", "estimate-pending-compaction-bytes", "compaction_count",
    "num-running-compactions", "num-running-flushes", "flush_count", "wal_bytes",
    "read_amp", "cache_hit_rate", "block-cache-usage", "cache_bytes",
)
BYTE_METRIC_SUFFIXES = ("size", "bytes", "usage")
DISK_METRIC = "disk_bytes"
BYTES_PER_MIB = 1024.0 * 1024.0
REP_WORKLOAD_PREF = ("mixed", "cart", "batch", "scan")

PANEL_W = 5.0
PANEL_H = 3.6
LEGEND_FONTSIZE = 7
GRID_ALPHA = 0.3


def to_float(text: str) -> float:
    assert isinstance(text, str), "value must be a string"
    assert text.strip() != "", "value must not be empty"
    return float(text)


def to_int(text: str) -> int:
    assert isinstance(text, str), "value must be a string"
    assert text.strip() != "", "value must not be empty"
    return int(text)


def read_tsv(path: Path) -> list[dict[str, str]]:
    assert isinstance(path, Path), "path must be a Path"
    assert path.suffix == ".tsv", "expected a tsv file"
    if not path.is_file():
        return []
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle, delimiter=TAB)
        rows = [dict(row) for row in reader]
    return rows


@dataclass
class Point:
    workload: str
    engine: str
    threads: int
    sweep_param: str
    sweep_value: int
    wu_per_s: float
    ops_per_s: float
    hit_rate: float
    ops: dict[str, dict[str, float]] = field(default_factory=dict)


def build_points(rows: list[dict[str, str]]) -> list[Point]:
    assert isinstance(rows, list), "rows must be a list"
    assert all(COL_WORKLOAD in r for r in rows), "rows missing workload column"
    by_key: dict[tuple[str, str, int, int], Point] = {}
    count = 0
    for row in rows:
        count += 1
        assert count <= len(rows) + 1, "point build exceeded its bound"
        threads = to_int(row[COL_THREADS])
        sweep_value = to_int(row[COL_SWEEP_VALUE])
        sweep_param = row.get(COL_SWEEP_PARAM, "")
        key = (row[COL_WORKLOAD], row[COL_ENGINE], threads, sweep_value)
        point = by_key.get(key)
        if point is None:
            point = Point(row[COL_WORKLOAD], row[COL_ENGINE], threads, sweep_param, sweep_value,
                          to_float(row[COL_WU]), to_float(row[COL_OPS]),
                          to_float(row[COL_HIT]))
            by_key[key] = point
        point.ops[row[COL_OP]] = {name: to_float(row[col]) for name, col in PCTL_COLUMNS}
    return list(by_key.values())


def distinct(values: list[str]) -> list[str]:
    assert isinstance(values, list), "values must be a list"
    seen: dict[str, int] = {}
    for v in values:
        seen[v] = 1
    assert len(seen) <= len(values) + 1, "distinct produced more than its input"
    return sorted(seen)


def engines_of(points: list[Point]) -> list[str]:
    assert isinstance(points, list), "points must be a list"
    return distinct([p.engine for p in points])


def peak_threads(points: list[Point]) -> int:
    assert isinstance(points, list), "points must be a list"
    return max((p.threads for p in points), default=0)


def baseline_value(points: list[Point], workload: str) -> int:
    """The smallest swept value a workload ran at, its unbatched or smallest
    record baseline. Used to pick one representative point per workload for the
    throughput, scalability, and latency figures."""
    assert isinstance(points, list), "points must be a list"
    assert isinstance(workload, str), "workload must be a string"
    return min((p.sweep_value for p in points if p.workload == workload), default=1)


def point_value(points: list[Point], workload: str, engine: str) -> float:
    assert isinstance(workload, str), "workload must be a string"
    assert isinstance(engine, str), "engine must be a string"
    for p in points:
        if p.workload == workload and p.engine == engine:
            return p.wu_per_s
    return 0.0


def swept_workloads(points: list[Point]) -> list[str]:
    assert isinstance(points, list), "points must be a list"
    out = []
    for w in distinct([p.workload for p in points]):
        if len({p.sweep_value for p in points if p.workload == w}) > 1:
            out.append(w)
    return out


def load_palette(root: Path) -> dict[str, str]:
    assert isinstance(root, Path), "root must be a Path"
    path = root / PALETTE_FILE
    palette: dict[str, str] = {}
    if not path.is_file():
        return palette
    for line in path.read_text().splitlines():
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        palette[key.strip().lower()] = value.strip()
    return palette


def engine_color(engine: str, palette: dict[str, str]) -> Optional[str]:
    assert isinstance(engine, str), "engine must be a string"
    assert isinstance(palette, dict), "palette must be a dict"
    parts = engine.split()
    token = parts[0].lower() if parts else ""
    return palette.get(token)


def is_byte_metric(name: str) -> bool:
    assert isinstance(name, str), "name must be a string"
    assert name != "", "name must not be empty"
    return any(name.endswith(suffix) for suffix in BYTE_METRIC_SUFFIXES)


def engine_internal_metrics(rows: list[dict[str, str]], engine: str) -> list[str]:
    """Every internal metric name this engine emitted, ordered with the most
    useful first and the rest alphabetically. System and throughput metrics are
    excluded since they have their own figures. Nothing is dropped for being
    flat or zero, a constant is itself a fact worth seeing. rocksdb and tidesdb
    expose disjoint names, so this is computed per engine."""
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(engine, str), "engine must be a string"
    known = set(SYSTEM_ALL) | set(THROUGHPUT_ALL)
    names = distinct([r[COL_METRIC] for r in rows
                      if r[COL_ENGINE] == engine and r[COL_METRIC] not in known])
    ordered = [m for m in ENGINE_PREFERRED if m in names]
    ordered += sorted(m for m in names if m not in ENGINE_PREFERRED)
    return ordered[:MAX_PANELS]


def tl_series(rows: list[dict[str, str]], engine: str, workload: str,
              metric: str) -> list[tuple[float, float]]:
    """The run timeline at one operating point, the peak thread cell at the
    baseline swept value, so a swept workload does not blur several runs of very
    different rates into one jagged average."""
    assert isinstance(metric, str), "metric must be a string"
    assert isinstance(engine, str), "engine must be a string"
    picked = [r for r in rows if r[COL_ENGINE] == engine and r[COL_WORKLOAD] == workload
              and r[COL_METRIC] == metric and r.get(COL_PHASE, "run") == "run"]
    if not picked:
        return []
    threads = max(to_int(r[COL_THREADS]) for r in picked)
    sweep = min(to_int(r[COL_SWEEP_VALUE]) for r in picked)
    picked = [r for r in picked if to_int(r[COL_THREADS]) == threads
              and to_int(r[COL_SWEEP_VALUE]) == sweep]
    buckets: dict[float, list[float]] = {}
    for r in picked:
        buckets.setdefault(round(to_float(r[COL_ELAPSED]), 1), []).append(to_float(r[COL_VALUE]))
    return [(t, sum(v) / len(v)) for t, v in sorted(buckets.items())]


def seed_series(rows: list[dict[str, str]], engine: str,
                workload: str) -> list[tuple[float, float]]:
    """The ingest curve for the single richest seed, the one cell with the most
    samples, its slowest or largest. Only one cell is shown rather than several
    blended, since across a sweep the cells may load different sized data, a 256
    byte value and a one megabyte value, and averaging those seeds would be
    meaningless. Repeats of that one cell are averaged by rounded elapsed."""
    assert isinstance(engine, str), "engine must be a string"
    assert isinstance(workload, str), "workload must be a string"
    picked = [r for r in rows if r[COL_ENGINE] == engine and r[COL_WORKLOAD] == workload
              and r[COL_METRIC] == SEED_METRIC and r.get(COL_PHASE, "run") == "seed"]
    if not picked:
        return []
    counts: dict[tuple[int, int], int] = {}
    for r in picked:
        k = (to_int(r[COL_THREADS]), to_int(r[COL_SWEEP_VALUE]))
        counts[k] = counts.get(k, 0) + 1
    best = max(counts, key=lambda k: counts[k])
    picked = [r for r in picked
              if (to_int(r[COL_THREADS]), to_int(r[COL_SWEEP_VALUE])) == best]
    buckets: dict[float, list[float]] = {}
    for r in picked:
        buckets.setdefault(round(to_float(r[COL_ELAPSED]), 1), []).append(to_float(r[COL_VALUE]))
    return [(t, sum(v) / len(v)) for t, v in sorted(buckets.items())]


def make_grid(npanels: int, title: str) -> tuple[object, list[object]]:
    assert npanels >= 1, "need at least one panel"
    assert npanels <= MAX_PANELS, "too many panels for one figure"
    cols = 1 if npanels == 1 else 2
    rows = (npanels + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(PANEL_W * cols, PANEL_H * rows),
                             squeeze=False)
    flat = [axes[i // cols][i % cols] for i in range(rows * cols)]
    for extra in range(npanels, len(flat)):
        flat[extra].axis("off")
    fig.suptitle(title)
    return fig, flat[:npanels]


def save(fig: object, outdir: Path, stem: str, ext: str) -> Path:
    assert isinstance(outdir, Path), "outdir must be a Path"
    assert ext in FIG_FORMATS, "unsupported figure format"
    outdir.mkdir(parents=True, exist_ok=True)
    path = outdir / f"{stem}.{ext}"
    fig.tight_layout(rect=(0, 0, 1, 0.97))
    fig.savefig(path, dpi=FIG_DPI)
    plt.close(fig)
    return path


def label_xticks(ax: object, values: list[int]) -> None:
    """Mark the x axis at exactly the values the run used, the thread counts or
    swept sizes, so the axis reads 1 8 16 plainly instead of whatever ticks a log
    scale would invent."""
    assert isinstance(values, list), "values must be a list"
    vals = sorted({int(v) for v in values})
    if not vals:
        return
    ax.set_xticks(vals)
    ax.set_xticklabels([str(v) for v in vals])
    ax.minorticks_off()


def legend_if_any(ax: object) -> None:
    """Add a legend only when the panel drew something labeled, so an empty panel
    does not warn about having no artists to put in a legend."""
    handles, _ = ax.get_legend_handles_labels()
    if handles:
        ax.legend(fontsize=LEGEND_FONTSIZE)


def plot_throughput_compare(points: list[Point], palette: dict[str, str],
                            outdir: Path, ext: str) -> Optional[Path]:
    assert isinstance(points, list), "points must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    top = peak_threads(points)
    usable = [p for p in points
              if p.threads == top and p.sweep_value == baseline_value(points, p.workload)]
    workloads = distinct([p.workload for p in usable])
    engines = engines_of(usable)
    if not workloads or not engines:
        return None
    fig, ax = plt.subplots(figsize=(max(PANEL_W, 1.5 * len(workloads)), PANEL_H))
    width = 1.0 / (len(engines) + 1)
    for idx, engine in enumerate(engines):
        ys = [point_value(usable, w, engine) for w in workloads]
        xs = [j + idx * width for j in range(len(workloads))]
        ax.bar(xs, ys, width=width, color=engine_color(engine, palette), label=engine)
    ax.set_xticks([j + width * (len(engines) - 1) / 2 for j in range(len(workloads))])
    ax.set_xticklabels(workloads)
    ax.set_ylabel("wu per s")
    ax.set_title(f"Throughput by workload at {top} threads")
    ax.grid(True, axis="y", alpha=GRID_ALPHA)
    legend_if_any(ax)
    return save(fig, outdir, "throughput_compare", ext)


def plot_scalability(points: list[Point], palette: dict[str, str], outdir: Path,
                     ext: str) -> Optional[Path]:
    assert isinstance(points, list), "points must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    usable = [p for p in points if p.sweep_value == baseline_value(points, p.workload)]
    workloads = distinct([p.workload for p in usable])
    if not workloads:
        return None
    fig, axes = make_grid(len(workloads), "Throughput vs threads")
    for ax, workload in zip(axes, workloads):
        for engine in distinct([p.engine for p in usable if p.workload == workload]):
            series = sorted((p.threads, p.wu_per_s) for p in usable
                            if p.workload == workload and p.engine == engine)
            ax.plot([t for t, _ in series], [v for _, v in series], marker="o",
                    color=engine_color(engine, palette), label=engine)
        ax.set_title(workload)
        ax.set_xscale("log", base=2)
        label_xticks(ax, [p.threads for p in usable if p.workload == workload])
        ax.set_xlabel("threads")
        ax.set_ylabel("wu per s")
        ax.grid(True, alpha=GRID_ALPHA)
        legend_if_any(ax)
    return save(fig, outdir, "scalability", ext)


def plot_latency_threads(points: list[Point], pctl: str, palette: dict[str, str],
                         outdir: Path, ext: str) -> list[Path]:
    """Latency against thread count, the latency analogue of the scalability
    figure. One figure per workload, a panel per op, x is threads, a line per
    engine, so tail latency under load reads the same way as throughput vs
    threads and is shown across the whole sweep rather than at one fixed point."""
    assert isinstance(points, list), "points must be a list"
    assert isinstance(pctl, str), "pctl must be a string"
    written: list[Path] = []
    count = 0
    for workload in distinct([p.workload for p in points]):
        count += 1
        assert count <= len(points) + 1, "latency threads loop exceeded its bound"
        wpoints = [p for p in points if p.workload == workload
                   and p.sweep_value == baseline_value(points, workload)]
        threadset = sorted({p.threads for p in wpoints})
        if len(threadset) < 2:
            continue
        ops = sorted({op for p in wpoints for op in p.ops})
        engines = engines_of(wpoints)
        if not ops or not engines:
            continue
        fig, axes = make_grid(len(ops), f"{workload}: {pctl} latency vs threads")
        for ax, op in zip(axes, ops):
            for engine in engines:
                series = sorted((p.threads, p.ops[op][pctl] / NS_PER_US) for p in wpoints
                                if p.engine == engine and op in p.ops)
                if not series:
                    continue
                ax.plot([t for t, _ in series], [v for _, v in series], marker="o",
                        color=engine_color(engine, palette), label=engine)
            ax.set_xscale("log", base=2)
            label_xticks(ax, threadset)
            ax.set_title(op)
            ax.set_xlabel("threads")
            ax.set_ylabel(f"{pctl} us")
            ax.grid(True, alpha=GRID_ALPHA)
            legend_if_any(ax)
        written.append(save(fig, outdir, f"latency_{pctl.replace('.', '')}_threads_{workload}", ext))
    return written


def plot_sweeps(points: list[Point], palette: dict[str, str], outdir: Path,
                ext: str) -> list[Path]:
    """One amortization figure per workload that sweeps a parameter, titled and
    axis labeled by whatever that workload sweeps, batch size, value bytes, or
    anything a future workload declares. Workloads that sweep nothing are skipped.
    Each swept workload gets its own figure since the parameters are not
    comparable across workloads."""
    assert isinstance(points, list), "points must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    written: list[Path] = []
    count = 0
    for workload in swept_workloads(points):
        count += 1
        assert count <= len(points) + 1, "sweep figure loop exceeded its bound"
        wpoints = [p for p in points if p.workload == workload]
        param = next((p.sweep_param for p in wpoints if p.sweep_param), "sweep")
        threadset = sorted({p.threads for p in wpoints})
        engines = engines_of(wpoints)
        fig, axes = make_grid(len(threadset), f"{workload}: ops per s vs {param}")
        for ax, thr in zip(axes, threadset):
            for engine in engines:
                series = sorted((p.sweep_value, p.ops_per_s) for p in wpoints
                                if p.threads == thr and p.engine == engine)
                if not series:
                    continue
                ax.plot([b for b, _ in series], [v for _, v in series], marker="o",
                        color=engine_color(engine, palette), label=engine)
            ax.set_xscale("log", base=2)
            label_xticks(ax, [p.sweep_value for p in wpoints])
            ax.set_title(f"{thr} threads")
            ax.set_xlabel(param)
            ax.set_ylabel("ops per s")
            ax.grid(True, alpha=GRID_ALPHA)
            legend_if_any(ax)
        written.append(save(fig, outdir, f"sweep_{workload}", ext))
    return written


def plot_timeline(rows: list[dict[str, str]], palette: dict[str, str],
                  outdir: Path, ext: str) -> Optional[Path]:
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    if not rows or THROUGHPUT_METRIC not in set(distinct([r[COL_METRIC] for r in rows])):
        return None
    workloads = distinct([r[COL_WORKLOAD] for r in rows])
    engines = distinct([r[COL_ENGINE] for r in rows])
    fig, axes = make_grid(len(workloads), "Throughput over time (peak threads)")
    for ax, workload in zip(axes, workloads):
        for engine in engines:
            series = tl_series(rows, engine, workload, THROUGHPUT_METRIC)
            if not series:
                continue
            ax.plot([t for t, _ in series], [v for _, v in series],
                    color=engine_color(engine, palette), label=engine)
        ax.set_title(workload)
        ax.set_xlabel("elapsed s")
        ax.set_ylabel("wu per s")
        ax.grid(True, alpha=GRID_ALPHA)
        legend_if_any(ax)
    return save(fig, outdir, "timeline_throughput", ext)


def plot_seed_throughput(rows: list[dict[str, str]], palette: dict[str, str],
                         outdir: Path, ext: str) -> Optional[Path]:
    """How the load fills the store over time, the cumulative keys seeded against
    elapsed seconds, one panel per workload and a line per engine. A steeper line
    is a faster ingest and the line simply ends when the seed is done, so a fast
    engine rises sharply and stops early. Cumulative rather than instantaneous
    rate so the curve is monotonic and readable instead of a noisy saw."""
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    seed_rows = [r for r in rows if r.get(COL_PHASE, "run") == "seed"]
    if not seed_rows or SEED_METRIC not in set(distinct([r[COL_METRIC] for r in seed_rows])):
        return None
    workloads = distinct([r[COL_WORKLOAD] for r in seed_rows])
    engines = distinct([r[COL_ENGINE] for r in seed_rows])
    fig, axes = make_grid(len(workloads), "Seed progress, keys loaded over time")
    for ax, workload in zip(axes, workloads):
        for engine in engines:
            series = seed_series(rows, engine, workload)
            if not series:
                continue
            ax.plot([t for t, _ in series], [v for _, v in series], marker="o",
                    color=engine_color(engine, palette), label=engine)
        ax.set_title(workload)
        ax.set_xlabel("elapsed s")
        ax.set_ylabel("keys loaded")
        ax.grid(True, alpha=GRID_ALPHA)
        legend_if_any(ax)
    return save(fig, outdir, "seed_progress", ext)


def plot_timeline_system(rows: list[dict[str, str]], palette: dict[str, str],
                         outdir: Path, ext: str) -> list[Path]:
    """One system usage figure per workload, since the system probe samples are
    tagged with the workload that was running, so a multi workload run needs a
    figure each rather than collapsing to one. A panel per metric, a line per
    engine, like the throughput timeline."""
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    if not rows:
        return []
    present = set(distinct([r[COL_METRIC] for r in rows]))
    metrics = [m for m in SYSTEM_ALL if m in present]
    if not metrics:
        return []
    engines = distinct([r[COL_ENGINE] for r in rows])
    written: list[Path] = []
    count = 0
    for workload in distinct([r[COL_WORKLOAD] for r in rows]):
        count += 1
        assert count <= len(rows) + 1, "timeline system loop exceeded its bound"
        fig, axes = make_grid(len(metrics), f"System usage over time ({workload}, peak threads)")
        drew = False
        for ax, metric in zip(axes, metrics):
            scale = BYTES_PER_MIB if metric == DISK_METRIC else 1.0
            for engine in engines:
                series = tl_series(rows, engine, workload, metric)
                if not series:
                    continue
                drew = True
                ax.plot([t for t, _ in series], [v / scale for _, v in series],
                        color=engine_color(engine, palette), label=engine)
            ax.set_title(metric if metric != DISK_METRIC else "disk_mib")
            ax.set_xlabel("elapsed s")
            ax.grid(True, alpha=GRID_ALPHA)
            legend_if_any(ax)
        if drew:
            written.append(save(fig, outdir, f"timeline_system_{workload}", ext))
        else:
            plt.close(fig)
    return written


def draw_engine_figure(rows: list[dict[str, str]], engine: str, workload: str,
                       palette: dict[str, str], outdir: Path, ext: str) -> Optional[Path]:
    """Render one figure of a single engine's internal metrics over time."""
    assert isinstance(engine, str), "engine must be a string"
    assert isinstance(workload, str), "workload must be a string"
    metrics = engine_internal_metrics(rows, engine)
    if not metrics:
        return None
    parts = engine.split()
    token = parts[0] if parts else engine
    color = engine_color(engine, palette)
    fig, axes = make_grid(len(metrics), f"{engine} internals over time ({workload}, peak threads)")
    idx = 0
    drew = False
    for ax, metric in zip(axes, metrics):
        idx += 1
        assert idx <= len(metrics) + 1, "panel loop exceeded its bound"
        scale = BYTES_PER_MIB if is_byte_metric(metric) else 1.0
        series = tl_series(rows, engine, workload, metric)
        if series:
            drew = True
            ax.plot([t for t, _ in series], [v / scale for _, v in series],
                    color=color, label=engine)
        ax.set_title(f"{metric} mib" if is_byte_metric(metric) else metric)
        ax.set_xlabel("elapsed s")
        ax.grid(True, alpha=GRID_ALPHA)
        legend_if_any(ax)
    if not drew:
        plt.close(fig)
        return None
    return save(fig, outdir, f"engine_stats_{token}_{workload}", ext)


def plot_engine_stats(rows: list[dict[str, str]], palette: dict[str, str],
                      outdir: Path, ext: str) -> list[Path]:
    """One engine internals figure per engine and workload, since rocksdb and
    tidesdb report disjoint metric names and so never share a panel, and the system
    probe tags each sample with the workload that was running, so every workload
    gets its own figure rather than collapsing to one."""
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    if not rows:
        return []
    written: list[Path] = []
    engines = distinct([r[COL_ENGINE] for r in rows])
    workloads = distinct([r[COL_WORKLOAD] for r in rows])
    count = 0
    for workload in workloads:
        for engine in engines:
            count += 1
            assert count <= len(workloads) * len(engines) + 1, "engine loop exceeded its bound"
            path = draw_engine_figure(rows, engine, workload, palette, outdir, ext)
            if path is not None:
                written.append(path)
    return written


def gather(dirs: list[Path]) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    assert isinstance(dirs, list), "dirs must be a list"
    assert len(dirs) <= MAX_DIRS, "too many result directories"
    points: list[dict[str, str]] = []
    timeline: list[dict[str, str]] = []
    count = 0
    for d in dirs:
        count += 1
        assert count <= MAX_DIRS, "directory scan exceeded its bound"
        points.extend(read_tsv(d / POINTS_FILE))
        timeline.extend(read_tsv(d / TIMELINE_FILE))
    return points, timeline


def main() -> int:
    parser = argparse.ArgumentParser(description="plot keybench results")
    parser.add_argument("dirs", nargs="+", help="result directories")
    parser.add_argument("--out", default="figures", help="output directory")
    parser.add_argument("--format", default=DEFAULT_FORMAT, choices=FIG_FORMATS,
                        help="figure file format (default png)")
    ns = parser.parse_args()
    dirs = [Path(d) for d in ns.dirs]
    outdir = Path(ns.out)
    ext = ns.format
    point_rows, timeline_rows = gather(dirs)
    if not point_rows:
        print("no points.tsv rows found")
        return 1
    points = build_points(point_rows)
    palette = load_palette(Path(__file__).resolve().parent.parent)
    results = [
        plot_throughput_compare(points, palette, outdir, ext),
        plot_scalability(points, palette, outdir, ext),
        plot_timeline(timeline_rows, palette, outdir, ext),
        plot_seed_throughput(timeline_rows, palette, outdir, ext),
    ]
    results.extend(plot_timeline_system(timeline_rows, palette, outdir, ext))
    results.extend(plot_sweeps(points, palette, outdir, ext))
    results.extend(plot_engine_stats(timeline_rows, palette, outdir, ext))
    for pctl in COMPARE_PCTLS:
        results.extend(plot_latency_threads(points, pctl, palette, outdir, ext))
    for result in results:
        if result is not None:
            print(f"wrote {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
