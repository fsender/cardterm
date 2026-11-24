cmake_minimum_required(VERSION 3.16)
project(cardterm VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------- deps
# LovyanGFX (vendored)
set(LGFX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/lib/LovyanGFX-1.3.0)

# LGFX translation units
file(GLOB LGFX_SOURCES
  ${LGFX_ROOT}/src/lgfx/utility/*.c
  ${LGFX_ROOT}/src/lgfx/v1/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/panel/*.cpp
  ${LGFX_ROOT}/src/lgfx/v1/touch/*.cpp
)

# ---------------------------------------------------------------- target
# 设备版 cardterm v0.3.2：板载 320x170 屏（/dev/fb0）+ 独占显示会话
add_executable(cardterm
  src/main.cpp
  src/ui.cpp
  src/keyboard.cpp
  src/terminal.cpp
  src/config.cpp
  src/session.cpp
  src/exclusive_display.cpp
  ${LGFX_SOURCES}
)
target_compile_definitions(cardterm PRIVATE LGFX_LINUX_FB)
target_include_directories(cardterm PRIVATE ${LGFX_ROOT}/src)
target_link_libraries(cardterm PRIVATE pthread util)   # util: openpty(3)
# 版本号随 CMake project() 自动同步（横幅/帮助等引用 CARDTERM_VERSION）
target_compile_definitions(cardterm PRIVATE CARDTERM_VERSION="${PROJECT_VERSION}")
