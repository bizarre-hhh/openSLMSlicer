# CMakeList.txt: 顶层 CMake 项目文件，在此处执行全局配置
# 并包含子项目。
#
cmake_minimum_required (VERSION 3.24)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("openSLMSlicer")

# 本工程没有安装步骤：跳过第三方库的 install 规则，
# 避免 VS 解决方案里生成大量用不到的 INSTALL 辅助工程。
set(CMAKE_SKIP_INSTALL_RULES TRUE)

# 只启用测试，不使用 CTest 的 Dashboard 目标（Continuous/Experimental/
# Nightly/NightlyMemoryCheck），减少 VS 里无用的辅助工程。
option(BUILD_TESTING "Build the testing tree." ON)
if (BUILD_TESTING)
  enable_testing()
endif()

# 包含子项目。
add_subdirectory ("openSLMSlicer")
