cmake_minimum_required(VERSION 3.21)
project(move-anything-surge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)

# Build as shared library (dsp.so)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Surge source root
set(SURGE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/surge")

# Surge configuration - headless/embedded mode
set(SURGE_SKIP_JUCE_FOR_RACK ON CACHE BOOL "Skip JUCE for headless build")
set(SURGE_SKIP_LUA ON CACHE BOOL "Skip LuaJIT (no formula modulator)")
# Note: MTS-ESP can't be skipped due to unguarded references in SurgeVoice.cpp.
# The MTS client library is lightweight (just a dlopen wrapper) and gracefully
# does nothing when the MTS-ESP library isn't present on the target device.
set(SURGE_SKIP_ODDSOUND_MTS OFF CACHE BOOL "Keep MTS-ESP (needed for compilation)")
set(SURGE_COMPILE_BLOCK_SIZE 32 CACHE STRING "Surge audio block size")

# Don't build anything except the common library
set(SURGE_BUILD_FX OFF CACHE BOOL "")
set(SURGE_BUILD_XT OFF CACHE BOOL "")
set(SURGE_BUILD_TESTRUNNER OFF CACHE BOOL "")
set(SURGE_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "")
set(SURGE_BUILD_CLAP OFF CACHE BOOL "")

# Set the Surge source directory (used by surge_add_lib_subdirectory)
set(SURGE_SOURCE_DIR "${SURGE_ROOT}" CACHE STRING "")

# simde for SIMD portability (SSE -> NEON on ARM)
set(SURGE_SIMDE_PATH "${SURGE_ROOT}/libs/simde" CACHE STRING "")

# Create symlink for libs/ at CMAKE_SOURCE_DIR level since Surge's
# common/CMakeLists.txt line 342 uses ${CMAKE_SOURCE_DIR}/libs/r8brain-free-src
# (a bug - should use SURGE_SOURCE_DIR, but we can't modify the submodule)
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/libs")
    file(CREATE_LINK "${SURGE_ROOT}/libs" "${CMAKE_SOURCE_DIR}/libs" SYMBOLIC)
endif()

# Add Surge source tree
add_subdirectory(${SURGE_ROOT}/src ${CMAKE_BINARY_DIR}/surge-src)

# Build the Move Anything plugin wrapper
add_library(surge-move-plugin SHARED
    src/dsp/surge_plugin.cpp
)

target_include_directories(surge-move-plugin PRIVATE
    ${SURGE_ROOT}/src/common
    ${SURGE_ROOT}/src/common/dsp
    ${SURGE_ROOT}/src/common/dsp/effects
    ${SURGE_ROOT}/src/common/dsp/filters
    ${SURGE_ROOT}/src/common/dsp/modulators
    ${SURGE_ROOT}/src/common/dsp/oscillators
    ${SURGE_ROOT}/src/common/dsp/utilities
    ${SURGE_ROOT}/libs/simde
    ${SURGE_ROOT}/libs/fmt/include
    ${SURGE_ROOT}/libs/tuning-library/include
    ${SURGE_ROOT}/libs/sst/sst-basic-blocks/include
    ${SURGE_ROOT}/libs/sst/sst-cpputils/include
    ${SURGE_ROOT}/libs/sst/sst-filters/include
    ${SURGE_ROOT}/libs/sst/sst-waveshapers/include
    ${SURGE_ROOT}/libs/sst/sst-effects/include
    ${SURGE_ROOT}/libs/sst/sst-plugininfra/include
)

target_link_libraries(surge-move-plugin PRIVATE
    surge-common
)

# Statically link libstdc++ - the Move device has GLIBCXX up to 3.4.29
# but Surge's C++20 code requires 3.4.30+
target_link_options(surge-move-plugin PRIVATE -static-libstdc++ -static-libgcc)

# Output as dsp.so
set_target_properties(surge-move-plugin PROPERTIES
    OUTPUT_NAME "dsp"
    PREFIX ""
    SUFFIX ".so"
)

