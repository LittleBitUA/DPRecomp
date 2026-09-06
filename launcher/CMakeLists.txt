cmake_minimum_required(VERSION 3.20)
project(DPLauncher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
  message(FATAL_ERROR "DPLauncher requires MSVC (Windows-only)")
endif()

add_executable(PlayDeadlyPremonition WIN32
  src/main.cpp
  src/resources.rc
)
set(TGT PlayDeadlyPremonition)

target_compile_definitions(${TGT} PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_compile_options(${TGT} PRIVATE
  /W3
  /utf-8
  /EHsc
)

target_link_libraries(${TGT} PRIVATE
  gdiplus
  shlwapi
  comctl32
  user32
  gdi32
  shell32
  winmm
  dwmapi
)

set_target_properties(${TGT} PROPERTIES
  OUTPUT_NAME "PlayDeadlyPremonition"
  VS_GLOBAL_VcpkgEnabled "false"
)
