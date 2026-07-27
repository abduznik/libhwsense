cmake_minimum_required(VERSION 3.15)
project(libhwsense VERSION 0.1.0 LANGUAGES C)

# Windows only
if(NOT WIN32)
    message(FATAL_ERROR "libhwsense requires Windows (WinRing0 kernel driver)")
endif()

# Source files (no PawnIO)
set(HWSENSE_SOURCES
    src/driver/driver.c
    src/amd/amd_zen.c
    src/intel/intel.c
)

# ── Static library ──────────────────────────────────────────────────
add_library(hwsense_static STATIC ${HWSENSE_SOURCES})
target_include_directories(hwsense_static
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(hwsense_static PRIVATE kernel32 advapi32)
set_target_properties(hwsense_static PROPERTIES
    OUTPUT_NAME hwsense
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ── Shared library (DLL) ───────────────────────────────────────────
add_library(hwsense_shared SHARED ${HWSENSE_SOURCES})
target_include_directories(hwsense_shared
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(hwsense_shared PRIVATE kernel32 advapi32)
set_target_properties(hwsense_shared PROPERTIES
    OUTPUT_NAME hwsense
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(hwsense_shared PRIVATE HWSENSE_DLL BUILDING_HWSENSE_DLL)
# Export macro: HWSENSE_API expands to __declspec(dllexport) in the DLL,
# __declspec(dllimport) for consumers.

# ── Example: read_cpu_temp ──────────────────────────────────────────
add_executable(read_cpu_temp examples/read_cpu_temp.c)
target_link_libraries(read_cpu_temp PRIVATE hwsense_static)
