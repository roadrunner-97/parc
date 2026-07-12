#!/usr/bin/env python3
"""Plot encode/decode speed vs compression ratio for parc and the baseline
codecs, as a labelled scatter — one point per fixed-level codec (lz4, zlib,
zstd) and one point per parc level, so parc's ratio/speed ladder can be read
against the reference points visually.

Data comes from the google-benchmark corpus harness (bench/bench_corpus.cpp),
which already times every corpus file through every codec and records
bytes_per_second (of uncompressed data, both directions) plus a `ratio`
counter. This script aggregates those per-file numbers into one point per
codec and draws the scatter; it does not measure anything itself.

Usage:
  tools/plot_bench.py                 # newest bench/results/*.json
  tools/plot_bench.py --json R.json   # a specific archived result
  tools/plot_bench.py --run [--filter REGEX] [--build DIR]   # measure fresh
  tools/plot_bench.py --single-thread # plot the 1-thread parc ladder instead

parc is plotted as actually run — its nproc-threaded level ladder (ratio is
thread-independent, so only the speed axis is affected); baselines are the
single-threaded reference codecs. Outputs one PNG per corpus group plus an
"all" aggregate under docs/benchmarks/.
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Categorical palette (dataviz skill reference instance, light surface).
# Assigned in fixed slot order to families; validated CVD-safe as a set.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e6e5e2"
FAMILY_COLOR = {
    "lz4": "#2a78d6",      # slot 1 blue
    "zlib": "#1baf7a",     # slot 2 aqua
    "zstd": "#eda100",     # slot 3 yellow
    "parc-v1": "#4a3aa7",  # slot 5 violet
}
# Draw order / legend order: baselines first, parc last (on top).
FAMILY_ORDER = ["lz4", "zlib", "zstd", "parc-v1"]

PARC_RE = re.compile(
    r"^parc-(?P<fmt>[01])(?:-(?P<mt>mt))?(?:-L(?P<level>\d+))?(?:-t(?P<thr>\d+))?$")
BASELINE_RE = re.compile(r"^(?P<name>[a-z0-9]+?)(?:-(?P<level>\d+))?$")


def classify(codec):
    """Map a benchmark codec name to (family, level, mt, threads, point_label).

    `mt` is True for the nproc-threaded parc ladder (parc-*-mt*). level is None
    for codecs with no exposed level knob. Returns None for names to ignore.
    """
    m = PARC_RE.match(codec)
    if m:
        fmt = m.group("fmt")
        level = int(m.group("level")) if m.group("level") else 3  # default level
        thr = int(m.group("thr")) if m.group("thr") else 1
        mt = m.group("mt") is not None
        return (f"parc-v{fmt}", level, mt, thr, f"L{level}")
    m = BASELINE_RE.match(codec)
    if m:
        name = m.group("name")
        level = int(m.group("level")) if m.group("level") else None
        return (name, level, False, 1, codec)
    return None


def load_manifest():
    """Map "group/file" -> uncompressed byte size, for size-weighting."""
    with open(os.path.join(ROOT, "corpus", "manifest.json")) as f:
        entries = json.load(f)
    return {e["file"]: e["bytes"] for e in entries}


def newest_result():
    results = glob.glob(os.path.join(ROOT, "bench", "results", "*.json"))
    if not results:
        sys.exit("no archived results in bench/results/ — pass --json or --run")
    return max(results, key=os.path.getmtime)


def run_bench(build, filt):
    binary = os.path.join(ROOT, build, "bench", "parc_bench")
    if not os.path.exists(binary):
        sys.exit(f"{binary} not found — configure with PARC_BENCH=ON and build")
    fd, out = tempfile.mkstemp(suffix=".json", prefix="parc_bench_")
    os.close(fd)
    cmd = [binary, f"--benchmark_out={out}", "--benchmark_out_format=json"]
    if filt:
        cmd.append(f"--benchmark_filter={filt}")
    print("running:", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True)
    return out


def parse(path, sizes):
    """Return points[direction][scope][codec] = {family,level,mt,thr,label,
    orig,comp,time_s} accumulated across files, where scope is a corpus
    group name or "all"."""
    with open(path) as f:
        data = json.load(f)
    pts = {"compress": {}, "decompress": {}}
    for b in data["benchmarks"]:
        name = b["name"]
        # google-benchmark appends /real_time (or /manual_time) to UseRealTime
        # entries — the nproc parc codecs. Strip it so the name splits cleanly.
        for suffix in ("/real_time", "/manual_time"):
            if name.endswith(suffix):
                name = name[: -len(suffix)]
        parts = name.split("/")
        if len(parts) != 4:
            continue
        direction, codec, group, fname = parts
        if direction not in pts:
            continue
        info = classify(codec)
        if info is None:
            continue
        family, level, mt, thr, label = info
        key = f"{group}/{fname}"
        size = sizes.get(key)
        bps = b.get("bytes_per_second")
        ratio = b.get("ratio")
        if not size or not bps or ratio is None:
            continue
        for scope in (group, "all"):
            slot = pts[direction].setdefault(scope, {}).setdefault(
                codec,
                dict(family=family, level=level, mt=mt, thr=thr, label=label,
                     orig=0.0, comp=0.0, time_s=0.0),
            )
            slot["orig"] += size
            slot["comp"] += ratio * size
            slot["time_s"] += size / bps
    return pts


def select_series(codec_map, single_thread):
    """Pick which codecs to plot: all baselines, plus one parc level ladder
    per format. By default that ladder is the nproc-threaded (mt) one — parc
    as actually run; --single-thread picks the 1-thread ladder instead. The
    parc-*-tN thread-scaling points are never a ladder and are always dropped.
    """
    families_with_mt = {s["family"] for s in codec_map.values()
                        if s["family"].startswith("parc-v") and s["mt"]}
    out = {}
    for codec, s in codec_map.items():
        if s["family"] == "parc-v0":
            continue  # v0 is the legacy Huffman format; plot the latest only
        if not s["family"].startswith("parc-v"):
            out[codec] = s
            continue
        want_mt = (not single_thread) and s["family"] in families_with_mt
        is_ladder = s["mt"] if want_mt else (not s["mt"] and s["thr"] == 1)
        if is_ladder:
            out[codec] = s
    return out


def aggregate(codec_map):
    """codec_map -> list of point dicts with x=comp ratio, y=MB/s."""
    out = []
    for slot in codec_map.values():
        if slot["time_s"] <= 0 or slot["comp"] <= 0:
            continue
        out.append(dict(
            family=slot["family"],
            level=slot["level"],
            thr=slot["thr"],
            label=slot["label"],
            ratio=slot["orig"] / slot["comp"],           # higher = better
            mbps=(slot["orig"] / slot["time_s"]) / 1e6,  # decimal MB/s
        ))
    return out


def draw_axis(ax, points, ylabel):
    ax.set_facecolor(SURFACE)
    ax.set_yscale("log")
    ax.grid(True, which="both", color=GRID, linewidth=0.6, zorder=0)
    for s in ax.spines.values():
        s.set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8)

    by_family = {}
    for p in points:
        by_family.setdefault(p["family"], []).append(p)

    for family in FAMILY_ORDER:
        pts = by_family.get(family)
        if not pts:
            continue
        color = FAMILY_COLOR[family]
        # connect a codec's level ladder so it reads as one curve
        laddered = sorted((p for p in pts if p["level"] is not None),
                          key=lambda p: p["level"])
        if len(laddered) > 1:
            ax.plot([p["ratio"] for p in laddered], [p["mbps"] for p in laddered],
                    color=color, linewidth=1.4, alpha=0.45, zorder=2)
        for p in pts:
            ax.scatter(p["ratio"], p["mbps"], s=70, color=color,
                       edgecolor=SURFACE, linewidth=1.2, zorder=4)
            ax.annotate(p["label"], (p["ratio"], p["mbps"]),
                        textcoords="offset points", xytext=(6, 4),
                        fontsize=7, color=INK, zorder=5)

    ax.set_ylabel(ylabel, color=INK, fontsize=9)


def make_figure(pts_enc, pts_dec, scope, out_path, source_label, single_thread):
    enc = aggregate(select_series(pts_enc.get(scope, {}), single_thread))
    dec = aggregate(select_series(pts_dec.get(scope, {}), single_thread))
    if not enc and not dec:
        return False

    fig, (ax_e, ax_d) = plt.subplots(2, 1, figsize=(8.5, 9), sharex=True)
    fig.patch.set_facecolor(SURFACE)
    draw_axis(ax_e, enc, "encode  MB/s")
    draw_axis(ax_d, dec, "decode  MB/s")
    ax_d.set_xlabel("compression ratio  (original ÷ compressed — higher is better)",
                    color=INK, fontsize=9)

    handles = [plt.Line2D([0], [0], marker="o", linestyle="",
                          markerfacecolor=FAMILY_COLOR[f], markeredgecolor=SURFACE,
                          markersize=8, label=f)
               for f in FAMILY_ORDER]
    ax_e.legend(handles=handles, loc="best", fontsize=8, framealpha=0.9,
                facecolor=SURFACE, edgecolor=GRID)

    title = "speed vs compression ratio" + (f" — {scope}" if scope != "all" else " — full corpus")
    fig.suptitle(title, color=INK, fontsize=13, fontweight="bold", x=0.02, ha="left")
    threads = "1 thread" if single_thread else "nproc threads"
    fig.text(0.02, 0.005, f"{source_label}   ·   parc: {threads}; baselines: 1 thread",
             color=INK2, fontsize=7, ha="left")
    fig.tight_layout(rect=(0, 0.02, 1, 0.97))
    fig.savefig(out_path, dpi=130, facecolor=SURFACE)
    plt.close(fig)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", help="benchmark result JSON (default: newest in bench/results)")
    ap.add_argument("--run", action="store_true", help="measure fresh via the bench binary")
    ap.add_argument("--build", default="build-release", help="build dir for --run")
    ap.add_argument("--filter", help="benchmark_filter regex passed through with --run")
    ap.add_argument("--single-thread", action="store_true",
                    help="plot the 1-thread parc ladder instead of the nproc (mt) one")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "benchmarks"), help="output dir")
    args = ap.parse_args()

    if args.run:
        path = run_bench(args.build, args.filter)
        source = f"source: fresh run ({args.build}/bench/parc_bench)"
    else:
        path = args.json or newest_result()
        source = f"source: {os.path.relpath(path, ROOT)}"

    sizes = load_manifest()
    pts = parse(path, sizes)

    os.makedirs(args.out, exist_ok=True)
    scopes = sorted(set(pts["compress"]) | set(pts["decompress"]))
    # emit "all" first, then each corpus group
    scopes = (["all"] if "all" in scopes else []) + [s for s in scopes if s != "all"]

    written = []
    for scope in scopes:
        out_path = os.path.join(args.out, f"speed_vs_ratio_{scope}.png")
        if make_figure(pts["compress"], pts["decompress"], scope, out_path,
                       source, args.single_thread):
            written.append(out_path)

    for w in written:
        print(os.path.relpath(w, ROOT))


if __name__ == "__main__":
    main()
