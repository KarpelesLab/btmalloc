# Tests for btmalloc. CTest entry per file.
#
# Each test target links against the static library so we test the
# allocator implementation regardless of LD_PRELOAD wiring.

function(btm_add_test name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE btmalloc_static)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

btm_add_test(test_basic)
btm_add_test(test_stress)
btm_add_test(test_harden)
btm_add_test(test_doublefree)
set_tests_properties(test_doublefree PROPERTIES ENVIRONMENT "BTM_HARDEN=1")
btm_add_test(test_compact)
set_tests_properties(test_compact PROPERTIES ENVIRONMENT "BTM_MESH=1")
btm_add_test(test_tiering)

# Standalone random fuzzer (also builds as a libFuzzer target via tools/fuzz.sh).
# Run a short PRNG-driven smoke in each mode so CI exercises random op
# sequences against the shadow-tag oracle.
add_executable(fuzz_alloc fuzz_alloc.c)
target_link_libraries(fuzz_alloc PRIVATE btmalloc_static)
target_include_directories(fuzz_alloc PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_test(NAME fuzz_default COMMAND fuzz_alloc 8000 1)
add_test(NAME fuzz_intern  COMMAND fuzz_alloc 8000 2)
set_tests_properties(fuzz_intern PROPERTIES ENVIRONMENT "BTM_PARTITION_MODE=intern")
add_test(NAME fuzz_mesh    COMMAND fuzz_alloc 3000 3)
set_tests_properties(fuzz_mesh PROPERTIES ENVIRONMENT "BTM_MESH=1")
add_test(NAME fuzz_harden  COMMAND fuzz_alloc 6000 4)
set_tests_properties(fuzz_harden PROPERTIES ENVIRONMENT "BTM_HARDEN=1")

# Preload test needs the shared library path at runtime. Only meaningful when
# the shared lib actually overrides libc malloc (not under ASan, which provides
# its own allocator and builds an ASan-runtime-dependent .so).
if(BTM_OVERRIDE_LIBC)
    add_executable(test_preload test_preload.c)
    target_link_libraries(test_preload PRIVATE btmalloc_static)
    target_include_directories(test_preload PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME test_preload COMMAND test_preload $<TARGET_FILE:btmalloc>)
endif()
