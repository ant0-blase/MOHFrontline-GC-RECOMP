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
