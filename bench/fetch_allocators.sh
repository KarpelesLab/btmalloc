#!/usr/bin/env bash
#
# fetch_allocators.sh — download and build third-party allocators so the
# benchmark suite can compare btmalloc against them (rather than reimplementing
# their ideas). Each allocator is cloned shallowly into bench/allocators/src/<name>,
# built, and its LD_PRELOAD shared object symlinked into bench/allocators/lib/<name>.so,
# where bench/run.sh auto-discovers it.
#
# Usage:
#   bench/fetch_allocators.sh [name ...]    # default: all known
#   FORCE=1 bench/fetch_allocators.sh       # rebuild even if lib/<name>.so exists
#
# Everything lands under bench/allocators/ (git-ignored). Each allocator is
# best-effort: a failure warns and is skipped, the rest continue. System-installed
# allocators (jemalloc, mimalloc) are found by run.sh directly and not fetched here.

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
base="$here/allocators"
src="$base/src"
lib="$base/lib"
logs="$base/logs"
mkdir -p "$src" "$lib" "$logs"

jobs="$(nproc 2>/dev/null || echo 4)"
FORCE="${FORCE:-0}"

ok=()    ; fail=()

note() { printf '  %s\n' "$*" >&2; }

# clone <name> <url> <ref>  -> shallow clone/refresh into $src/<name>
clone() {
    local name="$1" url="$2" ref="${3:-}"
    if [[ -d "$src/$name/.git" ]]; then
        note "[$name] already cloned"
        return 0
    fi
    note "[$name] cloning $url ..."
    if [[ -n "$ref" ]]; then
        git clone --depth 1 --branch "$ref" "$url" "$src/$name" >>"$logs/$name.log" 2>&1
    else
        git clone --depth 1 "$url" "$src/$name" >>"$logs/$name.log" 2>&1
    fi
}

# link_so <name> <built-so-path>  -> symlink into lib/<name>.so
link_so() {
    local name="$1" so="$2"
    if [[ -e "$so" ]]; then
        ln -sf "$so" "$lib/$name.so"
        note "[$name] -> lib/$name.so"
        ok+=("$name")
        return 0
    fi
    note "[$name] FAILED: no .so produced (see $logs/$name.log)"
    fail+=("$name")
    return 1
}

want() { # is <name> requested?
    [[ ${#REQUESTED[@]} -eq 0 ]] && return 0
    local n; for n in "${REQUESTED[@]}"; do [[ "$n" == "$1" ]] && return 0; done
    return 1
}

done_already() { # skip if built and not FORCE
    [[ "$FORCE" != 1 && -e "$lib/$1.so" ]] && { note "[$1] up to date (FORCE=1 to rebuild)"; ok+=("$1"); return 0; }
    return 1
}

# ---------------- snmalloc (message-passing; the pagemap design) ----------------
build_snmalloc() {
    want snmalloc || return 0
    done_already snmalloc && return 0
    clone snmalloc https://github.com/microsoft/snmalloc.git || { fail+=(snmalloc); return 0; }
    note "[snmalloc] building ..."
    ( cmake -S "$src/snmalloc" -B "$src/snmalloc/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        && ninja -C "$src/snmalloc/build" snmallocshim ) >>"$logs/snmalloc.log" 2>&1
    local so; so="$(ls "$src"/snmalloc/build/libsnmallocshim.so 2>/dev/null | head -1)"
    [[ -z "$so" ]] && so="$(find "$src/snmalloc/build" -name 'libsnmallocshim*.so' 2>/dev/null | head -1)"
    link_so snmalloc "$so"
}

# ---------------- tcmalloc (gperftools, minimal = pure allocator) ----------------
build_tcmalloc() {
    want tcmalloc || return 0
    done_already tcmalloc && return 0
    clone tcmalloc https://github.com/gperftools/gperftools.git || { fail+=(tcmalloc); return 0; }
    note "[tcmalloc] building ..."
    ( cmake -S "$src/tcmalloc" -B "$src/tcmalloc/build" -DCMAKE_BUILD_TYPE=Release \
        -DGPERFTOOLS_BUILD_STATIC=OFF -DGPERFTOOLS_BUILD_HEAP_PROFILER=OFF \
        -DGPERFTOOLS_BUILD_HEAP_CHECKER=OFF -DGPERFTOOLS_BUILD_CPU_PROFILER=OFF \
        && cmake --build "$src/tcmalloc/build" -j "$jobs" ) >>"$logs/tcmalloc.log" 2>&1
    local so; so="$(find "$src/tcmalloc/build" -name 'libtcmalloc_minimal.so' 2>/dev/null | head -1)"
    [[ -z "$so" ]] && so="$(find "$src/tcmalloc/build" -name 'libtcmalloc.so' 2>/dev/null | head -1)"
    link_so tcmalloc "$so"
}

# ---------------- ffmalloc (security: one-time allocation, UAF-immune) ----------------
build_ffmalloc() {
    want ffmalloc || return 0
    done_already ffmalloc && return 0
    clone ffmalloc https://github.com/bwickman97/ffmalloc.git || { fail+=(ffmalloc); return 0; }
    note "[ffmalloc] building ..."
    ( make -C "$src/ffmalloc" -j "$jobs" ) >>"$logs/ffmalloc.log" 2>&1
    # ffmalloc emits several variants; prefer the multithreaded shared lib.
    local so
    so="$(find "$src/ffmalloc" -maxdepth 2 -name 'libffmalloc*.so' 2>/dev/null | grep -vi npmt | head -1)"
    [[ -z "$so" ]] && so="$(find "$src/ffmalloc" -maxdepth 2 -name 'libffmalloc*.so' 2>/dev/null | head -1)"
    link_so ffmalloc "$so"
}

# ---------------- hardened_malloc (security: hardened, GrapheneOS) ----------------
build_hardened() {
    want hardened_malloc || return 0
    done_already hardened_malloc && return 0
    clone hardened_malloc https://github.com/GrapheneOS/hardened_malloc.git || { fail+=(hardened_malloc); return 0; }
    note "[hardened_malloc] building ..."
    ( make -C "$src/hardened_malloc" -j "$jobs" ) >>"$logs/hardened_malloc.log" 2>&1
    local so; so="$(find "$src/hardened_malloc/out" -name 'libhardened_malloc*.so' 2>/dev/null | head -1)"
    link_so hardened_malloc "$so"
}

REQUESTED=("$@")

build_snmalloc
build_tcmalloc
build_ffmalloc
build_hardened

echo >&2
echo "=== fetch summary ===" >&2
echo "built/available: ${ok[*]:-none}" >&2
echo "failed:          ${fail[*]:-none}" >&2
echo "libs in: $lib" >&2
ls -l "$lib" 2>/dev/null >&2 || true
