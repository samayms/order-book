cmake_minimum_required(VERSION 3.20)
project(orderbook VERSION 2.0.0 LANGUAGES CXX)

option(ORDERBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
set(ORDERBOOK_SANITIZER "" CACHE STRING "Sanitizer: address-undefined or thread")

find_package(Threads REQUIRED)

# Enable link-time / interprocedural optimization for Release only. Measured on
# the deterministic benchmark (M4-class arm64, Apple Clang 17): LTO improves
# Release throughput repeatably (~+2-6% across interleaved runs) with identical
# correctness (checksum, invariants, zero rejections). Debug and sanitizer
# builds deliberately skip it to keep codegen fast and debuggable. Guarded by a
# support check so toolchains without IPO still configure.
include(CheckIPOSupported)
check_ipo_supported(RESULT ORDERBOOK_IPO_SUPPORTED OUTPUT ORDERBOOK_IPO_MESSAGE)
if(ORDERBOOK_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
else()
    message(STATUS "IPO/LTO not enabled: ${ORDERBOOK_IPO_MESSAGE}")
endif()

# Release ships -O2 instead of CMake's default -O3. On the deterministic
# benchmark (M4-class arm64, Apple Clang 17, LTO on) -O2 measured faster than
# -O3 in two independent paired studies (+0.3% over 15 rounds, +1.65% over 10
# rounds, significant the second time; checksum identical either way). The
# margin is small and workload-specific — -O3's heavier inlining/unrolling costs
# more than it buys on this branchy map + hash-probe code. Revisit if the
# workload changes. Surgical swap so -DNDEBUG and any other default flags remain.
if(NOT MSVC)
    string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

add_library(orderbook_warnings INTERFACE)
if(MSVC)
    target_compile_options(orderbook_warnings INTERFACE
        /W4 $<$<BOOL:${ORDERBOOK_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(orderbook_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        $<$<BOOL:${ORDERBOOK_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

add_library(orderbook
    src/engine.cpp
    src/events.cpp
    src/order_book.cpp
    src/order_index.cpp
    src/order_pool.cpp)
add_library(orderbook::orderbook ALIAS orderbook)
target_compile_features(orderbook PUBLIC cxx_std_20)
set_target_properties(orderbook PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(orderbook PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(orderbook PUBLIC Threads::Threads PRIVATE orderbook_warnings)

if(ORDERBOOK_SANITIZER STREQUAL "address-undefined")
    if(MSVC)
        message(FATAL_ERROR "address-undefined sanitizer requires Clang or GCC")
    endif()
    target_compile_options(orderbook PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(orderbook PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif(ORDERBOOK_SANITIZER STREQUAL "thread")
    if(MSVC)
        message(FATAL_ERROR "thread sanitizer requires Clang or GCC")
    endif()
    target_compile_options(orderbook PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(orderbook PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT ORDERBOOK_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown ORDERBOOK_SANITIZER=${ORDERBOOK_SANITIZER}")
endif()

function(orderbook_executable target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(${target} PRIVATE orderbook::orderbook orderbook_warnings)
endfunction()

orderbook_executable(orderbook_demo apps/demo_main.cpp)
orderbook_executable(orderbook_benchmark benchmarks/orderbook_benchmark.cpp)

enable_testing()
orderbook_executable(orderbook_core_tests tests/test_order_book.cpp)
target_include_directories(orderbook_core_tests PRIVATE tests)
add_test(NAME orderbook_core_tests COMMAND orderbook_core_tests)
set_tests_properties(orderbook_core_tests PROPERTIES LABELS "unit")

orderbook_executable(orderbook_engine_tests tests/test_engine.cpp)
target_include_directories(orderbook_engine_tests PRIVATE tests)
add_test(NAME orderbook_engine_tests COMMAND orderbook_engine_tests)
set_tests_properties(orderbook_engine_tests PROPERTIES LABELS "integration;threaded")

include(GNUInstallDirs)
install(TARGETS orderbook EXPORT orderbookTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
