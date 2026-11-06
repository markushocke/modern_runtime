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
        COMMAND modern_runtime_stress --case ${_scenario} --iterations 100000
      )
      set_tests_properties(modern_runtime_stress_${_scenario} PROPERTIES
        TIMEOUT 45
        LABELS "stress;kpi"
      )
    endforeach()
  endif()

  add_executable(thread_pool_stress tests/thread_pool_stress.cpp)
  target_link_libraries(thread_pool_stress PRIVATE modern_runtime)
  if(BUILD_TESTING)
    foreach(_workers IN ITEMS 1 2 4 8 16)
      foreach(_scenario IN ITEMS producers local_fanout shutdown_race queue_limit exceptions lifecycle allocation_failure cancellation_race)
        add_test(NAME pool_${_scenario}_${_workers}
          COMMAND thread_pool_stress ${_scenario} ${_workers} 100000 injection)
        set_tests_properties(pool_${_scenario}_${_workers} PROPERTIES
          TIMEOUT 45 LABELS "stress;pool")
      endforeach()
    endforeach()
    # Lock-free backend: the same race/priority/allocator scenarios as the
    # mutex variants. 10k per case keeps default CTest latency reasonable.
    foreach(_workers IN ITEMS 1 2 4 8 16)
      foreach(_scenario IN ITEMS producers local_fanout shutdown_race
                                  queue_limit exceptions lifecycle
                                  allocation_failure cancellation_race)
        add_test(NAME pool_chase_lev_${_scenario}_${_workers}
          COMMAND thread_pool_stress ${_scenario} ${_workers} 10000 chase_lev)
        set_tests_properties(pool_chase_lev_${_scenario}_${_workers} PROPERTIES
          TIMEOUT 45 LABELS "stress;pool;lockfree")
      endforeach()
    endforeach()
    # Idle-only parking is an OPT-IN scheduling experiment, independent of
    # the selected queue backend. Test all eight legacy stress scenarios plus
    # repeated wake-after-idle waves, 1..16 workers. Distinct CTest names
    # prevent silent changes to the existing semaphore-backed acceptance suite.
    foreach(_workers IN ITEMS 1 2 4 8 16)
      foreach(_scenario IN ITEMS producers local_fanout shutdown_race queue_limit
                                  exceptions lifecycle allocation_failure
                                  cancellation_race idle_waves)
        add_test(NAME pool_park_chase_lev_${_scenario}_${_workers}
          COMMAND thread_pool_stress ${_scenario} ${_workers} 10000 chase_lev park)
        set_tests_properties(pool_park_chase_lev_${_scenario}_${_workers}
          PROPERTIES TIMEOUT 45 LABELS "stress;pool;idlepark;lockfree")
      endforeach()
    endforeach()
    # Prove wakeup/queue independence on both existing mutex backends.
    foreach(_backend IN ITEMS direct injection)
      foreach(_workers IN ITEMS 1 4 16)
        foreach(_scenario IN ITEMS producers local_fanout shutdown_race
                                    queue_limit idle_waves)
          add_test(NAME pool_park_${_backend}_${_scenario}_${_workers}
            COMMAND thread_pool_stress ${_scenario} ${_workers} 10000 ${_backend} park)
          set_tests_properties(pool_park_${_backend}_${_scenario}_${_workers}
            PROPERTIES TIMEOUT 45 LABELS "stress;pool;idlepark")
        endforeach()
      endforeach()
    endforeach()
    # Small identical correctness matrix on the original queue backend.
    # The default path above tests injection; both use the same scenarios.
    foreach(_workers IN ITEMS 1 2 4 8 16)
      foreach(_scenario IN ITEMS producers local_fanout shutdown_race
                                  queue_limit exceptions lifecycle
                                  allocation_failure cancellation_race)
        add_test(NAME pool_direct_${_scenario}_${_workers}
          COMMAND thread_pool_stress ${_scenario} ${_workers} 10000 direct)
        set_tests_properties(pool_direct_${_scenario}_${_workers} PROPERTIES
          TIMEOUT 45 LABELS "stress;pool;backend")
      endforeach()
    endforeach()
  endif()
endif()
