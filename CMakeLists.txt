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

# Core allocator sources, shared by both library flavors. The LD_PRELOAD
# symbol overrides (src/preload.c) are deliberately NOT here — they go only
# into the shared library, so the static library the tests link against does
# not hijack libc malloc inside the test process.
set(BTMALLOC_CORE_SOURCES
    src/btmalloc.c
    src/size_class.c
    src/chunk.c
    src/partition.c
    src/tcache.c
    src/large.c
    src/background.c
)

# liburing for the async backing-store (Phase C). Required.
find_library(URING_LIB uring REQUIRED)
find_path(URING_INCLUDE liburing.h)

# Compile the core once as position-independent objects, reused by both libs.
add_library(btmalloc_objs OBJECT ${BTMALLOC_CORE_SOURCES})
target_include_directories(btmalloc_objs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties(btmalloc_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Shared library — the primary deliverable + LD_PRELOAD drop-in.
set(BTMALLOC_SHARED_SOURCES $<TARGET_OBJECTS:btmalloc_objs>)
if(BTM_OVERRIDE_LIBC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/preload.c")
    list(APPEND BTMALLOC_SHARED_SOURCES src/preload.c)
endif()

add_library(btmalloc SHARED ${BTMALLOC_SHARED_SOURCES})
target_include_directories(btmalloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(btmalloc PUBLIC pthread ${URING_LIB})
set_target_properties(btmalloc PROPERTIES
    OUTPUT_NAME btmalloc
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION   ${PROJECT_VERSION}
)

# Static library — used by tests so they exercise the implementation directly.
add_library(btmalloc_static STATIC $<TARGET_OBJECTS:btmalloc_objs>)
target_include_directories(btmalloc_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(btmalloc_static PUBLIC pthread ${URING_LIB})
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
