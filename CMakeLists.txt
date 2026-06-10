cmake_minimum_required(VERSION 3.21)

project(Sims3MonoModder LANGUAGES CXX)

# C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg toolchain integration (set VCPKG_ROOT or CMAKE_TOOLCHAIN_FILE before configuring)
set(VCPKG_TARGET_TRIPLET "x86-windows-static" CACHE STRING "vcpkg triplet")

# Find packages via vcpkg
find_package(imgui CONFIG REQUIRED)
find_path(TOMLPP_INCLUDE_DIRS "toml++/toml.hpp")
find_library(TOMLPP_LIBRARY tomlplusplus REQUIRED)
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")
find_library(DETOURS_LIBRARY detours REQUIRED)

# Collect source files
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
)
file(GLOB_RECURSE HEADERS
    "src/*.h"
)
set(RC_FILES "src/resources/resources.rc")

# Build S3MM.Helper.dll from S3MM.cs before the resource compiler embeds it
add_custom_command(
    OUTPUT "${CMAKE_SOURCE_DIR}/S3MM.Helper/S3MM.Helper.dll"
    COMMAND cmd /c "${CMAKE_SOURCE_DIR}/S3MM.Helper/build.bat"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/S3MM.Helper"
    DEPENDS "${CMAKE_SOURCE_DIR}/S3MM.Helper/S3MM.cs"
    COMMENT "Building S3MM.Helper.dll from S3MM.cs"
)
add_custom_target(S3MMHelper DEPENDS "${CMAKE_SOURCE_DIR}/S3MM.Helper/S3MM.Helper.dll")

# Create shared library (.asi is just a renamed .dll)
add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS} ${RC_FILES})

# Ensure S3MM.Helper.dll is built before the ASI (rc compiler needs it)
add_dependencies(${PROJECT_NAME} S3MMHelper)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link libraries
target_include_directories(${PROJECT_NAME} PRIVATE
    ${TOMLPP_INCLUDE_DIRS}
    ${DETOURS_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    imgui::imgui
    ${TOMLPP_LIBRARY}
    ${DETOURS_LIBRARY}
    d3d9
    dxguid
    psapi
)

# Preprocessor definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32
    UNICODE
    _UNICODE
    S3MM_EXPORTS
    _WINDOWS
    _USRDLL
    _CRT_SECURE_NO_WARNINGS
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Compile options
target_compile_options(${PROJECT_NAME} PRIVATE
    /W4           # Warning level 4 (dead code, unreferenced params, etc.)
    /analyze      # MSVC static analysis
    /sdl          # SDL checks
    /permissive-  # Conformance mode
    /MP           # Multi-processor compilation
    $<$<CONFIG:Debug>:/Od>        # No optimization in debug
    $<$<CONFIG:Release>:/O2>      # Full optimization in release
    $<$<CONFIG:Release>:/GL>      # Whole program optimization
)

# Static runtime linking (matches /MT and /MTd)
set_property(TARGET ${PROJECT_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Output settings - .asi extension
set_target_properties(${PROJECT_NAME} PROPERTIES
    SUFFIX ".asi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<CONFIG>"
)

# Release link optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS_RELEASE "/LTCG /OPT:REF /OPT:ICF"
    )
endif()
