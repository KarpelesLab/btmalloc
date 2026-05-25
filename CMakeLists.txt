# btmalloc — CMake build.
#
# Two library targets (shared + static) sharing one source set. Tests are
# under tests/ (gated on BTM_BUILD_TESTS); benchmarks under bench/ (gated
# on BTM_BUILD_BENCH; land in M4).

cmake_minimum_required(VERSION 3.15)
project(btmalloc
    VERSION 0.0.1
    DESCRIPTION "malloc/realloc/free replacement library"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # gnu11: enables _GNU_SOURCE features, __thread, etc.

# Default to RelWithDebInfo for single-config generators.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(BTM_BUILD_TESTS    "Build CTest test suite"                  ON)
option(BTM_BUILD_BENCH    "Build benchmark suite (lands in M4)"     OFF)
option(BTM_OVERRIDE_LIBC  "Override libc malloc symbols (M3+)"      ON)
option(BTM_DEBUG_FILL     "Fill freed memory with 0xDF (debug aid)" OFF)
option(BTM_ASAN           "Build with AddressSanitizer"             OFF)

# ASan ships its own malloc; can't sensibly coexist with our overrides.
if(BTM_ASAN AND BTM_OVERRIDE_LIBC)
    message(STATUS "btmalloc: BTM_ASAN=ON forces BTM_OVERRIDE_LIBC=OFF")
    set(BTM_OVERRIDE_LIBC OFF)
endif()

add_compile_options(
    -Wall -Wextra -Werror
    -Wno-unused-parameter
    -fvisibility=hidden
    -fPIC
    -pthread
    -D_GNU_SOURCE
)

# Per-config flags.
add_compile_options(
    "$<$<CONFIG:Debug>:-O0;-g3;-DBTM_DEBUG=1>"
    "$<$<CONFIG:Release>:-O3;-DNDEBUG;-flto>"
    "$<$<CONFIG:RelWithDebInfo>:-O2;-g>"
    "$<$<CONFIG:MinSizeRel>:-Os;-DNDEBUG>"
)
add_link_options(
    "$<$<CONFIG:Release>:-flto>"
)

if(BTM_DEBUG_FILL)
    add_compile_definitions(BTM_DEBUG_FILL=1)
endif()

if(BTM_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

set(BTMALLOC_SOURCES
    src/btmalloc.c
)

# LD_PRELOAD shim — landed for real in M3; currently absent.
if(BTM_OVERRIDE_LIBC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/preload.c")
    list(APPEND BTMALLOC_SOURCES src/preload.c)
endif()

# Shared library — the primary deliverable.
add_library(btmalloc SHARED ${BTMALLOC_SOURCES})
target_include_directories(btmalloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(btmalloc PUBLIC pthread)
set_target_properties(btmalloc PROPERTIES
    OUTPUT_NAME btmalloc
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION   ${PROJECT_VERSION}
)

# Static library — used by tests so they exercise the implementation
# directly without dlopen-time gymnastics.
add_library(btmalloc_static STATIC ${BTMALLOC_SOURCES})
target_include_directories(btmalloc_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(btmalloc_static PUBLIC pthread)
set_target_properties(btmalloc_static PROPERTIES OUTPUT_NAME btmalloc)

if(BTM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BTM_BUILD_BENCH)
    add_subdirectory(bench)
endif()

message(STATUS "btmalloc ${PROJECT_VERSION}")
message(STATUS "  CMAKE_BUILD_TYPE     = ${CMAKE_BUILD_TYPE}")
message(STATUS "  BTM_BUILD_TESTS      = ${BTM_BUILD_TESTS}")
message(STATUS "  BTM_BUILD_BENCH      = ${BTM_BUILD_BENCH}")
message(STATUS "  BTM_OVERRIDE_LIBC    = ${BTM_OVERRIDE_LIBC}")
message(STATUS "  BTM_DEBUG_FILL       = ${BTM_DEBUG_FILL}")
message(STATUS "  BTM_ASAN             = ${BTM_ASAN}")
