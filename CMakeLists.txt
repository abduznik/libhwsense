cmake_minimum_required(VERSION 3.15)
project(libhwsense VERSION 0.3.0 LANGUAGES C)

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
        src/core/alert.c
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
    #
    # export.c and alert.c are platform-independent, and the installed
    # headers declare them unconditionally, so they must be built here too
    # or a Linux consumer gets undefined references.
    set(HWSENSE_SOURCES
        src/core/driver_linux.c
        src/cpu/intel_linux.c
        src/core/export.c
        src/core/alert.c
    )
endif()

add_library(hwsense SHARED ${HWSENSE_SOURCES})

target_include_directories(hwsense
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_include_directories(hwsense PRIVATE src)

set_target_properties(hwsense PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

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

# HWSENSE_ALERT_LOGIC_ONLY drops the one function that reads a live
# sensor, leaving the threshold logic the tests actually cover.
add_executable(test_alert tests/test_alert.c src/core/alert.c)
target_include_directories(test_alert PRIVATE src include)
target_compile_definitions(test_alert PRIVATE HWSENSE_ALERT_LOGIC_ONLY)
add_test(NAME alert COMMAND test_alert)

# ── Install ───────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS hwsense
    EXPORT hwsenseTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
    include/hwsense.h
    include/hwsense_unified.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Lets consumers do find_package(hwsense) + target_link_libraries(app hwsense::hwsense)
install(EXPORT hwsenseTargets
    FILE        hwsenseTargets.cmake
    NAMESPACE   hwsense::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hwsense
)

configure_package_config_file(
    cmake/hwsenseConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hwsenseConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hwsense
)

# SameMajorVersion: the API is not yet stable across minor releases.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hwsenseConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hwsenseConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hwsenseConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hwsense
)

# pkg-config, for Linux consumers not using CMake
if(NOT WIN32)
    configure_file(cmake/hwsense.pc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/hwsense.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hwsense.pc
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
