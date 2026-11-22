# joyride launcher: installs the games (your own copies), builds joyride from GTA's executable, sets it up and
# starts it. Separate from joyride's own build, which needs the generated
# code first:
#   cmake -S launcher -B build-launcher -G Ninja && cmake --build build-launcher
cmake_minimum_required(VERSION 3.21)
project(joyride_launcher CXX)
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
