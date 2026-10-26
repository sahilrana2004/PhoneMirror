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

find_package(OpenSSL REQUIRED)
find_package(unofficial-libplist CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

# Vendored UxPlay protocol layer (C). Its POSIX gaps are filled by src/airplay/posix; dnssd.c is
# replaced by src/airplay/dnssd_mdns.cpp, linked into the executable.
file(GLOB UXPLAY_SOURCES CONFIGURE_DEPENDS
  src/airplay/uxplay/*.c src/airplay/uxplay/playfair/*.c src/airplay/uxplay/llhttp/*.c)
add_library(uxplay STATIC ${UXPLAY_SOURCES})
target_include_directories(uxplay
  PUBLIC src/airplay/uxplay
  PRIVATE src/airplay/posix src/airplay/uxplay/playfair src/airplay/uxplay/llhttp)
target_compile_definitions(uxplay
  PUBLIC OPENSSL_API_COMPAT=0x10101000L
  PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _WINSOCK_DEPRECATED_NO_WARNINGS
          NOHOLD PLIST_210 PLIST_230)
target_compile_options(uxplay PRIVATE /std:c17 /W0 /utf-8
  /FI${CMAKE_CURRENT_SOURCE_DIR}/src/airplay/posix/posix_compat.h)
target_link_libraries(uxplay PUBLIC OpenSSL::Crypto unofficial::libplist::libplist ws2_32 iphlpapi)

add_executable(PhoneMirror WIN32
  src/app/main.cpp
  src/app/app.cpp
  src/app/tray.cpp
  src/app/log.cpp
  src/app/settings.cpp
  src/app/session.cpp
  src/app/file_source.cpp
  src/net/firewall.cpp
  src/net/wifi_tuning.cpp
  src/record/recorder.cpp
  src/record/screenshot.cpp
  src/audio/audio_pipeline.cpp
  src/airplay/receiver.cpp
  src/airplay/dnssd_mdns.cpp
  src/input/input_capture.cpp
  src/input/keymap.cpp
  src/input/winbt_backend.cpp
  src/lab/lab_phone.cpp
  src/lab/lab_runner.cpp
  src/mdns/responder.cpp
  src/ui/mirror_window.cpp
  src/video/renderer.cpp
  src/video/decoder.cpp
  src/video/pipeline.cpp
  res/app.rc
  ${SHADER_DIR}/present_vs.h
  ${SHADER_DIR}/present_ps.h)

target_include_directories(PhoneMirror PRIVATE src res ${SHADER_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_directories(PhoneMirror PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_compile_definitions(PhoneMirror PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PhoneMirror PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc>)
target_link_options(PhoneMirror PRIVATE /MANIFEST:NO)
target_link_libraries(PhoneMirror PRIVATE uxplay ${FFMPEG_LIBRARIES}
  d3d11 dxgi d2d1 dwrite dwmapi shell32 iphlpapi ws2_32 ole32 oleaut32 avrt windowsapp wlanapi windowscodecs
  dbghelp winmm dcomp)

# M7 spike: can Windows itself be a BLE HID peripheral? (console tool, not shipped)
add_executable(bt_spike tools/bt_spike/main.cpp)
target_compile_options(bt_spike PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(bt_spike PRIVATE windowsapp)
