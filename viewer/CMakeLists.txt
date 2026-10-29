cmake_minimum_required(VERSION 3.24)
project(BoardScopeViewer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)
include(ExternalProject)

# Every dependency is pinned to a release archive and its hash, and linked statically,
# so the viewer is one file with nothing to install beside it.

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
  URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  URL_HASH SHA256=21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99)

# libjpeg-turbo refuses to be built with add_subdirectory(), so FetchContent only downloads it
# (SOURCE_SUBDIR points nowhere) and ExternalProject builds it with the same compiler.
FetchContent_Declare(libjpeg_turbo
  URL https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz
  URL_HASH SHA256=6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e
  SOURCE_SUBDIR built-by-external-project)

FetchContent_MakeAvailable(SDL3 imgui libjpeg_turbo)

set(jpeg_build ${CMAKE_BINARY_DIR}/libjpeg-turbo)
set(jpeg_args
  -DCMAKE_BUILD_TYPE=Release
  # Never installed; its Windows default (C:/libjpeg-turbo-gcc64) breaks configuring on a Mac.
  -DCMAKE_INSTALL_PREFIX=${jpeg_build}/unused-install
  -DENABLE_SHARED=OFF
  -DENABLE_STATIC=ON
  -DWITH_TURBOJPEG=ON
  -DWITH_TOOLS=OFF
  -DWITH_TESTS=OFF)
foreach(var CMAKE_TOOLCHAIN_FILE CMAKE_C_COMPILER CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET)
  if(${var})
    list(APPEND jpeg_args "-D${var}=${${var}}")
  endif()
endforeach()
set(jpeg_library ${jpeg_build}/${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX})
ExternalProject_Add(libjpeg_turbo_build
  SOURCE_DIR ${libjpeg_turbo_SOURCE_DIR}
  BINARY_DIR ${jpeg_build}
  CMAKE_ARGS ${jpeg_args}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static --config Release
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${jpeg_library})
add_library(turbojpeg STATIC IMPORTED)
set_target_properties(turbojpeg PROPERTIES
  IMPORTED_LOCATION ${jpeg_library}
  INTERFACE_INCLUDE_DIRECTORIES ${libjpeg_turbo_SOURCE_DIR}/src)
add_dependencies(turbojpeg libjpeg_turbo_build)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)

add_executable(BoardScope WIN32
  src/main.cpp
  src/link.cpp
  src/usbmux.cpp
  src/plist.cpp
  src/net.cpp)
target_link_libraries(BoardScope PRIVATE imgui turbojpeg SDL3::SDL3-static)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(BoardScope PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  target_link_libraries(BoardScope PRIVATE ws2_32)
  if(MINGW)
    # No MinGW runtime DLLs beside the .exe.
    target_link_options(BoardScope PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
  endif()
endif()
