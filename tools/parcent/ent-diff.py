#!/usr/bin/env python3
"""Differential tester: drive `ent` and `parcent` over the same inputs and
diff the results semantically.

For every input file this checks:

  1. ent agreement   — bytes, order-0 entropy, chi-square, mean, Monte Carlo
                       pi, and serial correlation from `ent -t` must match
                       parcent's JSON output within print-precision tolerances.
                       The chi-square p is parsed from ent's verbose output
                       ("would exceed this value X percent") and compared,
                       honoring ent's "less than 0.01" / "more than 99.99"
                       clamps. Undefined values must agree: ent's -100000
                       serial-correlation sentinel and nan Monte Carlo map to
                       parcent's null.
  2. thread invariance — parcent -t 1 and parcent -t N must produce identical
                       JSON (merge must reconstitute exact single-pass state).
  3. stream invariance — parcent reading the file via stdin must match the
                       mmap'd run on every field except lz_probe (documented
                       as unavailable when streaming).

With no FILE arguments a deterministic suite of synthetic inputs (edge sizes,
constants, ramps, uniform/biased random, text-like, multi-MiB for the
threaded path) is generated in a temp dir. Extra files (e.g. corpus/) can be
passed as arguments and are checked in addition.

Exit status: 0 if all checks pass, 1 otherwise.
"""

import argparse
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

SCC_UNDEF = -100000.0  # ent's "undefined serial correlation" sentinel

# ent -t prints 6 decimal places; allow the quantization step plus slack.
ABS_TOL = 2e-6
CHI2_REL_TOL = 1e-9  # chi-square can be huge; add a relative component
# ent's p comes from an approximation (Wilson-Hilferty style) and is printed
# as a percentage with 2 decimals; compare in percent points.
P_TOL_PCT = 0.1


