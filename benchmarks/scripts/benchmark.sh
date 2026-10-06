#!/bin/sh
# Runs bin/benchmarkDraw and saves its results in bin, named after the system, the compiler and the processor.
#
# Usage: benchmark.sh [benchmarkDraw arguments...]
# Build benchmarkDraw first, in Release, with -DFONTRENDERER_BUILD_BENCHMARKS=ON. The script already adds --csv.

scripts=$(cd "$(dirname "$0")" && pwd)
cd "$scripts/../../bin" || exit 1
if [ ! -x ./benchmarkDraw ]; then
    echo "There is no bin/benchmarkDraw. Build it first, with -DFONTRENDERER_BUILD_BENCHMARKS=ON." >&2
    exit 1
fi

name=$(./benchmarkDraw --name) || exit 1

# A Mac sleeps during a long run unless a program asks it not to.
awake=
if command -v caffeinate >/dev/null 2>&1; then
    awake="caffeinate -i"
fi

echo "Running the benchmark. It writes bin/benchmark_$name.txt when it ends."
if ! $awake ./benchmarkDraw --csv "benchmark_$name.csv" "$@" > "benchmark_$name.txt"; then
    cat "benchmark_$name.txt"
    exit 1
fi

python=python3
if ! command -v python3 >/dev/null 2>&1; then
    python=python
fi
"$python" "$scripts/plot_benchmark.py" "benchmark_$name.csv" -o "$name.html"
