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
