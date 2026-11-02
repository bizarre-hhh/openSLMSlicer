# PySLM C++ 0.6.1 — vendored minimal source subset for openSLMSlicer.
# 只保留编译 PySLM C++ 所需的源码：src/、include/ 和 third_party/。
# 示例/测试/文档/安装规则/构建产物均已移除；许可见 LICENSE / THIRD_PARTY_NOTICES.md。
cmake_minimum_required(VERSION 3.24)
project(PySLM_CPP VERSION 0.6.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# support_engine 的自动支撑需要 mesh Boolean 后端。
option(PYSLM_BUILD_MANIFOLD "Build the pinned native mesh Boolean backend" ON)

foreach(dep clipper2 geos)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dep}/CMakeLists.txt")
    if(NOT dep STREQUAL "clipper2" OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clipper2/CPP/CMakeLists.txt")
      message(FATAL_ERROR "Missing ${dep}: expected prepared third_party sources")
    endif()
  endif()
endforeach()
if(PYSLM_BUILD_MANIFOLD AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/manifold/CMakeLists.txt")
  message(FATAL_ERROR "Missing manifold: expected prepared third_party sources")
endif()

if(MSVC)
  add_compile_options(/utf-8 /MP8)
endif()

# 第三方子工程：统一构建为共享库，关闭其测试与 benchmark。
set(BUILD_SHARED_LIBS ON CACHE BOOL "Build native shared libraries" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Skip third-party tests" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "Skip third-party benchmarks" FORCE)
set(GEOS_BUILD_DEVELOPER OFF CACHE BOOL "Disable GEOS developer-only settings" FORCE)
add_subdirectory(third_party/geos third_party/geos EXCLUDE_FROM_ALL)

add_library(pyslm_clipper STATIC
  third_party/clipper2/CPP/Clipper2Lib/src/clipper.engine.cpp
  third_party/clipper2/CPP/Clipper2Lib/src/clipper.offset.cpp
  third_party/clipper2/CPP/Clipper2Lib/src/clipper.rectclip.cpp)
target_include_directories(pyslm_clipper PUBLIC third_party/clipper2/CPP/Clipper2Lib/include)
target_compile_definitions(pyslm_clipper PUBLIC USINGZ=1)

add_library(pyslm_triangle STATIC third_party/triangle-c/triangle.c)
target_compile_definitions(pyslm_triangle PRIVATE VOID=void REAL=double NO_TIMER=1 TRILIBRARY=1 ANSI_DECLARATORS=1)
target_include_directories(pyslm_triangle PUBLIC third_party/triangle-c)

add_library(pyslm_fitpack STATIC third_party/fitpack/dfitpack.c)
target_include_directories(pyslm_fitpack PUBLIC third_party/fitpack)

add_library(pyslm_minpack STATIC third_party/minpack/minpack.c)
target_include_directories(pyslm_minpack PUBLIC third_party/minpack)

file(GLOB QHULL_SOURCES third_party/qhull/src/libqhull_r/*.c)
add_library(pyslm_qhull STATIC ${QHULL_SOURCES})
target_include_directories(pyslm_qhull PUBLIC third_party/qhull/src)

if(PYSLM_BUILD_MANIFOLD)
  set(MANIFOLD_CROSS_SECTION OFF CACHE BOOL "Only mesh Boolean backend is required" FORCE)
  set(MANIFOLD_PAR OFF CACHE BOOL "Deterministic serial backend" FORCE)
  set(MANIFOLD_TEST OFF CACHE BOOL "Skip upstream tests" FORCE)
  set(MANIFOLD_PYBIND OFF CACHE BOOL "No Python runtime bindings" FORCE)
  set(MANIFOLD_CBIND OFF CACHE BOOL "Use native C++ mesh API" FORCE)
  set(MANIFOLD_DOWNLOADS OFF CACHE BOOL "Sources must be prepared explicitly" FORCE)
  add_subdirectory(third_party/manifold third_party/manifold EXCLUDE_FROM_ALL)
endif()

foreach(native_target geos geos_c manifold)
  if(TARGET ${native_target})
    set_target_properties(${native_target} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  endif()
endforeach()

file(GLOB PYSLM_SOURCES CONFIGURE_DEPENDS src/*.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/numpy_sort_avx2.cpp")
  if(MSVC)
    set_source_files_properties(src/numpy_sort_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(src/numpy_sort_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mpopcnt")
  endif()
endif()

add_library(pyslm_cpp SHARED ${PYSLM_SOURCES})
add_library(PySLM::pyslm_cpp ALIAS pyslm_cpp)
target_compile_definitions(pyslm_cpp PRIVATE PYSLM_CPP_BUILD)
target_include_directories(pyslm_cpp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(pyslm_cpp PRIVATE
  pyslm_clipper pyslm_triangle pyslm_fitpack pyslm_minpack pyslm_qhull
  GEOS::geos_c opengl32 gdi32 user32)
target_include_directories(pyslm_cpp PRIVATE third_party/earcut/include)
target_include_directories(pyslm_cpp PRIVATE third_party)
if(PYSLM_BUILD_MANIFOLD)
  target_link_libraries(pyslm_cpp PRIVATE manifold)
  target_compile_definitions(pyslm_cpp PRIVATE PYSLM_HAS_MANIFOLD=1)
endif()
if(MSVC)
  target_compile_options(pyslm_cpp PRIVATE /W4 /wd4251 /wd4275)
endif()
