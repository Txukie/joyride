# joyride launcher: installs the games from your own copies (a Steam
# install, an installer, a disc image), builds joyride from GTA's executable, sets it up and
# starts it. Separate from joyride's own build, which needs the generated
# code first:
#   cmake -S launcher -B build-launcher -G Ninja && cmake --build build-launcher
cmake_minimum_required(VERSION 3.21)
project(joyride_launcher VERSION 1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(LibArchive REQUIRED)
find_package(ZLIB REQUIRED)

# the installing, without Qt: shared by the launcher and joyride-setup
add_library(joyride_setup STATIC
    src/core/archive.cpp
    src/core/bincue.cpp
    src/core/games.cpp
    src/core/is5cab.cpp)
target_link_libraries(joyride_setup PUBLIC LibArchive::LibArchive ZLIB::ZLIB)
target_compile_options(joyride_setup PRIVATE -Wall -Wextra)

add_executable(joyride-setup src/cli/main.cpp)
target_link_libraries(joyride-setup PRIVATE joyride_setup)

# the launcher itself: Qt Quick, controllers through SDL3
find_package(Qt6 6.5 COMPONENTS Quick QuickControls2)
find_package(SDL3 CONFIG)
if(Qt6_FOUND AND SDL3_FOUND)
    qt_standard_project_setup(REQUIRES 6.5)
    qt_add_executable(joyride-launcher
        src/app/main.cpp
        src/app/backend.cpp
        src/app/gamepad.cpp)
    if(WIN32)
        enable_language(RC)
        target_sources(joyride-launcher PRIVATE joyride.rc)                     # its icon
        set_target_properties(joyride-launcher PROPERTIES WIN32_EXECUTABLE ON)  # no console window
    endif()
    # cross-compiled (Windows, cmake/mingw-w64.cmake), the QML is not
    # compiled ahead: this machine's qmlcachegen writes for its own Qt's
    # private headers, which a patch release apart do not match
    set(JOYRIDE_QML_OPTS)
    if(CMAKE_CROSSCOMPILING)
        set(JOYRIDE_QML_OPTS NO_CACHEGEN)
    endif()
    qt_add_qml_module(joyride-launcher
        URI JoyrideLauncher
        VERSION 1.0
        ${JOYRIDE_QML_OPTS}
        QML_FILES qml/Main.qml qml/BigButton.qml qml/GameCard.qml)
    # joyride's own art (art/, made by tools/art.py): the background, the icon
    qt_add_resources(joyride-launcher "art" PREFIX "/" BASE ".."
        FILES ../art/background.png ../art/joyride-16.png ../art/joyride-32.png ../art/joyride-48.png
              ../art/joyride-64.png ../art/joyride-128.png ../art/joyride-256.png ../art/controls-dark.png)
    target_link_libraries(joyride-launcher PRIVATE joyride_setup Qt6::Quick Qt6::QuickControls2 SDL3::SDL3)
    # where joyride's checkout is: preparing runs its tools/lift.sh and build
    get_filename_component(JOYRIDE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    target_compile_definitions(joyride-launcher PRIVATE JOYRIDE_SOURCE_DIR="${JOYRIDE_SOURCE_DIR}" JOYRIDE_VERSION="${PROJECT_VERSION}")
    target_compile_options(joyride-launcher PRIVATE -Wall -Wextra)
else()
    message(WARNING "Qt 6 (Quick, QuickControls2) or SDL3 not found: only joyride-setup is built")
endif()
