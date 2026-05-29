cmake_minimum_required(VERSION 3.28)

project(modern_runtime VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(MODERN_RUNTIME_PACKAGE_NAME modern_runtime)
set(MODERN_RUNTIME_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/${MODERN_RUNTIME_PACKAGE_NAME}")

find_package(Threads REQUIRED)

add_library(modern_runtime)
target_compile_features(modern_runtime PUBLIC cxx_std_23)
target_link_libraries(modern_runtime PUBLIC Threads::Threads)
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
      modules/task/modern.task.cppm
      modules/task/shared_state.cppm
      modules/task/continuation.cppm
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
      modules/runtime/coroutine_task.cppm
        modules/runtime/io_bridge.cppm
          modules/runtime/io_backend.cppm
        modules/runtime/modern_io_adapter.cppm
          modules/runtime/io_uring_adapter.cppm
          modules/runtime/poller_adapter.cppm
            modules/runtime/fiber_scheduler.cppm
        modules/runtime/sender_bridge.cppm
  PRIVATE
      modules/exec/scheduler.cpp
      modules/exec/inline_executor.cpp
      modules/thread/thread_pool.cpp
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
