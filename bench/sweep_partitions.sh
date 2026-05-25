#!/usr/bin/env bash
#
# sweep_partitions.sh — measure peak RSS and wall time of a real, multi-call-site
# workload (python) across btmalloc's partition count P, vs glibc.
#
# This validates the central design knob: P trades a small amount of peak
# footprint (more partitions => more minimum per-pool overhead) for finer
# call-site cohorting and segregation. Note that btmalloc's *reclamation* win
# (returning memory after a burst) is measured separately by bench_rss; this
# script measures PEAK footprint of a held working set, where the partition
# overhead is a small net cost.
#
# Usage: bench/sweep_partitions.sh [build_dir]

set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="${1:-$here/../build}"
so="$build/libbtmalloc.so"

if ! command -v python3 >/dev/null; then echo "python3 required"; exit 1; fi
[[ -e "$so" ]] || { echo "build $so first (-DBTM_BUILD_BENCH not required)"; exit 1; }

wl=$(mktemp --suffix=.py)
cat > "$wl" <<'PY'
import random, json, resource
random.seed(1)
def dicts(): return [{str(i): i*i for i in range(200)} for _ in range(3000)]
def lists(): return [[random.random() for _ in range(100)] for _ in range(3000)]
def strs():  return [("x"*random.randint(1,300)) for _ in range(20000)]
pool=[]
for _ in range(40):
    pool.append((dicts(), lists(), strs(), [json.dumps(d) for d in dicts()]))
    if len(pool) > 3: pool.pop(0)
print("MAXRSS_KB", resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
PY

run() { # label  env...
    local label="$1"; shift
    local t0 t1 out
    t0=$(date +%s.%N)
    out=$(env "$@" python3 "$wl" 2>/dev/null | awk '/MAXRSS_KB/{print $2}')
    t1=$(date +%s.%N)
    printf "%-14s %8.1f MB   %.2fs\n" "$label" \
        "$(awk "BEGIN{print $out/1024}")" "$(awk "BEGIN{print $t1-$t0}")"
}

echo "config           peak_RSS     wall"
run "glibc"
for P in 1 8 64 256 1024; do
    run "btmalloc P=$P" "BTM_PARTITIONS=$P" "LD_PRELOAD=$so"
done
rm -f "$wl"
