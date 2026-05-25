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

# Preload test needs the shared library path at runtime. Only meaningful when
# the shared lib actually overrides libc malloc (not under ASan, which provides
# its own allocator and builds an ASan-runtime-dependent .so).
if(BTM_OVERRIDE_LIBC)
    add_executable(test_preload test_preload.c)
    target_link_libraries(test_preload PRIVATE btmalloc_static)
    target_include_directories(test_preload PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME test_preload COMMAND test_preload $<TARGET_FILE:btmalloc>)
endif()
