#!/usr/bin/env bash
# Run the corpus baseline benchmarks and archive JSON results under
# bench/results/, named by the current commit (suffixed -dirty if the
# tree has uncommitted changes). Extra arguments are passed through to
# the benchmark binary (e.g. --benchmark_filter='zstd.*/silesia').
#
# Usage: tools/run_bench.sh [--build DIR] [benchmark args...]
set -euo pipefail
cd "$(dirname "$0")/.."

build=build-release
if [[ "${1:-}" == "--build" ]]; then
    build=$2
    shift 2
fi

rev=$(git rev-parse --short HEAD)
git diff --quiet || rev="${rev}-dirty"
out="bench/results/${rev}.json"

mkdir -p bench/results
"$build/bench/parc_bench" \
    --benchmark_out="$out" --benchmark_out_format=json "$@"
echo "archived: $out"
