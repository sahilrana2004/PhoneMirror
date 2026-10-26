cmake_minimum_required(VERSION 3.28)

set(VCPKG_TARGET_TRIPLET x64-windows-static CACHE STRING "")
project(PhoneMirror VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

find_program(FXC fxc REQUIRED HINTS
  "$ENV{WindowsSdkVerBinPath}/x64"
  "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_SYSTEM_VERSION}/x64")

set(SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PRESENT_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/src/video/shaders/present.hlsl)
add_custom_command(
  OUTPUT ${SHADER_DIR}/present_vs.h ${SHADER_DIR}/present_ps.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
  COMMAND ${FXC} /nologo /O3 /T vs_5_0 /E vs_main /Vn g_present_vs /Fh ${SHADER_DIR}/present_vs.h ${PRESENT_HLSL}
  COMMAND ${FXC} /nologo /O3 /T ps_5_0 /E ps_main /Vn g_present_ps /Fh ${SHADER_DIR}/present_ps.h ${PRESENT_HLSL}
  DEPENDS ${PRESENT_HLSL}
  VERBATIM)

configure_file(src/app/version.h.in ${SHADER_DIR}/version.h @ONLY)

add_executable(PhoneMirror WIN32
  src/app/main.cpp
  src/app/app.cpp
  src/app/tray.cpp
  src/app/log.cpp
  src/ui/mirror_window.cpp
  src/video/renderer.cpp
  res/app.rc
  ${SHADER_DIR}/present_vs.h
  ${SHADER_DIR}/present_ps.h)

target_include_directories(PhoneMirror PRIVATE src res ${SHADER_DIR})
target_compile_definitions(PhoneMirror PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PhoneMirror PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc>)
target_link_options(PhoneMirror PRIVATE /MANIFEST:NO)
target_link_libraries(PhoneMirror PRIVATE d3d11 dxgi d2d1 dwrite dwmapi shell32)
