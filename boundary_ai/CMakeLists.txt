cmake_minimum_required(VERSION 3.24)
project(boundary_ai LANGUAGES CXX)

add_library(boundary_ai INTERFACE)
target_include_directories(boundary_ai INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(boundary_ai INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(boundary_ai INTERFACE /W4 /permissive-)
else()
  target_compile_options(boundary_ai INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(boundary_ai_tests tests/test_boundary_ai.cpp)
  target_link_libraries(boundary_ai_tests PRIVATE boundary_ai)
  add_test(NAME boundary_ai_tests COMMAND boundary_ai_tests)

  add_executable(ptcg_adapter_compile_test tests/ptcg_adapter_compile_test.cpp)
  target_link_libraries(ptcg_adapter_compile_test PRIVATE boundary_ai)
  target_include_directories(ptcg_adapter_compile_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../ptcg_engine/ptcgProgram 22")
  add_test(NAME ptcg_adapter_compile_test COMMAND ptcg_adapter_compile_test)

  add_executable(ptcg_exact_bound_merge_test tests/exact_bound_merge_test.cpp)
  target_link_libraries(ptcg_exact_bound_merge_test PRIVATE boundary_ai)
  target_include_directories(ptcg_exact_bound_merge_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../ptcg_engine/ptcgProgram 22")
  add_test(NAME ptcg_exact_bound_merge_test COMMAND ptcg_exact_bound_merge_test)

  add_executable(boundary_ai_benchmark tests/benchmark.cpp)
  target_link_libraries(boundary_ai_benchmark PRIVATE boundary_ai)
endif()
