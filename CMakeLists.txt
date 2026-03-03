cmake_minimum_required(VERSION 3.27)

project(hyprloading
    DESCRIPTION "Startup notification with bouncing app icon near cursor"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(GLOB_RECURSE SRC "src/*.cpp")

add_library(hyprloading SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
)
target_link_libraries(hyprloading PRIVATE rt PkgConfig::deps)

install(TARGETS hyprloading)
