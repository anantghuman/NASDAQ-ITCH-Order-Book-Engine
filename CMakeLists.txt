cmake_minimum_required(VERSION 3.20)

project(totalview_itch_lob VERSION 0.1.0 LANGUAGES CXX)

option(LOB_ENABLE_SANITIZERS "Enable address and undefined-behaviour sanitizers" OFF)
option(LOB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(LOB_ENABLE_HARDENING "Enable platform-appropriate production hardening" ON)
option(LOB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LOB_BUILD_BENCHMARKS "Build local latency benchmark executables" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(ZLIB QUIET)

function(lob_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(LOB_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(LOB_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(lob_enable_hardening target)
  if(NOT LOB_ENABLE_HARDENING)
    return()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /sdl)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -fstack-protector-strong)
    if(NOT APPLE)
      target_compile_definitions(${target} PRIVATE $<$<CONFIG:Release>:_FORTIFY_SOURCE=3>)
    endif()
  endif()
endfunction()

add_library(lob_core
  src/book/order_book.cpp
  src/itch/itch.cpp
  src/engine/matching_engine.cpp
  src/metrics/latency.cpp
)

target_include_directories(lob_core PUBLIC include)
target_link_libraries(lob_core PUBLIC Threads::Threads)

lob_enable_warnings(lob_core)
lob_enable_hardening(lob_core)

if(LOB_ENABLE_SANITIZERS AND LOB_ENABLE_TSAN)
  message(FATAL_ERROR "LOB_ENABLE_SANITIZERS and LOB_ENABLE_TSAN cannot be enabled together")
endif()

if(LOB_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lob_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_core PUBLIC -fsanitize=address,undefined)
endif()

if(LOB_ENABLE_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lob_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(lob_core PUBLIC -fsanitize=thread)
endif()

add_executable(itch_replay src/app/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE lob_core)
lob_enable_warnings(itch_replay)
lob_enable_hardening(itch_replay)
if(ZLIB_FOUND)
  target_link_libraries(itch_replay PRIVATE ZLIB::ZLIB)
  target_compile_definitions(itch_replay PRIVATE LOB_HAVE_ZLIB=1)
endif()

if(LOB_BUILD_BENCHMARKS)
  add_executable(lob_latency_bench bench/latency_bench.cpp)
  target_link_libraries(lob_latency_bench PRIVATE lob_core)
  lob_enable_warnings(lob_latency_bench)
  lob_enable_hardening(lob_latency_bench)
endif()

enable_testing()
foreach(test_name IN ITEMS order_book order_book_model itch concurrency_metrics matching_engine)
  add_executable(${test_name}_test tests/${test_name}_test.cpp)
  target_link_libraries(${test_name}_test PRIVATE lob_core)
  lob_enable_warnings(${test_name}_test)
  lob_enable_hardening(${test_name}_test)
  # The decoder and book tests use assert; retain their checks even in a
  # Release benchmark build.
  if(MSVC)
    target_compile_options(${test_name}_test PRIVATE /UNDEBUG)
  else()
    target_compile_options(${test_name}_test PRIVATE -UNDEBUG)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name}_test)
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endforeach()
