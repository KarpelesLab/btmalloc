# Benchmarks. These deliberately do NOT link any btmalloc target — they call
# the standard malloc/free so the same binary measures whichever allocator is
# LD_PRELOAD'd (glibc, jemalloc, btmalloc). bench/run.sh orchestrates that.

function(btm_add_bench name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE pthread)
    # Keep optimizer from eliding allocations while still building -O2/-O3.
    target_compile_options(${name} PRIVATE -fno-builtin-malloc
                                           -fno-builtin-calloc
                                           -fno-builtin-realloc
                                           -fno-builtin-free)
endfunction()

btm_add_bench(bench_micro)
btm_add_bench(bench_threaded)
btm_add_bench(bench_rss)
btm_add_bench(bench_latency)
