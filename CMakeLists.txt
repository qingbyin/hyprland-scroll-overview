cmake_minimum_required(VERSION 3.27)

project(scrolloverview
    DESCRIPTION "scrolloverview plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

# Plugin sources live at the root; never pull tests or build trees into it.
file(GLOB SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(scrolloverview SHARED ${SRC})

set(VERSION_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(VERSION_HEADER "${VERSION_HEADER_DIR}/PluginVersion.hpp")
add_custom_target(scrolloverview-version ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${VERSION_HEADER_DIR}"
    COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-plugin-version.sh" "${VERSION_HEADER}" "${CMAKE_CURRENT_SOURCE_DIR}"
    BYPRODUCTS "${VERSION_HEADER}"
    VERBATIM
)
add_dependencies(scrolloverview scrolloverview-version)
target_include_directories(scrolloverview PRIVATE "${VERSION_HEADER_DIR}")

find_package(PkgConfig REQUIRED)
pkg_search_module(lua REQUIRED IMPORTED_TARGET "lua5.4>=5.4" "lua>=5.4")
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
    dbus-1
)
target_link_libraries(scrolloverview PRIVATE rt PkgConfig::deps PkgConfig::lua)

install(TARGETS scrolloverview)
