cmake_minimum_required(VERSION 3.15)
project(libhwsense C)

if(WIN32)
    # Define HWSENSE_DLL_EXPORTS for DLL builds
    add_definitions(-DHWSENSE_DLL_EXPORTS)

    set(HWSENSE_SOURCES
        src/core/driver.c
        src/core/api.c
        src/core/win_sysstats.c
        src/core/wmi.c
        src/core/hwsense_unified.c
        src/core/export.c
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
else()
    # Linux: MSR access via /dev/cpu/N/msr, no kernel driver needed.
    # Only the Intel CPU temperature path is ported so far — the WinRing0,
    # WMI, ADL, Super I/O and EC backends are Windows-only.
    set(HWSENSE_SOURCES
        src/core/driver_linux.c
        src/cpu/intel_linux.c
    )
endif()

add_library(hwsense SHARED ${HWSENSE_SOURCES})

target_include_directories(hwsense PUBLIC include)
target_include_directories(hwsense PRIVATE src)

if(WIN32)
    # Link Win32 APIs and COM libraries
    target_link_libraries(hwsense PRIVATE kernel32 advapi32 shell32 ole32 oleaut32 wbemuuid)
endif()

# Set output properties
set_target_properties(hwsense PROPERTIES
    OUTPUT_NAME "hwsense"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Example: read_cpu_temp
if(WIN32)
    add_executable(read_cpu_temp examples/read_cpu_temp.c)
else()
    add_executable(read_cpu_temp examples/read_cpu_temp_linux.c)
endif()
target_link_libraries(read_cpu_temp PRIVATE hwsense)

# Tests: pure math and serialization, no hardware needed
enable_testing()

add_executable(test_sensor_math tests/test_sensor_math.c)
target_include_directories(test_sensor_math PRIVATE src)
if(NOT WIN32)
    target_link_libraries(test_sensor_math PRIVATE m)
endif()
add_test(NAME sensor_math COMMAND test_sensor_math)

# export.c is compiled into the test directly rather than linked from the
# shared library, so the serializers can be tested on platforms where the
# rest of the library does not build.
add_executable(test_export tests/test_export.c src/core/export.c)
target_include_directories(test_export PRIVATE src include)
add_test(NAME export COMMAND test_export)
