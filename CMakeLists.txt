cmake_minimum_required(VERSION 3.15)
project(libhwsense C)

# Windows only
if(NOT WIN32)
    message(FATAL_ERROR "libhwsense requires Windows (WinRing0 kernel driver)")
endif()

# Define HWSENSE_DLL_EXPORTS for DLL builds
add_definitions(-DHWSENSE_DLL_EXPORTS)

# Shared library: hwsense.dll
add_library(hwsense SHARED
    src/core/driver.c
    src/core/api.c
    src/core/win_sysstats.c
    src/core/wmi.c
    src/core/hwsense_unified.c
    src/cpu/amd_zen.c
    src/cpu/amd_svi2.c
    src/cpu/amd_smu.c
    src/cpu/intel.c
    src/cpu/cpu_diag.c
    src/storage/storage.c
    src/motherboard/superio.c
    src/motherboard/ec.c
    src/gpu/gpu.c
)

target_include_directories(hwsense PUBLIC include)
target_include_directories(hwsense PRIVATE src)

# Link Win32 APIs and COM libraries
target_link_libraries(hwsense PRIVATE kernel32 advapi32 shell32 ole32 oleaut32 wbemuuid)

# Set output properties
set_target_properties(hwsense PROPERTIES
    OUTPUT_NAME "hwsense"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Example: read_cpu_temp
add_executable(read_cpu_temp examples/read_cpu_temp.c)
target_link_libraries(read_cpu_temp PRIVATE hwsense)
