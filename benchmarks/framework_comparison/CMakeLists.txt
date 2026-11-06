cmake_minimum_required(VERSION 3.28)
project(modern_runtime_framework_comparison LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(FetchContent)
find_package(Threads REQUIRED)

option(COMPARE_WITH_TASKFLOW "Build the Taskflow adapter" ON)
option(COMPARE_WITH_ONETBB "Build the oneTBB adapter" ON)
option(COMPARE_WITH_STDEXEC "Build the stdexec sender/receiver adapter" OFF)
option(COMPARE_FETCH_DEPS "Download pinned competitor sources" ON)
option(COMPARE_USE_SYSTEM_TBB "Use installed oneTBB instead of the pinned revision" OFF)

# Build the runtime unmodified as a dependency of these tests.
set(MODERN_RUNTIME_BUILD_KPI OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/runtime")

function(compare_executable name selector)
  add_executable(${name} pool_compare.cpp)
  target_compile_features(${name} PRIVATE cxx_std_23)
  target_compile_definitions(${name} PRIVATE "COMPARE_${selector}=1")
  target_link_libraries(${name} PRIVATE Threads::Threads)
endfunction()

compare_executable(compare_modern MODERN)
compare_executable(compare_modern_injection MODERN_INJECTION)
compare_executable(compare_modern_chase_lev MODERN_CHASE_LEV)
compare_executable(compare_modern_chase_lev_park MODERN_PARK)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  compare_executable(compare_modern_chase_lev_futex MODERN_FUTEX)
endif()
# Both benchmark translation units import the runtime's named C++23 module.
set_property(TARGET compare_modern PROPERTY CXX_SCAN_FOR_MODULES ON)
set_property(TARGET compare_modern_injection PROPERTY CXX_SCAN_FOR_MODULES ON)
set_property(TARGET compare_modern_chase_lev PROPERTY CXX_SCAN_FOR_MODULES ON)
set_property(TARGET compare_modern_chase_lev_park PROPERTY CXX_SCAN_FOR_MODULES ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set_property(TARGET compare_modern_chase_lev_futex PROPERTY CXX_SCAN_FOR_MODULES ON)
endif()
target_link_libraries(compare_modern PRIVATE modern_runtime)
target_link_libraries(compare_modern_injection PRIVATE modern_runtime)
target_link_libraries(compare_modern_chase_lev PRIVATE modern_runtime)
target_link_libraries(compare_modern_chase_lev_park PRIVATE modern_runtime)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(compare_modern_chase_lev_futex PRIVATE modern_runtime)
endif()

if(COMPARE_WITH_TASKFLOW)
  if(NOT COMPARE_FETCH_DEPS)
    message(FATAL_ERROR "Enable COMPARE_FETCH_DEPS for pinned Taskflow")
  endif()
  FetchContent_Declare(compare_taskflow
    GIT_REPOSITORY https://github.com/taskflow/taskflow.git
    GIT_TAG 45366fe5bc4f2f8ec9aa590b40c504e296886865
    GIT_SHALLOW FALSE)
  FetchContent_GetProperties(compare_taskflow)
  if(NOT compare_taskflow_POPULATED)
    FetchContent_Populate(compare_taskflow)
  endif()
  compare_executable(compare_taskflow_binary TASKFLOW)
  target_include_directories(compare_taskflow_binary PRIVATE "${compare_taskflow_SOURCE_DIR}")
endif()

if(COMPARE_WITH_ONETBB)
  if(COMPARE_USE_SYSTEM_TBB)
    find_package(TBB CONFIG REQUIRED)
  else()
    if(NOT COMPARE_FETCH_DEPS)
      message(FATAL_ERROR "Pinned oneTBB requires COMPARE_FETCH_DEPS=ON")
    endif()
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(compare_onetbb
      GIT_REPOSITORY https://github.com/uxlfoundation/oneTBB.git
      GIT_TAG f1862f38f83568d96e814e469ab61f88336cc595
      GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(compare_onetbb)
  endif()
  compare_executable(compare_onetbb_binary ONETBB)
  target_link_libraries(compare_onetbb_binary PRIVATE TBB::tbb)
endif()

if(COMPARE_WITH_STDEXEC)
  if(NOT COMPARE_FETCH_DEPS)
    message(FATAL_ERROR "Enable COMPARE_FETCH_DEPS for pinned stdexec")
  endif()
  FetchContent_Declare(compare_stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG 697cd690f48c409071b023f33b53c6c369015fdb
    GIT_SHALLOW FALSE)
  FetchContent_GetProperties(compare_stdexec)
  if(NOT compare_stdexec_POPULATED)
    FetchContent_Populate(compare_stdexec)
  endif()
  compare_executable(compare_stdexec_binary STDEXEC)
  target_include_directories(compare_stdexec_binary PRIVATE "${compare_stdexec_SOURCE_DIR}/include")
endif()
