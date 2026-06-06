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
COL_BATCH = "batch"
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
    batch: int
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
        batch = to_int(row[COL_BATCH])
        key = (row[COL_WORKLOAD], row[COL_ENGINE], threads, batch)
        point = by_key.get(key)
        if point is None:
            point = Point(row[COL_WORKLOAD], row[COL_ENGINE], threads, batch,
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


def baseline_batch(points: list[Point]) -> int:
    assert isinstance(points, list), "points must be a list"
    return min((p.batch for p in points), default=1)


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
        if len({p.batch for p in points if p.workload == w}) > 1:
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


def representative_workload(rows: list[dict[str, str]]) -> str:
    assert isinstance(rows, list), "rows must be a list"
    present = set(distinct([r[COL_WORKLOAD] for r in rows]))
    for w in REP_WORKLOAD_PREF:
        if w in present:
            return w
    return next(iter(present), "")


def tl_series(rows: list[dict[str, str]], engine: str, workload: str,
              metric: str) -> list[tuple[float, float]]:
    assert isinstance(metric, str), "metric must be a string"
    assert isinstance(engine, str), "engine must be a string"
    picked = [r for r in rows if r[COL_ENGINE] == engine and r[COL_WORKLOAD] == workload
              and r[COL_METRIC] == metric]
    if not picked:
        return []
    threads = max(to_int(r[COL_THREADS]) for r in picked)
    buckets: dict[float, list[float]] = {}
    for r in picked:
        if to_int(r[COL_THREADS]) != threads:
            continue
        buckets.setdefault(to_float(r[COL_ELAPSED]), []).append(to_float(r[COL_VALUE]))
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


def plot_throughput_compare(points: list[Point], palette: dict[str, str],
                            outdir: Path, ext: str) -> Optional[Path]:
    assert isinstance(points, list), "points must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    top = peak_threads(points)
    usable = [p for p in points if p.threads == top and p.batch == baseline_batch(points)]
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
    ax.legend(fontsize=LEGEND_FONTSIZE)
    return save(fig, outdir, "throughput_compare", ext)


def plot_scalability(points: list[Point], palette: dict[str, str], outdir: Path,
                     ext: str) -> Optional[Path]:
    assert isinstance(points, list), "points must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    usable = [p for p in points if p.batch == baseline_batch(points)]
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
        ax.set_xlabel("threads")
        ax.set_ylabel("wu per s")
        ax.grid(True, alpha=GRID_ALPHA)
        ax.legend(fontsize=LEGEND_FONTSIZE)
    return save(fig, outdir, "scalability", ext)


def draw_op_compare(ax: object, points: list[Point], workload: str, ops: list[str],
                    engines: list[str], pctl: str, palette: dict[str, str]) -> None:
    assert isinstance(ops, list), "ops must be a list"
    assert isinstance(engines, list), "engines must be a list"
    if not ops:
        return
    width = 1.0 / (len(engines) + 1)
    for idx, engine in enumerate(engines):
        pt = next((p for p in points if p.workload == workload and p.engine == engine), None)
        ys = [(pt.ops[op][pctl] / NS_PER_US if pt and op in pt.ops else 0.0) for op in ops]
        xs = [j + idx * width for j in range(len(ops))]
        ax.bar(xs, ys, width=width, color=engine_color(engine, palette), label=engine)
    ax.set_xticks([j + width * (len(engines) - 1) / 2 for j in range(len(ops))])
    ax.set_xticklabels(ops)
    ax.set_ylabel(f"{pctl} us")
    ax.grid(True, axis="y", alpha=GRID_ALPHA)
    ax.legend(fontsize=LEGEND_FONTSIZE)


def plot_latency_compare(points: list[Point], pctl: str, palette: dict[str, str],
                         outdir: Path, ext: str) -> Optional[Path]:
    assert isinstance(points, list), "points must be a list"
    assert isinstance(pctl, str), "pctl must be a string"
    top = peak_threads(points)
    chosen = [p for p in points if p.threads == top and p.batch == baseline_batch(points)]
    workloads = distinct([p.workload for p in chosen])
    engines = engines_of(chosen)
    if not workloads or not engines:
        return None
    fig, axes = make_grid(len(workloads), f"{pctl} latency by op at {top} threads")
    for ax, workload in zip(axes, workloads):
        ops = sorted({op for p in chosen if p.workload == workload for op in p.ops})
        draw_op_compare(ax, chosen, workload, ops, engines, pctl, palette)
        ax.set_title(workload)
    return save(fig, outdir, f"latency_{pctl.replace('.', '')}_compare", ext)


def plot_batch(points: list[Point], palette: dict[str, str], outdir: Path,
               ext: str) -> Optional[Path]:
    assert isinstance(points, list), "points must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    swept = swept_workloads(points)
    if not swept:
        return None
    bpoints = [p for p in points if p.workload in swept]
    threadset = sorted({p.threads for p in bpoints})
    engines = engines_of(bpoints)
    fig, axes = make_grid(len(threadset), "Batch amortization (ops per s)")
    for ax, thr in zip(axes, threadset):
        for engine in engines:
            series = sorted((p.batch, p.ops_per_s) for p in bpoints
                            if p.threads == thr and p.engine == engine)
            if not series:
                continue
            ax.plot([b for b, _ in series], [v for _, v in series], marker="o",
                    color=engine_color(engine, palette), label=engine)
        ax.set_xscale("log", base=2)
        ax.set_title(f"{thr} threads")
        ax.set_xlabel("batch size")
        ax.set_ylabel("ops per s")
        ax.grid(True, alpha=GRID_ALPHA)
        ax.legend(fontsize=LEGEND_FONTSIZE)
    return save(fig, outdir, "batch_amortization", ext)


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
        ax.legend(fontsize=LEGEND_FONTSIZE)
    return save(fig, outdir, "timeline_throughput", ext)


def plot_timeline_system(rows: list[dict[str, str]], palette: dict[str, str],
                         outdir: Path, ext: str) -> Optional[Path]:
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    if not rows:
        return None
    present = set(distinct([r[COL_METRIC] for r in rows]))
    metrics = [m for m in SYSTEM_ALL if m in present]
    if not metrics:
        return None
    workload = representative_workload(rows)
    engines = distinct([r[COL_ENGINE] for r in rows])
    fig, axes = make_grid(len(metrics), f"System usage over time ({workload}, peak threads)")
    for ax, metric in zip(axes, metrics):
        scale = BYTES_PER_MIB if metric == DISK_METRIC else 1.0
        for engine in engines:
            series = tl_series(rows, engine, workload, metric)
            if not series:
                continue
            ax.plot([t for t, _ in series], [v / scale for _, v in series],
                    color=engine_color(engine, palette), label=engine)
        ax.set_title(metric if metric != DISK_METRIC else "disk_mib")
        ax.set_xlabel("elapsed s")
        ax.grid(True, alpha=GRID_ALPHA)
        ax.legend(fontsize=LEGEND_FONTSIZE)
    return save(fig, outdir, "timeline_system", ext)


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
    for ax, metric in zip(axes, metrics):
        idx += 1
        assert idx <= len(metrics) + 1, "panel loop exceeded its bound"
        scale = BYTES_PER_MIB if is_byte_metric(metric) else 1.0
        series = tl_series(rows, engine, workload, metric)
        if series:
            ax.plot([t for t, _ in series], [v / scale for _, v in series],
                    color=color, label=engine)
        ax.set_title(f"{metric} mib" if is_byte_metric(metric) else metric)
        ax.set_xlabel("elapsed s")
        ax.grid(True, alpha=GRID_ALPHA)
        ax.legend(fontsize=LEGEND_FONTSIZE)
    return save(fig, outdir, f"engine_stats_{token}", ext)


def plot_engine_stats(rows: list[dict[str, str]], palette: dict[str, str],
                      outdir: Path, ext: str) -> list[Path]:
    """One engine internals figure per engine, since rocksdb and tidesdb report
    disjoint metric names and so never share a panel. Every engine that emitted
    samples gets its own complete figure."""
    assert isinstance(rows, list), "rows must be a list"
    assert isinstance(outdir, Path), "outdir must be a Path"
    if not rows:
        return []
    workload = representative_workload(rows)
    written: list[Path] = []
    engines = distinct([r[COL_ENGINE] for r in rows])
    count = 0
    for engine in engines:
        count += 1
        assert count <= len(engines) + 1, "engine loop exceeded its bound"
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
        plot_batch(points, palette, outdir, ext),
        plot_timeline(timeline_rows, palette, outdir, ext),
        plot_timeline_system(timeline_rows, palette, outdir, ext),
    ]
    results.extend(plot_engine_stats(timeline_rows, palette, outdir, ext))
    for pctl in COMPARE_PCTLS:
        results.append(plot_latency_compare(points, pctl, palette, outdir, ext))
    for result in results:
        if result is not None:
            print(f"wrote {result}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
