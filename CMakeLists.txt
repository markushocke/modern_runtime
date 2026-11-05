cmake_minimum_required(VERSION 3.28)

project(modern_runtime VERSION 0.1.0 LANGUAGES CXX)

# GCC 13 cannot provide CMake C++23 named-module dependency discovery.
# Report an actionable error before CMake reaches opaque try_compile failures.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR
    "modern_runtime requires C++23 named-module dependency scanning; "
    "GCC ${CMAKE_CXX_COMPILER_VERSION} is not supported by this build. "
    "Configure a fresh build with Clang and clang-scan-deps, e.g. "
    "cmake --preset kpi, or set -DCMAKE_CXX_COMPILER=/path/to/clang++. "
    "CMake caches the compiler; use a fresh build directory when switching.")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/modern_trace_provider.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MODERN_RUNTIME_PACKAGE_NAME modern_runtime)
set(MODERN_RUNTIME_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}")
set(MODERN_TRACE_PROVIDER "auto" CACHE STRING "How to resolve modern_trace: auto, package, local, or fetch")
set_property(CACHE MODERN_TRACE_PROVIDER PROPERTY STRINGS auto package local fetch)
set(MODERN_TRACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../modern_trace" CACHE PATH "Path to a local modern_trace checkout")
set(MODERN_TRACE_GIT_REPOSITORY "https://github.com/markushocke/modern_trace.git" CACHE STRING "Git repository used when MODERN_TRACE_PROVIDER=fetch")
set(MODERN_TRACE_GIT_TAG "main" CACHE STRING "Git tag or branch used when MODERN_TRACE_PROVIDER=fetch")

modern_runtime_resolve_trace_target(
  MODERN_TRACE_TARGET
  DEFAULT_PROVIDER "auto"
  LOCAL_ROOT "${MODERN_TRACE_ROOT}"
  FETCH_REPOSITORY "${MODERN_TRACE_GIT_REPOSITORY}"
  FETCH_TAG "${MODERN_TRACE_GIT_TAG}"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/modern_trace"
)

find_package(Threads REQUIRED)

# Enable module scanning only after compiler/threads probes. CMake uses
# this default for importers (tests, demos and KPI executables) as well.
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(MODERN_RUNTIME_INLINE_CALLABLE_BYTES "48" CACHE STRING
  "Inline capacity for small scheduled callables (32 or 48 bytes)")
set_property(CACHE MODERN_RUNTIME_INLINE_CALLABLE_BYTES PROPERTY STRINGS 32 48)
if(NOT MODERN_RUNTIME_INLINE_CALLABLE_BYTES MATCHES "^(32|48)$")
  message(FATAL_ERROR "MODERN_RUNTIME_INLINE_CALLABLE_BYTES must be 32 or 48")
endif()

add_library(modern_runtime)
target_compile_features(modern_runtime PUBLIC cxx_std_23)
target_compile_definitions(modern_runtime PUBLIC
  MODERN_RUNTIME_INLINE_CALLABLE_BYTES=${MODERN_RUNTIME_INLINE_CALLABLE_BYTES})
target_link_libraries(modern_runtime PUBLIC Threads::Threads ${MODERN_TRACE_TARGET})
target_include_directories(modern_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/modules
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/detail
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/exec
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/thread
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/timer
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/memory
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/platform
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/sync
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/stream
  ${CMAKE_CURRENT_SOURCE_DIR}/modules/runtime
)
add_library(modern_runtime::modern_runtime ALIAS modern_runtime)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_definitions(modern_runtime PUBLIC __cpp_concepts=202002L)
  target_compile_options(modern_runtime PUBLIC -Wno-builtin-macro-redefined)
endif()

target_sources(modern_runtime
  PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES
      modules/exec/modern.exec.cppm
      modules/exec/api.cppm
      modules/exec/detail.cppm
      modules/exec/scheduler.cppm
      modules/task/modern.task.cppm
      modules/task/shared_state.cppm
      modules/task/task_continuation.cppm
      modules/task/environment.cppm
      modules/stream/modern.stream.cppm
      modules/thread/modern.thread.cppm
      modules/thread/api.cppm
      modules/thread/state.cppm
      modules/timer/modern.timer.cppm
      modules/timer/api.cppm
      modules/timer/delayed.cppm
      modules/timer/periodic_templates.cppm
      modules/timer/state.cppm
      modules/memory/modern.memory.cppm
      modules/platform/modern.platform.cppm
      modules/sync/modern.sync.cppm
      modules/modern.runtime.cppm
      modules/runtime/thread_adapter.cppm
      modules/runtime/timer_adapter.cppm
      modules/runtime/io_bridge.cppm
      modules/runtime/io_backend.cppm
      modules/runtime/modern_io_adapter.cppm
      modules/runtime/io_uring_adapter.cppm
      modules/runtime/poller_adapter.cppm
      modules/runtime/fiber_scheduler.cppm
      modules/runtime/sender_bridge.cppm
  PRIVATE
    modules/thread/thread_pool.cpp
    modules/thread/queue_chase_lev.cpp
    modules/thread/worker.cpp
    modules/timer/scheduled_executor.cpp
    modules/timer/periodic.cpp
    modules/memory/arena.cpp
    modules/memory/pool.cpp
    modules/platform/clocks.cpp
    modules/sync/event.cpp
)

add_executable(exec_smoke tests/exec_smoke.cpp)
target_link_libraries(exec_smoke PRIVATE modern_runtime)

add_executable(async_demo examples/async_demo.cpp)
target_link_libraries(async_demo PRIVATE modern_runtime)

add_executable(smoke_test tests/smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE modern_runtime)

add_executable(task_characterization tests/task_characterization.cpp)
target_link_libraries(task_characterization PRIVATE modern_runtime)

add_executable(stream_characterization tests/stream_characterization.cpp)
target_link_libraries(stream_characterization PRIVATE modern_runtime)

add_executable(move_only_function_test tests/move_only_function_test.cpp)
set_target_properties(move_only_function_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_compile_definitions(move_only_function_test PRIVATE
  MODERN_RUNTIME_INLINE_CALLABLE_BYTES=${MODERN_RUNTIME_INLINE_CALLABLE_BYTES})

# Header-only queue algorithm validation, independent of C++23 modules.
add_executable(lock_free_queue_primitives_test tests/lock_free_queue_primitives.cpp)
set_target_properties(lock_free_queue_primitives_test PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_link_libraries(lock_free_queue_primitives_test PRIVATE Threads::Threads)

include(CTest)
if(BUILD_TESTING)
  add_test(NAME exec_smoke COMMAND exec_smoke)
  add_test(NAME smoke_test COMMAND smoke_test)
  add_test(NAME task_characterization COMMAND task_characterization)
  add_test(NAME stream_characterization COMMAND stream_characterization)
  add_test(NAME move_only_function_test COMMAND move_only_function_test)
  add_test(NAME lock_free_queue_primitives COMMAND lock_free_queue_primitives_test)
  set_tests_properties(lock_free_queue_primitives PROPERTIES
    TIMEOUT 45 LABELS "stress;pool;lockfree")
endif()

get_target_property(_modern_runtime_compile_options modern_runtime COMPILE_OPTIONS)
if(_modern_runtime_compile_options STREQUAL "_modern_runtime_compile_options-NOTFOUND")
  set(_modern_runtime_compile_options "")
endif()

get_target_property(_modern_runtime_compile_definitions modern_runtime COMPILE_DEFINITIONS)
if(_modern_runtime_compile_definitions STREQUAL "_modern_runtime_compile_definitions-NOTFOUND")
  set(_modern_runtime_compile_definitions "")
endif()

get_target_property(_modern_runtime_interface_link_libraries modern_runtime INTERFACE_LINK_LIBRARIES)
if(_modern_runtime_interface_link_libraries STREQUAL "_modern_runtime_interface_link_libraries-NOTFOUND")
  set(_modern_runtime_interface_link_libraries "")
endif()

set(MODERN_RUNTIME_IMPORTED_COMPILE_OPTIONS "${_modern_runtime_compile_options}")
set(MODERN_RUNTIME_IMPORTED_COMPILE_DEFINITIONS "${_modern_runtime_compile_definitions}")
set(MODERN_RUNTIME_IMPORTED_LINK_LIBRARIES "${_modern_runtime_interface_link_libraries}")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}Config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}Config.cmake"
  INSTALL_DESTINATION "${MODERN_RUNTIME_INSTALL_CMAKEDIR}"
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}Targets.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}Targets.cmake"
  @ONLY
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}ConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY AnyNewerVersion
)

install(
  TARGETS modern_runtime
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  FILE_SET cxx_modules DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${MODERN_RUNTIME_PACKAGE_NAME}"
)

install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/modules/detail"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${MODERN_RUNTIME_PACKAGE_NAME}/modules"
  FILES_MATCHING
    PATTERN "*.hpp"
)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}ConfigVersion.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}Targets.cmake"
  DESTINATION "${MODERN_RUNTIME_INSTALL_CMAKEDIR}"
)

# Optional, standalone performance and stress harness.
include(cmake/RuntimeKpi.cmake)
