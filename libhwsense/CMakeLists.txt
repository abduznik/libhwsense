cmake_minimum_required(VERSION 3.15)
project(libhwsense C)

# Windows only
if(NOT WIN32)
    message(FATAL_ERROR "libhwsense requires Windows (WinRing0 kernel driver)")
endif()

# Static library: libhwsense
add_library(hwsense STATIC
    src/core/driver.c
    src/core/api.c
    src/core/win_sysstats.c
    src/core/wmi.c
    src/cpu/amd_zen.c
    src/cpu/amd_svi2.c
    src/cpu/amd_smu.c
    src/cpu/intel.c
    src/storage/storage.c
    src/motherboard/superio.c
    src/motherboard/ec.c
    src/gpu/gpu.c
)

target_include_directories(hwsense PUBLIC include)
target_include_directories(hwsense PRIVATE src)

# Link Win32 APIs and COM libraries
target_link_libraries(hwsense PRIVATE kernel32 advapi32 shell32 ole32 oleaut32 wbemuuid)

# Example: read_cpu_temp
add_executable(read_cpu_temp examples/read_cpu_temp.c)
target_link_libraries(read_cpu_temp PRIVATE hwsense)