def run(cmd, stdin=None):
    r = subprocess.run(cmd, stdin=stdin, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if r.returncode != 0:
        raise RuntimeError("%s failed (rc=%d): %s"
                           % (cmd[0], r.returncode, r.stderr.decode().strip()))
    return r.stdout.decode()


def ent_terse(ent, path):
    """Parse `ent -t`: header line then '1,bytes,entropy,chi2,mean,pi,scc'."""
    out = run([ent, "-t", path])
    row = None
    for line in out.splitlines():
        if line.startswith("1,"):
            row = line.split(",")
    if row is None or len(row) != 7:
        raise RuntimeError("unparseable ent -t output for %s: %r" % (path, out))
    return {
        "bytes": int(row[1]),
        "entropy_o0": float(row[2]),
        "chi2": float(row[3]),
        "mean": float(row[4]),
        "montecarlo_pi": float(row[5]),
        "serial_corr": float(row[6]),
    }


def ent_chi2_p(ent, path):
    """Parse the verbose chi-square sentence. Returns (op, percent) with op
    one of '<', '>', '=' for the "less than" / "more than" / exact forms,
    or None when ent omits the line (empty input)."""
    out = run([ent, path])
    m = re.search(r"would exceed this value (?:(less than|more than(?: than)?) )?"
                  r"([0-9.]+) percent", out)
    if m is None:
        return None
    op = {"less than": "<", "more than": ">", "more than than": ">",
          None: "="}[m.group(1)]
    return (op, float(m.group(2)))


def parcent_json(parcent, args, path=None, stdin_from=None):
    cmd = [parcent, "-j"] + args + ([path] if path is not None else [])
    if stdin_from is not None:
        with open(stdin_from, "rb") as f:
            out = run(cmd, stdin=f)
    else:
        out = run(cmd)
    reports = json.loads(out)
    if len(reports) != 1:
        raise RuntimeError("expected 1 report, got %d" % len(reports))
    return reports[0]


def is_undef(v):
    if v is None:
        return True
    if isinstance(v, float) and math.isnan(v):
        return True
    return False


def close(a, b, abs_tol=ABS_TOL, rel_tol=0.0):
    return abs(a - b) <= abs_tol + rel_tol * abs(b)


class Checker:
    def __init__(self):
        self.failures = []
        self.checked = 0

    def check(self, name, field, ok, detail):
        self.checked += 1
        if not ok:
            self.failures.append((name, field, detail))
            print("FAIL %-28s %-22s %s" % (name, field, detail))

    def agree(self, name, field, ent_v, parc_v, **tol):
        eu, pu = is_undef(ent_v), is_undef(parc_v)
        if field == "serial_corr" and ent_v == SCC_UNDEF:
            eu = True
        if eu or pu:
            self.check(name, field, eu == pu,
                       "ent=%r parcent=%r (undefined mismatch)" % (ent_v, parc_v))
            return
        self.check(name, field, close(ent_v, parc_v, **tol),
                   "ent=%.9g parcent=%.9g diff=%.3g"
                   % (ent_v, parc_v, abs(ent_v - parc_v)))


def check_file(chk, name, path, ent, parcent, threads):
    e = ent_terse(ent, path)
    p = parcent_json(parcent, [], path)

    chk.check(name, "bytes", e["bytes"] == p["bytes"],
              "ent=%d parcent=%d" % (e["bytes"], p["bytes"]))

    if e["bytes"] == 0:
        # ent reports nan chi2/mean and the scc sentinel for empty input;
        # parcent defines them as 0/1.0/null. Only entropy is comparable.
        chk.agree(name, "entropy_o0", e["entropy_o0"], p["entropy_o0"])
        return

    chk.agree(name, "entropy_o0", e["entropy_o0"], p["entropy_o0"])
    chk.agree(name, "chi2", e["chi2"], p["chi2"], rel_tol=CHI2_REL_TOL)
    chk.agree(name, "mean", e["mean"], p["mean"])
    chk.agree(name, "montecarlo_pi", e["montecarlo_pi"], p["montecarlo_pi"])
    chk.agree(name, "serial_corr", e["serial_corr"], p["serial_corr"])

    ep = ent_chi2_p(ent, path)
    if ep is not None and p["chi2_p"] is not None:
        op, pct = ep
        pp = p["chi2_p"] * 100.0
        if op == "<":
            ok = pp <= pct + P_TOL_PCT
        elif op == ">":
            ok = pp >= pct - P_TOL_PCT
        else:
            ok = abs(pp - pct) <= P_TOL_PCT
        chk.check(name, "chi2_p", ok,
                  "ent: %s %.4g%%  parcent: %.6g%%" % (op, pct, pp))

    # Thread invariance: -t 1 vs -t N must be bit-identical.
    p1 = parcent_json(parcent, ["-t", "1"], path)
    pn = parcent_json(parcent, ["-t", str(threads)], path)
    for k in sorted(set(p1) | set(pn)):
        if k == "file":
            continue
        chk.check(name, "threads[%s]" % k, p1.get(k) == pn.get(k),
                  "-t1=%r -t%d=%r" % (p1.get(k), threads, pn.get(k)))

    # Stream invariance: stdin run must match the mapped run except lz_probe.
    ps = parcent_json(parcent, [], stdin_from=path)
    for k in sorted(set(p) | set(ps)):
        if k in ("file", "lz_probe"):
            continue
        chk.check(name, "stream[%s]" % k, p.get(k) == ps.get(k),
                  "mapped=%r stream=%r" % (p.get(k), ps.get(k)))


def synth_suite(dirpath):
    """Deterministic synthetic inputs covering the edge cases that matter:
    tiny sizes around the 6-byte Monte Carlo group and 2-byte pair minimums,
    constant/degenerate data, exact-uniform data (chi2 = 0), skewed data,
    and multi-MiB buffers that engage the worker-thread path."""
    rng = random.Random(0x9E3779B9)
    files = {}

    def add(name, data):
        p = os.path.join(dirpath, name)
        with open(p, "wb") as f:
            f.write(data)
        files[name] = p

    add("empty", b"")
    for n in range(1, 14):
        add("tiny-%02d" % n, bytes(rng.randrange(256) for _ in range(n)))
    add("const-00-4k", b"\x00" * 4096)
    add("const-ff-4k", b"\xff" * 4096)
    add("const-aa-7", b"\xaa" * 7)
    add("alt-ab-64k", b"ab" * 32768)
    add("ramp-64k", bytes(range(256)) * 256)          # chi2 == 0, p -> 1
    add("all-bytes-once", bytes(range(256)))
    add("uniform-64k", rng.randbytes(65536))
    add("uniform-1M-plus-5", rng.randbytes((1 << 20) + 5))
    add("uniform-5M", rng.randbytes(5 << 20))          # threaded path
    add("biased", bytes(rng.choices(range(256),
                                    weights=[256 - i for i in range(256)],
                                    k=131072)))
    add("runs", b"".join(bytes([rng.randrange(256)]) * rng.randrange(1, 64)
                         for _ in range(4096)))
    add("repeat-block-3M", rng.randbytes(1024) * 3072)  # threaded + compressible
    words = ["the", "quick", "entropy", "of", "a", "stream", "parc", "codec"]
    add("text", (" ".join(rng.choices(words, k=30000)) + "\n").encode())
    return files


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_parcent = os.path.normpath(
        os.path.join(here, "..", "..", "build", "tools", "parcent", "parcent"))

    ap = argparse.ArgumentParser(
        description="semantic diff of ent vs parcent over test inputs")
    ap.add_argument("files", nargs="*",
                    help="extra files to check (in addition to, or with "
                         "--no-synth instead of, the synthetic suite)")
    ap.add_argument("--parcent", default=default_parcent)
    ap.add_argument("--ent", default=shutil.which("ent") or "ent")
    ap.add_argument("--threads", type=int, default=os.cpu_count() or 4,
                    help="thread count for the -tN invariance run")
    ap.add_argument("--no-synth", action="store_true",
                    help="skip the generated suite, only check FILE args")
    ap.add_argument("--keep", metavar="DIR",
                    help="write synthetic inputs to DIR and keep them")
    args = ap.parse_args()

    if not os.access(args.parcent, os.X_OK):
        sys.exit("parcent binary not found/executable: %s" % args.parcent)

    chk = Checker()
    tmpdir = None
    try:
        targets = []
        if not args.no_synth:
            d = args.keep or (tmpdir := tempfile.mkdtemp(prefix="ent-diff-"))
            os.makedirs(d, exist_ok=True)
            targets += sorted(synth_suite(d).items())
        targets += [(f, f) for f in args.files]
        if not targets:
            sys.exit("nothing to check")

        for name, path in targets:
            try:
                check_file(chk, name, path, args.ent, args.parcent,
                           args.threads)
            except (RuntimeError, OSError, json.JSONDecodeError) as ex:
                chk.check(name, "(run)", False, str(ex))
    finally:
        if tmpdir is not None:
            shutil.rmtree(tmpdir, ignore_errors=True)

    n_files = len(targets)
    if chk.failures:
        print("\n%d/%d checks failed across %d files"
              % (len(chk.failures), chk.checked, n_files))
        return 1
    print("all %d checks passed across %d files" % (chk.checked, n_files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
