cmake_minimum_required(VERSION 3.16)

# The router is a separate firmware target.  Keeping its sdkconfig and build
# directory independent prevents it from changing any gateway Stage artifact.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(PROJECT_VER "0.1.0-router")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp-zigbee-lib/CMakeLists.txt")
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp-zigbee-lib"
    )
endif()

# Some older Windows installations expose the ESP-IDF tools to idf.py but not
# to the compiler-identification subprocess started by CMake.  The build script
# passes the already validated toolchain directory explicitly so a clean build
# does not depend on that inherited PATH behavior.
if(ROUTER_TOOLCHAIN_BIN)
    file(TO_CMAKE_PATH "${ROUTER_TOOLCHAIN_BIN}" _router_toolchain_bin)
    set(ENV{PATH} "${_router_toolchain_bin};$ENV{PATH}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
# ESP-IDF builds the bootloader as a nested CMake project.  This Windows host
# does not expose Ninja through the child process PATH reliably, so forward the
# already selected absolute executable explicitly.
if(CMAKE_MAKE_PROGRAM)
    idf_build_set_property(EXTRA_CMAKE_ARGS
        "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" APPEND)
endif()
if(_router_toolchain_bin)
    # IDF's target toolchain file deliberately resets compiler variables back
    # to their executable names.  CMAKE_PROGRAM_PATH lets that later lookup
    # resolve them even on hosts whose child-process PATH is incomplete.
    idf_build_set_property(EXTRA_CMAKE_ARGS
        "-DCMAKE_PROGRAM_PATH=${_router_toolchain_bin}" APPEND)
    idf_build_set_property(EXTRA_CMAKE_ARGS
        "-DCMAKE_C_COMPILER=${_router_toolchain_bin}/riscv32-esp-elf-gcc.exe" APPEND)
    idf_build_set_property(EXTRA_CMAKE_ARGS
        "-DCMAKE_CXX_COMPILER=${_router_toolchain_bin}/riscv32-esp-elf-g++.exe" APPEND)
    idf_build_set_property(EXTRA_CMAKE_ARGS
        "-DCMAKE_ASM_COMPILER=${_router_toolchain_bin}/riscv32-esp-elf-gcc.exe" APPEND)
endif()
project(esp32c6_zigbee_router)
