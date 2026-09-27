cmake_minimum_required(VERSION 3.20)
project(MOHFrontlineLauncher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(MOH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(SDL_ROOT "${MOH_ROOT}/ModernGekko/vendor/dolphin/Externals/SDL/SDL")
set(IMGUI_ROOT "${MOH_ROOT}/ModernGekko/vendor/dolphin/Externals/imgui/imgui")

if(NOT EXISTS "${SDL_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "Vendored SDL3 source is missing: ${SDL_ROOT}")
endif()
if(NOT EXISTS "${IMGUI_ROOT}/imgui.cpp")
  message(FATAL_ERROR "Vendored Dear ImGui source is missing: ${IMGUI_ROOT}")
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
  # XTest is not used by the standalone recompilation launcher. Keeping it
  # disabled avoids pulling an unnecessary libXtst development dependency
  # into portable/CI builds while preserving normal X11 window/input support.
  set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${SDL_ROOT}" "${CMAKE_BINARY_DIR}/_deps/sdl3" EXCLUDE_FROM_ALL)

set(MOH_NOD_VERSION "v2.0.0-alpha.12" CACHE STRING "encounter/nod version")
find_package(nod QUIET CONFIG)
if(NOT TARGET nod::nod)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _moh_nod_arch)
  set(_moh_nod_platform "")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _moh_nod_arch MATCHES "^(x86_64|amd64)$")
    set(_moh_nod_platform "linux-x86_64")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND _moh_nod_arch MATCHES "^(x86_64|amd64)$")
    set(_moh_nod_platform "windows-x86_64")
  endif()
  if(NOT _moh_nod_platform)
    message(FATAL_ERROR "No prebuilt encounter/nod package for this platform")
  endif()
  include(FetchContent)
  FetchContent_Declare(moh_nod
    URL "https://github.com/encounter/nod/releases/download/${MOH_NOD_VERSION}/libnod-${_moh_nod_platform}.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(moh_nod)
  set(_moh_saved_shared "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF)
  find_package(nod REQUIRED CONFIG
    PATHS "${moh_nod_SOURCE_DIR}"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH)
  set(BUILD_SHARED_LIBS "${_moh_saved_shared}")
endif()

add_executable(MOHFrontline-DiscCache src/disc_cache.cpp)
target_link_libraries(MOHFrontline-DiscCache PRIVATE nod::nod)
set_target_properties(MOHFrontline-DiscCache PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${MOH_ROOT}/launcher/bin"
)
add_executable(MOHFrontline-Launcher
  src/main.cpp
  "${IMGUI_ROOT}/imgui.cpp"
  "${IMGUI_ROOT}/imgui_draw.cpp"
  "${IMGUI_ROOT}/imgui_tables.cpp"
  "${IMGUI_ROOT}/imgui_widgets.cpp"
  "${IMGUI_ROOT}/backends/imgui_impl_sdl3.cpp"
  "${IMGUI_ROOT}/backends/imgui_impl_sdlrenderer3.cpp"
)

target_include_directories(MOHFrontline-Launcher PRIVATE
  "${IMGUI_ROOT}"
  "${IMGUI_ROOT}/backends"
)
target_link_libraries(MOHFrontline-Launcher PRIVATE SDL3::SDL3-static)

if(WIN32)
  set_target_properties(MOHFrontline-Launcher PROPERTIES WIN32_EXECUTABLE TRUE)
  target_compile_definitions(MOHFrontline-Launcher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set_target_properties(MOHFrontline-Launcher PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${MOH_ROOT}/launcher/bin"
)
