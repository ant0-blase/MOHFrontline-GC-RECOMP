cmake_minimum_required(VERSION 3.16)
project(moderngekko_multi_recomp_module C)

set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID")
set(MULTI_MANIFEST "" CACHE FILEPATH "Generated multi-image CMake manifest")
set(GXRUNTIME_DIR "" CACHE PATH "GXRuntime checkout")
set(CHASSIS_ABI_DIR "" CACHE PATH "StaticRecomp ABI directory")
set(RECOMPCORE_MODULE_OPT_LEVEL "3" CACHE STRING "Optimization level 0..3")

if(NOT GAME_ID OR NOT MULTI_MANIFEST OR NOT EXISTS "${MULTI_MANIFEST}")
  message(FATAL_ERROR "GAME_ID and an existing MULTI_MANIFEST are required")
endif()
if(NOT EXISTS "${GXRUNTIME_DIR}")
  message(FATAL_ERROR "GXRuntime not found: ${GXRUNTIME_DIR}")
endif()
if(NOT EXISTS "${CHASSIS_ABI_DIR}")
  message(FATAL_ERROR "StaticRecomp ABI directory not found: ${CHASSIS_ABI_DIR}")
endif()
include("${MULTI_MANIFEST}")

if(NOT MULTI_EXPORT_SOURCE OR NOT EXISTS "${MULTI_EXPORT_SOURCE}")
  message(FATAL_ERROR "MULTI_EXPORT_SOURCE is missing")
endif()
if(NOT MULTI_CHUNK_INPUTS OR NOT MULTI_ADAPTER_INPUTS)
  message(FATAL_ERROR "multi-image manifest has no generated inputs")
endif()

set(MODULE_NAME "g${GAME_ID}_recomp")
add_library(${MODULE_NAME} SHARED
  "${MULTI_EXPORT_SOURCE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/moh_frontline_enhancements.c"
  ${MULTI_ADAPTER_INPUTS}
  "${GXRUNTIME_DIR}/src/core/cpu.c"
  "${GXRUNTIME_DIR}/src/core/cpu_exception.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c"
  ${MULTI_CHUNK_INPUTS})

set_target_properties(${MODULE_NAME} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${MODULE_NAME}"
  UNITY_BUILD OFF
  C_VISIBILITY_PRESET hidden)

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")

# MOH_GMFE69_GAMECUBE_MEM1_ONLY
# GMFE69 is a retail GameCube title: MEM1 is fixed at 24 MiB and there is no
# Wii MEM2. GXRuntime's get_ram_ptr() already has a dedicated compile-time
# GameCube path which keeps non-MEM1 addresses on the existing external/MMIO
# fallback while removing the unnecessary EXRAM/MEM2 check from every inline
# memory access.
if(GAME_ID STREQUAL "GMFE69")
  target_compile_definitions(${MODULE_NAME} PRIVATE GXRUNTIME_GAMECUBE_MEM1_ONLY=1)
endif()
target_include_directories(${MODULE_NAME} PRIVATE
  "${GXRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${MULTI_WORK_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT WIN32)
  target_link_libraries(${MODULE_NAME} PRIVATE m)
endif()
if(UNIX AND NOT APPLE)
  target_link_options(${MODULE_NAME} PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module.exports")
  set_property(TARGET ${MODULE_NAME} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module.exports")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties("${GXRUNTIME_DIR}/src/core/cpu.c" PROPERTIES COMPILE_OPTIONS "-fvisibility=default")
  target_compile_options(${MODULE_NAME} PRIVATE -O${RECOMPCORE_MODULE_OPT_LEVEL} -ffp-contract=off -fno-fast-math)
endif()
