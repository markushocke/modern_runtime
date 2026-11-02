option(MODERN_RUNTIME_BUILD_KPI "Build standalone runtime KPI benchmarks and stress tests" OFF)
if(MODERN_RUNTIME_BUILD_KPI)
  add_executable(modern_runtime_benchmark benchmarks/runtime_benchmark.cpp)
  target_link_libraries(modern_runtime_benchmark PRIVATE modern_runtime)
  target_compile_features(modern_runtime_benchmark PRIVATE cxx_std_23)

  add_executable(modern_runtime_stress tests/runtime_stress.cpp)
  target_link_libraries(modern_runtime_stress PRIVATE modern_runtime)
  target_compile_features(modern_runtime_stress PRIVATE cxx_std_23)
  if(BUILD_TESTING)
    foreach(_scenario IN ITEMS producers shutdown cancellation)
      add_test(
        NAME modern_runtime_stress_${_scenario}
        COMMAND modern_runtime_stress --case ${_scenario} --iterations 10000
      )
      set_tests_properties(modern_runtime_stress_${_scenario} PROPERTIES
        TIMEOUT 45
        LABELS "stress;kpi"
      )
    endforeach()
  endif()
endif()
