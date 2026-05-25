#!/usr/bin/env bash
#
# run.sh — run the btmalloc benchmark suite under multiple allocators and emit
# a single combined CSV for comparison.
#
# Usage:
#   bench/run.sh [build_dir]
#
# Env knobs:
#   BENCH_ALLOCATORS   space-separated subset of: glibc jemalloc btmalloc
#                      (default: all that are available)
#   BENCH_SET          space-separated subset of: micro threaded rss latency
#                      (default: all)
#   BENCH_TASKSET      optional taskset cpu-list, e.g. "0-15" to pin to P-cores
#                      on the 14900K for lower variance (default: unset)
#   JEMALLOC_SO        path to libjemalloc (default: auto-detect)
#   BENCH_MICRO_ARGS / BENCH_THREADED_ARGS / BENCH_RSS_ARGS /
#   BENCH_LATENCY_ARGS optional argv passed to each benchmark, e.g.
#                      BENCH_MICRO_ARGS="200000 50000" for a fast smoke run.
#
# Output: bench/results/<utc-timestamp>.csv  and a copy at bench/results/latest.csv
# CSV columns: benchmark,allocator,param,metric,value

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
build="${1:-$root/build}"
bench_bin="$build/bench"
btmalloc_so="$build/libbtmalloc.so"

results_dir="$root/bench/results"
mkdir -p "$results_dir"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
out="$results_dir/$stamp.csv"

# Locate jemalloc / mimalloc (newest soname wins).
jemalloc_so="${JEMALLOC_SO:-}"
if [[ -z "$jemalloc_so" ]]; then
    jemalloc_so="$(ldconfig -p 2>/dev/null | awk '/libjemalloc\.so/{print $NF; exit}')" || true
fi
mimalloc_so="${MIMALLOC_SO:-}"
if [[ -z "$mimalloc_so" ]]; then
    mimalloc_so="$(ldconfig -p 2>/dev/null | awk '/libmimalloc\.so/{print $NF; exit}')" || true
fi

# Which allocators?
declare -a allocs
if [[ -n "${BENCH_ALLOCATORS:-}" ]]; then
    read -r -a allocs <<< "$BENCH_ALLOCATORS"
else
    allocs=(glibc)
    [[ -n "$jemalloc_so" && -e "$jemalloc_so" ]] && allocs+=(jemalloc)
    [[ -n "$mimalloc_so" && -e "$mimalloc_so" ]] && allocs+=(mimalloc)
    # Third-party allocators fetched by bench/fetch_allocators.sh (snmalloc,
    # tcmalloc, ffmalloc, hardened_malloc, ...): each as lib/<name>.so.
    if compgen -G "$root/bench/allocators/lib/*.so" >/dev/null 2>&1; then
        for so in "$root"/bench/allocators/lib/*.so; do
            allocs+=("$(basename "$so" .so)")
        done
    fi
    [[ -e "$btmalloc_so" ]] && allocs+=(btmalloc)
fi

# Which benchmark binaries?
declare -A bench_exe=(
    [micro]="$bench_bin/bench_micro"
    [threaded]="$bench_bin/bench_threaded"
    [rss]="$bench_bin/bench_rss"
    [latency]="$bench_bin/bench_latency"
)
declare -a sets
if [[ -n "${BENCH_SET:-}" ]]; then
    read -r -a sets <<< "$BENCH_SET"
else
    sets=(micro threaded rss latency)
fi

ts_prefix=()
[[ -n "${BENCH_TASKSET:-}" ]] && ts_prefix=(taskset -c "$BENCH_TASKSET")

preload_for() {
    case "$1" in
        glibc)    echo "" ;;
        jemalloc) echo "$jemalloc_so" ;;
        mimalloc) echo "$mimalloc_so" ;;
        btmalloc) echo "$btmalloc_so" ;;
        *)
            # Fetched third-party allocator: bench/allocators/lib/<name>.so
            local cand="$root/bench/allocators/lib/$1.so"
            [[ -e "$cand" ]] && echo "$cand" || echo ""
            ;;
    esac
}

echo "benchmark,allocator,param,metric,value" > "$out"
echo "# build=$build  allocators=${allocs[*]}  sets=${sets[*]}  taskset=${BENCH_TASKSET:-none}" >&2
echo "# jemalloc=$jemalloc_so" >&2

for set in "${sets[@]}"; do
    exe="${bench_exe[$set]:-}"
    if [[ -z "$exe" || ! -x "$exe" ]]; then
        echo "skip: $set (no binary at $exe)" >&2
        continue
    fi
    # Per-set extra argv (for fast smoke runs vs full runs).
    argvar="BENCH_$(echo "$set" | tr '[:lower:]' '[:upper:]')_ARGS"
    read -r -a extra_args <<< "${!argvar:-}"
    for a in "${allocs[@]}"; do
        pl="$(preload_for "$a")"
        echo ">> $set / $a" >&2
        if [[ -n "$pl" ]]; then
            env BTM_BENCH_LABEL="$a" LD_PRELOAD="$pl" "${ts_prefix[@]}" "$exe" "${extra_args[@]}" >> "$out" \
                || echo "!! $set/$a failed" >&2
        else
            env BTM_BENCH_LABEL="$a" "${ts_prefix[@]}" "$exe" "${extra_args[@]}" >> "$out" \
                || echo "!! $set/$a failed" >&2
        fi
    done
done

cp -f "$out" "$results_dir/latest.csv"
echo "wrote $out" >&2

# Quick human-readable pivot of a few headline metrics.
echo >&2
echo "=== headline comparison (lower=better for ns; higher=better for Mops) ===" >&2
awk -F, '
    NR==1 { next }
    /^#/  { next }
    {
        key=$1"|"$3"|"$4
        val[key","$2]=$5
        seen_alloc[$2]=1
        order[key]=1
    }
    END {
        # Stable, meaningful column order: baselines first, btmalloc last.
        n=0
        split("glibc jemalloc mimalloc btmalloc", pref, " ")
        for (i=1;i<=4;i++) if (pref[i] in seen_alloc) { allocs[n++]=pref[i]; delete seen_alloc[pref[i]] }
        for (a in seen_alloc) allocs[n++]=a
        printf "%-22s %-18s %-16s", "benchmark", "param", "metric" > "/dev/stderr"
        for (i=0;i<n;i++) printf " %12s", allocs[i] > "/dev/stderr"
        printf "\n" > "/dev/stderr"
        for (k in order) {
            split(k, f, "|")
            printf "%-22s %-18s %-16s", f[1], f[2], f[3] > "/dev/stderr"
            for (i=0;i<n;i++) {
                v=val[k","allocs[i]]
                printf " %12s", (v==""?"-":v) > "/dev/stderr"
            }
            printf "\n" > "/dev/stderr"
        }
    }
' "$out" | sort >&2 || true
