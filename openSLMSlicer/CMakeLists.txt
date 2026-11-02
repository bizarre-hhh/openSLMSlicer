cmake_minimum_required(VERSION 3.16)
project(openSLMSlicer LANGUAGES C CXX)

include(qt.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use this machine's installed Qt kit when CMake has not received a Qt prefix
# from Qt Creator, Visual Studio, or the command line.
if (NOT CMAKE_PREFIX_PATH AND EXISTS "D:/Qt/Qt5.14.2/5.14.2/msvc2017_64/lib/cmake/Qt5")
  list(PREPEND CMAKE_PREFIX_PATH "D:/Qt/Qt5.14.2/5.14.2/msvc2017_64")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Svg)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Svg)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

# GLFW and Dear ImGui remain the renderer inside the Qt central widget.
set(GLFW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/glfw")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/imgui")
set(APP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IMGUIZMO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/ImGuizmo")
set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/glad")
set(GLM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/glm")
set(ASSIMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/assimp")
set(STB_IMAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/stb_image")
# 品牌资源：logo 的 PNG（Qt 资源）与 ICO（Windows exe 图标）。
set(LOGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asset/logo")

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${GLFW_DIR}" "${CMAKE_BINARY_DIR}/_deps/glfw-build" EXCLUDE_FROM_ALL)

find_package(OpenGL REQUIRED)

if (WIN32)
  add_library(assimp_prebuilt SHARED IMPORTED GLOBAL)
  set_target_properties(assimp_prebuilt PROPERTIES
    IMPORTED_IMPLIB "${ASSIMP_DIR}/lib/assimp.lib"
    IMPORTED_LOCATION "${ASSIMP_DIR}/bin/assimp-vc140-mt.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${ASSIMP_DIR}/include"
  )
else()
  message(FATAL_ERROR "This example currently uses the copied Windows Assimp binary.")
endif()

# PySLM C++：仓库内的 pyslm_cpp/ 源码作为子工程一起编译，
# 保证编译器、MSVC 运行时库和 Debug/Release 配置与主程序一致。
set(PYSLM_CPP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pyslm_cpp"
  CACHE PATH "PySLM C++ 源码目录（包含 CMakeLists.txt）")

if (NOT EXISTS "${PYSLM_CPP_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "PySLM C++ 源码不存在：${PYSLM_CPP_SOURCE_DIR}。"
    "可用 -DPYSLM_CPP_SOURCE_DIR=<path> 指定源码目录。")
endif()
if (CMAKE_VERSION VERSION_LESS 3.24)
  message(FATAL_ERROR "编译 PySLM C++ 需要 CMake 3.24 或更高版本，当前为 ${CMAKE_VERSION}")
endif()

# PySLM 依赖随仓库裁剪好的 third_party 源码；缺失时提前给出可操作的提示。
foreach(_pyslm_dep_file
    "${PYSLM_CPP_SOURCE_DIR}/third_party/geos/CMakeLists.txt"
    "${PYSLM_CPP_SOURCE_DIR}/third_party/manifold/CMakeLists.txt"
    "${PYSLM_CPP_SOURCE_DIR}/third_party/clipper2/CPP/CMakeLists.txt")
  if (NOT EXISTS "${_pyslm_dep_file}")
    message(FATAL_ERROR
      "PySLM C++ 的 third_party 依赖不完整（缺少 ${_pyslm_dep_file}）。"
      "请检查 pyslm_cpp 目录是否完整，或用 -DPYSLM_CPP_SOURCE_DIR=<path> 指向完整源码树。")
  endif()
endforeach()

# support_engine 的悬垂识别/实体块支撑使用 PySLM 的 mesh Boolean 后端。
set(PYSLM_BUILD_MANIFOLD ON CACHE BOOL "PySLM: build the native mesh Boolean backend" FORCE)

# 部署 MSVC 运行时 DLL：从本机 VS 工具链的 redist 目录收集，保持输出目录可独立运行。
set(_openslm_crt_dlls "")
if (MSVC)
  set(_openslm_redist_roots "")
  if (CMAKE_GENERATOR_INSTANCE)
    # VS 生成器：CMAKE_GENERATOR_INSTANCE 指向 VS 安装根目录。
    list(APPEND _openslm_redist_roots "${CMAKE_GENERATOR_INSTANCE}/VC/Redist/MSVC")
  endif()
  if (DEFINED ENV{VCToolsRedistDir})
    # VS 开发者环境：.../VC/Redist/MSVC/<ver>/
    get_filename_component(_env_redist_root "$ENV{VCToolsRedistDir}/../.." ABSOLUTE)
    list(APPEND _openslm_redist_roots "${_env_redist_root}")
  endif()
  if (IS_ABSOLUTE "${CMAKE_CXX_COMPILER}")
    # 兼容直接指定绝对 cl.exe 路径：.../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe
    get_filename_component(_cl_host_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(_cl_host_dir "${_cl_host_dir}/.." ABSOLUTE)   # .../Hostx64
    get_filename_component(_cl_bin_dir "${_cl_host_dir}/.." ABSOLUTE)    # .../bin
    get_filename_component(_toolset_dir "${_cl_bin_dir}/.." ABSOLUTE)    # .../<ver>
    get_filename_component(_vc_root "${_toolset_dir}/../.." ABSOLUTE)    # .../VC
    list(APPEND _openslm_redist_roots "${_vc_root}/Redist/MSVC")
  endif()
  list(REMOVE_DUPLICATES _openslm_redist_roots)
  foreach(_redist_root ${_openslm_redist_roots})
    file(GLOB _openslm_crt_dirs
      "${_redist_root}/*/x64/Microsoft.VC*.CRT"
      "${_redist_root}/*/debug_nonredist/x64/Microsoft.VC*.DebugCRT")
    foreach(_crt_dir ${_openslm_crt_dirs})
      file(GLOB _crt_dir_dlls "${_crt_dir}/*.dll")
      list(APPEND _openslm_crt_dlls ${_crt_dir_dlls})
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES _openslm_crt_dlls)
  if (NOT _openslm_crt_dlls)
    message(WARNING
      "未找到 MSVC redist 目录，输出目录不会自动部署 CRT；"
      "运行时需要系统已安装 VC++ 运行库（Debug 需 VS 环境）。")
  endif()
endif()

# PySLM 是纯 C++ 工程，不应继承本工程的 Qt 自动生成/EditAndContinue 设置。
set(_openslm_saved_build_testing "${BUILD_TESTING}")
set(_openslm_saved_automoc "${CMAKE_AUTOMOC}")
set(_openslm_saved_autouic "${CMAKE_AUTOUIC}")
set(_openslm_saved_autorcc "${CMAKE_AUTORCC}")
set(_openslm_saved_msvc_debug_info "${CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}")
if (DEFINED BUILD_SHARED_LIBS)
  set(_openslm_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
  set(_openslm_had_build_shared_libs TRUE)
endif()

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)
if (DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
  # 上游用 /MP 并行编译，和 EditAndContinue(/ZI) 不兼容；第三方库也不需要 E&C。
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
endif()

add_subdirectory("${PYSLM_CPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/pyslm_cpp-build")

set(CMAKE_AUTOMOC "${_openslm_saved_automoc}")
set(CMAKE_AUTOUIC "${_openslm_saved_autouic}")
set(CMAKE_AUTORCC "${_openslm_saved_autorcc}")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "${_openslm_saved_msvc_debug_info}")
set(BUILD_TESTING "${_openslm_saved_build_testing}" CACHE BOOL "Build tests" FORCE)
if (_openslm_had_build_shared_libs)
  set(BUILD_SHARED_LIBS "${_openslm_saved_build_shared_libs}" CACHE BOOL "Build shared libraries" FORCE)
else()
  unset(BUILD_SHARED_LIBS CACHE)
endif()

function(openslm_copy_pyslm_runtime target)
  set(_runtime_dlls $<TARGET_FILE:pyslm_cpp> $<TARGET_FILE:geos> $<TARGET_FILE:geos_c>)
  if (TARGET manifold)
    list(APPEND _runtime_dlls $<TARGET_FILE:manifold>)
  endif()
  if (_openslm_crt_dlls)
    # 同时带上 Release/Debug CRT，不区分配置；多余的 DLL 不会被加载。
    list(APPEND _runtime_dlls ${_openslm_crt_dlls})
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_runtime_dlls}
      "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Copying PySLM runtime files")
endfunction()

set(PROJECT_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/../asset/icon/toolbar_icons.qrc"
  "${LOGO_DIR}/logo.qrc"
  "${IMGUIZMO_DIR}/ImGuizmo.cpp"
  "main.cpp"
  "project_file.cpp"
  "recent_projects.cpp"
  "welcome_dialog.cpp"
  "scene_editor_window.cpp"
  "export_slice_dialog.cpp"
  "slice_file_info_dialog.cpp"
  "scene_viewport.cpp"
  "slice_engine.cpp"
  "support_engine.cpp"
  "slice_export.cpp"
  "slice_report_export.cpp"
  "slmslice_format.cpp"
  "cli_format.cpp"
  "camera_controller.cpp"
  "view_cube.cpp"
  "view_toolbar.cpp"
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_demo.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
  "${GLAD_DIR}/src/glad.c"
  "${STB_IMAGE_DIR}/src/stb_image.cpp"
)

qt_add_executable(openSLMSlicer ${PROJECT_SOURCES})

# 把 logo 编译进 Windows exe：资源管理器、任务栏和文件属性使用 app.rc.in
# 中配置的图标与版本信息（图标路径在配置阶段写入绝对路径，避免 rc.exe
# 依赖工作目录解析相对路径）。
if (WIN32)
  set(OPENSLM_APP_ICON "${LOGO_DIR}/slm_logo.ico")
  if (NOT EXISTS "${OPENSLM_APP_ICON}")
    message(FATAL_ERROR "缺少应用图标文件：${OPENSLM_APP_ICON}。"
      "请从 asset/logo/slm_logo.png 重新生成多尺寸 .ico。")
  endif()
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc"
    @ONLY)
  target_sources(openSLMSlicer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
endif()

target_include_directories(openSLMSlicer PRIVATE
  "${APP_SOURCE_DIR}"
  "${IMGUIZMO_DIR}"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${GLAD_DIR}/include"
  "${GLM_DIR}/include"
  "${ASSIMP_DIR}/include"
  "${STB_IMAGE_DIR}/include"
)

target_compile_definitions(openSLMSlicer PRIVATE _CRT_SECURE_NO_WARNINGS)
if (MSVC)
  target_compile_options(openSLMSlicer PRIVATE /utf-8)
endif()
target_link_libraries(openSLMSlicer PRIVATE
  glfw
  OpenGL::GL
  imm32
  assimp_prebuilt
  pyslm_cpp
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Svg
)

if (WIN32)
  set_target_properties(openSLMSlicer PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

set(MODEL_RESOURCE_OUTPUT_DIR "$<TARGET_FILE_DIR:openSLMSlicer>/resources/asset/backpack")
set(SHADER_RESOURCE_OUTPUT_DIR "$<TARGET_FILE_DIR:openSLMSlicer>/resources/shaders")
add_custom_command(TARGET openSLMSlicer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/backpack"
    "${MODEL_RESOURCE_OUTPUT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_RESOURCE_OUTPUT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/shaders/model.vs"
    "${SHADER_RESOURCE_OUTPUT_DIR}/model.vs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/shaders/model.fs"
    "${SHADER_RESOURCE_OUTPUT_DIR}/model.fs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/shaders/base_plate.vs"
    "${SHADER_RESOURCE_OUTPUT_DIR}/base_plate.vs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/shaders/base_plate.fs"
    "${SHADER_RESOURCE_OUTPUT_DIR}/base_plate.fs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/shaders/stl.vs"
    "${SHADER_RESOURCE_OUTPUT_DIR}/stl.vs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/../asset/shaders/stl.fs"
    "${SHADER_RESOURCE_OUTPUT_DIR}/stl.fs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${ASSIMP_DIR}/bin/assimp-vc140-mt.dll"
    "$<TARGET_FILE_DIR:openSLMSlicer>/assimp-vc140-mt.dll"
)
if (WIN32)
  get_target_property(_qt_core_release Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION_RELEASE)
  if (NOT _qt_core_release)
    get_target_property(_qt_core_release Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION)
  endif()
  if (_qt_core_release)
    get_filename_component(_qt_bin_dir "${_qt_core_release}" DIRECTORY)
  endif()
  find_program(QT_WINDEPLOYQT_EXECUTABLE
    NAMES windeployqt windeployqt.exe
    HINTS "${_qt_bin_dir}" ${CMAKE_PREFIX_PATH}
    PATH_SUFFIXES bin
  )
  if (QT_WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET openSLMSlicer POST_BUILD
      COMMAND "${QT_WINDEPLOYQT_EXECUTABLE}" --no-translations "$<TARGET_FILE:openSLMSlicer>"
      COMMENT "Deploying Qt runtime files"
    )
  endif()
endif()

# Copy the PySLM runtime (and the matching MSVC runtime in fallback mode) after
# Qt deployment so its DLLs take precedence over other copies next to the exe.
openslm_copy_pyslm_runtime(openSLMSlicer)

if (BUILD_TESTING)
  set(PROJECT_TEST_SOURCES ${PROJECT_SOURCES})
  list(REMOVE_ITEM PROJECT_TEST_SOURCES "main.cpp")
  add_executable(project_tests "tests/project_tests.cpp" ${PROJECT_TEST_SOURCES})
  get_target_property(_app_include_dirs openSLMSlicer INCLUDE_DIRECTORIES)
  get_target_property(_app_link_libraries openSLMSlicer LINK_LIBRARIES)
  target_include_directories(project_tests PRIVATE ${_app_include_dirs})
  target_link_libraries(project_tests PRIVATE ${_app_link_libraries})
  target_compile_definitions(project_tests PRIVATE _CRT_SECURE_NO_WARNINGS
    TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../asset")
  if (MSVC)
    target_compile_options(project_tests PRIVATE /utf-8)
  endif()
  set_target_properties(project_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  add_dependencies(project_tests openSLMSlicer)
  openslm_copy_pyslm_runtime(project_tests)
  add_test(NAME project_roundtrip COMMAND project_tests)
  add_test(NAME project_ui_workflow COMMAND project_tests --ui)
  set_tests_properties(project_ui_workflow PROPERTIES TIMEOUT 45
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QPA_PLATFORM_PLUGIN_PATH=${_qt_bin_dir}/../plugins")
  set_tests_properties(project_roundtrip PROPERTIES TIMEOUT 45
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=${_qt_bin_dir}/../plugins")

  add_executable(slicing_tests "tests/slicing_tests.cpp" "slice_engine.cpp")
  target_include_directories(slicing_tests PRIVATE
    "${APP_SOURCE_DIR}"
  )
  target_link_libraries(slicing_tests PRIVATE pyslm_cpp)
  target_compile_definitions(slicing_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  if (MSVC)
    target_compile_options(slicing_tests PRIVATE /utf-8)
  endif()
  set_target_properties(slicing_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  openslm_copy_pyslm_runtime(slicing_tests)
  add_test(NAME slicing_engine COMMAND slicing_tests)

  add_executable(support_tests "tests/support_tests.cpp" "support_engine.cpp")
  target_include_directories(support_tests PRIVATE
    "${APP_SOURCE_DIR}"
    "${GLM_DIR}/include"
  )
  target_link_libraries(support_tests PRIVATE pyslm_cpp)
  target_compile_definitions(support_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  if (MSVC)
    target_compile_options(support_tests PRIVATE /utf-8)
  endif()
  set_target_properties(support_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  openslm_copy_pyslm_runtime(support_tests)
  add_test(NAME support_engine COMMAND support_tests)

  add_executable(slice_export_tests "tests/slice_export_tests.cpp"
    "slice_export.cpp" "slice_report_export.cpp" "slmslice_format.cpp"
    "cli_format.cpp")
  target_include_directories(slice_export_tests PRIVATE "${APP_SOURCE_DIR}")
  target_link_libraries(slice_export_tests PRIVATE Qt${QT_VERSION_MAJOR}::Core)
  target_compile_definitions(slice_export_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  if (MSVC)
    target_compile_options(slice_export_tests PRIVATE /utf-8)
  endif()
  set_target_properties(slice_export_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME slice_export COMMAND slice_export_tests)
  set_tests_properties(slice_export PROPERTIES TIMEOUT 45)
endif()
