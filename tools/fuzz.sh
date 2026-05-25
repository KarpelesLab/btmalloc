#!/usr/bin/env bash
#
# fuzz.sh — coverage-guided fuzzing of btmalloc with clang's libFuzzer + ASan.
#
# Compiles the allocator core and tests/fuzz_alloc.c with
# -fsanitize=fuzzer,address and runs libFuzzer. The shadow-tag oracle in
# fuzz_alloc.c catches allocator-level corruption (overlapping/aliased memory,
# clobbered live objects); ASan catches harness/global memory errors; coverage
# feedback drives execution through btmalloc's code paths.
#
# Usage:
#   tools/fuzz.sh [seconds] [mode]
#     seconds : wall-clock budget per run (default 60)
#     mode    : default | intern | mesh | harden  (default: default)
#
# Reproduce a crash:  ./build-fuzz/fuzz_<mode> <crash-file>
# (or replay with the standalone build: build/tests/fuzz_alloc <crash-file>)

set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
secs="${1:-60}"
mode="${2:-default}"

command -v clang >/dev/null || { echo "clang required for libFuzzer"; exit 1; }

out="$root/build-fuzz"
mkdir -p "$out" "$out/corpus_$mode"

# liburing for the async backing store; libdl for the heap profiler's dladdr.
uring_flags="$(pkg-config --cflags --libs liburing 2>/dev/null || echo -luring)"

# Core sources only — NOT src/preload.c. preload.c defines malloc/free/etc.,
# which would collide with ASan's own malloc interception and crash the fuzzer.
# The harness calls the btm_* API directly.
core_srcs=(btmalloc size_class chunk partition tcache large background)
src_list=()
for s in "${core_srcs[@]}"; do src_list+=("$root/src/$s.c"); done

echo ">> compiling fuzzer ($mode)"
clang -O1 -g -fsanitize=fuzzer,address -fno-omit-frame-pointer \
    -D_GNU_SOURCE -DBTM_LIBFUZZER \
    -I"$root/include" \
    "${src_list[@]}" "$root/tests/fuzz_alloc.c" \
    $uring_flags -ldl -lpthread \
    -o "$out/fuzz_$mode"

# Per-mode env (read by btmalloc at init).
declare -a env=()
case "$mode" in
    intern) env=(BTM_PARTITION_MODE=intern) ;;
    mesh)   env=(BTM_MESH=1) ;;
    harden) env=(BTM_HARDEN=1) ;;
esac

echo ">> fuzzing $mode for ${secs}s (corpus: $out/corpus_$mode)"
env "${env[@]}" "$out/fuzz_$mode" \
    -max_total_time="$secs" -rss_limit_mb=4096 -print_final_stats=1 \
    "$out/corpus_$mode"
