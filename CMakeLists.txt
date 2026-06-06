cmake_minimum_required(VERSION 3.20)
project(qr-blocker-plugin
    VERSION 1.0.27
    DESCRIPTION "OBS QR Code Blocker Filter Plugin"
    LANGUAGES C
)

# ============================================================
# Configuration
# ============================================================

# OBS installation path (set by build.bat or environment)
if(NOT DEFINED ENV{OBS_DIR})
    set(OBS_DIR "C:/Program Files/obs-studio" CACHE PATH "OBS Studio installation directory")
else()
    set(OBS_DIR "$ENV{OBS_DIR}" CACHE PATH "OBS Studio installation directory")
endif()

# OBS headers path (downloaded by build.bat)
if(NOT DEFINED ENV{OBS_HEADERS_DIR})
    set(OBS_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/obs-headers" CACHE PATH "OBS headers directory")
else()
    set(OBS_HEADERS_DIR "$ENV{OBS_HEADERS_DIR}" CACHE PATH "OBS headers directory")
endif()

# Quirc source path (downloaded by build.bat)
if(NOT DEFINED ENV{QUIRC_DIR})
    set(QUIRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/quirc" CACHE PATH "Quirc source directory")
else()
    set(QUIRC_DIR "$ENV{QUIRC_DIR}" CACHE PATH "Quirc source directory")
endif()

message(STATUS "OBS_DIR:        ${OBS_DIR}")
message(STATUS "OBS_HEADERS_DIR: ${OBS_HEADERS_DIR}")
message(STATUS "QUIRC_DIR:       ${QUIRC_DIR}")

# ============================================================
# OBS Headers Check
# ============================================================

if(NOT EXISTS "${OBS_HEADERS_DIR}/obs-module.h")
    message(FATAL_ERROR
        "OBS headers not found at ${OBS_HEADERS_DIR}.\n"
        "Please run build.bat first to download dependencies."
    )
endif()

# ============================================================
# Quirc Check
# ============================================================

if(NOT EXISTS "${QUIRC_DIR}/lib/quirc.c")
    message(FATAL_ERROR
        "Quirc source not found at ${QUIRC_DIR}.\n"
        "Please run build.bat first to download dependencies."
    )
endif()

# ============================================================
# Import Library Generation (if needed)
# ============================================================

# Check if OBS import library exists, if not, generate it
set(OBS_LIB "${OBS_DIR}/bin/64bit/obs.lib")
if(NOT EXISTS "${OBS_LIB}")
    message(STATUS "OBS import library not found. Will generate during build.")

    # Find the .def file or create it
    set(OBS_DLL "${OBS_DIR}/bin/64bit/obs.dll")
    if(NOT EXISTS "${OBS_DLL}")
        message(FATAL_ERROR "OBS DLL not found at ${OBS_DLL}")
    endif()

    # Custom target to generate .lib from .dll
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/obs.def"
        COMMAND dumpbin /exports "${OBS_DLL}" > "${CMAKE_BINARY_DIR}/obs_exports.txt"
        COMMAND ${CMAKE_COMMAND} -E echo "Generating .def file from exports..."
        COMMAND ${CMAKE_COMMAND}
            -DOBS_DLL="${OBS_DLL}"
            -DOUTPUT_DIR="${CMAKE_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_import_lib.cmake"
        DEPENDS "${OBS_DLL}"
        COMMENT "Generating OBS import library..."
    )

    add_custom_target(generate_obs_lib ALL
        DEPENDS "${CMAKE_BINARY_DIR}/obs.def"
    )

    set(OBS_IMPORT_LIB "${CMAKE_BINARY_DIR}/obs.lib")
    set(OBS_IMPORT_DEP generate_obs_lib)
else()
    set(OBS_IMPORT_LIB "${OBS_LIB}")
    set(OBS_IMPORT_DEP "")
    message(STATUS "Using existing OBS import library: ${OBS_LIB}")
endif()

# ============================================================
# Plugin Target
# ============================================================

# Quirc sources
set(QUIRC_SOURCES
    "${QUIRC_DIR}/lib/quirc.c"
    "${QUIRC_DIR}/lib/identify.c"
    "${QUIRC_DIR}/lib/version_db.c"
)

# Plugin sources
set(PLUGIN_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/qr-blocker-filter.c"
    ${QUIRC_SOURCES}
)

# Build the plugin as a shared library (DLL)
add_library(qr-blocker-filter MODULE
    ${PLUGIN_SOURCES}
)

# Target properties
set_target_properties(qr-blocker-filter PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# Include directories
target_include_directories(qr-blocker-filter PRIVATE
    "${OBS_HEADERS_DIR}"
    "${QUIRC_DIR}/lib"
)

# Link OBS
if(EXISTS "${OBS_IMPORT_LIB}")
    target_link_libraries(qr-blocker-filter PRIVATE "${OBS_IMPORT_LIB}")
endif()

# Link Windows system libraries
target_link_libraries(qr-blocker-filter PRIVATE
    gdi32
    user32
    ole32
)

# Add dependency on import lib generation
if(OBS_IMPORT_DEP)
    add_dependencies(qr-blocker-filter ${OBS_IMPORT_DEP})
endif()

# ============================================================
# Compile Definitions
# ============================================================

target_compile_definitions(qr-blocker-filter PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _AMD64_
)

# ============================================================
# Install Rules
# ============================================================

# Install plugin to OBS
install(TARGETS qr-blocker-filter
    LIBRARY DESTINATION "${OBS_DIR}/obs-plugins/64bit"
    RUNTIME DESTINATION "${OBS_DIR}/obs-plugins/64bit"
)

# Install locale files
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/"
    DESTINATION "${OBS_DIR}/data/obs-plugins/qr-blocker-filter"
)

# ============================================================
# Post-build: Copy to OBS plugins
# ============================================================

add_custom_command(TARGET qr-blocker-filter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:qr-blocker-filter>"
        "${OBS_DIR}/obs-plugins/64bit/qr-blocker-filter.dll"
    COMMENT "Copying plugin to OBS plugins directory..."
)

message(STATUS "Project configured successfully!")
message(STATUS "Build with: cmake --build . --config Release")
