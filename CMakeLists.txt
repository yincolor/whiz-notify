cmake_minimum_required(VERSION 3.28)

project(whiz-notify VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# 核心库 whiz-notify（纯 C++23，不依赖任何 GUI 框架）
# ---------------------------------------------------------------------------

add_library(whiz-notify STATIC
    src/api.cpp
    src/event_loop.cpp
)
add_library(whiz-notify::whiz-notify ALIAS whiz-notify)

target_include_directories(whiz-notify PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(whiz-notify PUBLIC cxx_std_23)
set_target_properties(whiz-notify PROPERTIES CXX_EXTENSIONS OFF)

# 库自身告警；Debug 下 -Werror（见设计文档 §8.3）。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(whiz-notify PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Debug>:-Werror>)
elseif(MSVC)
    target_compile_options(whiz-notify PRIVATE /W4 $<$<CONFIG:Debug>:/WX>)
endif()

if(WIN32)
    target_sources(whiz-notify PRIVATE src/backend_windows.cpp)
    target_link_libraries(whiz-notify PRIVATE windowsapp)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(whiz-notify PRIVATE src/backend_macos.mm)
    target_link_libraries(whiz-notify PRIVATE
        "-framework UserNotifications"
        "-framework Foundation")
else()
    target_sources(whiz-notify PRIVATE src/backend_linux.cpp)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    target_link_libraries(whiz-notify PRIVATE PkgConfig::DBUS)
endif()
