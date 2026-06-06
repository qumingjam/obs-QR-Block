# Script to generate .def file from OBS DLL exports
# Reads dumpbin /exports output and creates a .def file

set(EXPORTS_FILE "${OUTPUT_DIR}/obs_exports.txt")
set(DEF_FILE "${OUTPUT_DIR}/obs.def")

if(NOT EXISTS "${EXPORTS_FILE}")
    message(FATAL_ERROR "Exports file not found: ${EXPORTS_FILE}")
endif()

file(READ "${EXPORTS_FILE}" EXPORTS_CONTENT)

# Parse ordinal and name from dumpbin output
# Format:    ordinal hint RVA      name
# Example:   1    0 00000001 CreateOBSSource
string(REGEX MATCHALL "\n[ \t]*([0-9]+)[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([^ \t\r\n]+)" MATCHED_LINES "${EXPORTS_CONTENT}")

set(EXPORTS "")
foreach(MATCH ${MATCHED_LINES})
    string(REGEX REPLACE "\n[ \t]*([0-9]+)[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([^ \t\r\n]+)" "\\2" FUNC_NAME "${MATCH}")
    # Skip non-function entries (like directory names with extension)
    if(FUNC_NAME MATCHES "^[_a-zA-Z][_a-zA-Z0-9]*$")
        list(APPEND EXPORTS "${FUNC_NAME}")
    endif()
endforeach()

# Write .def file
file(WRITE "${DEF_FILE}" "EXPORTS\n")
foreach(FUNC ${EXPORTS})
    file(APPEND "${DEF_FILE}" "    ${FUNC}\n")
endforeach()

message(STATUS "Generated .def file with ${EXPORTS} exports: ${DEF_FILE}")

# Now run lib.exe to generate .lib
set(LIB_FILE "${OUTPUT_DIR}/obs.lib")
execute_process(
    COMMAND lib.exe /def:"${DEF_FILE}" /out:"${LIB_FILE}" /machine:x64
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    RESULT_VARIABLE LIB_RESULT
    OUTPUT_VARIABLE LIB_OUTPUT
    ERROR_VARIABLE LIB_ERROR
)

if(NOT LIB_RESULT EQUAL 0)
    message(WARNING "lib.exe failed: ${LIB_ERROR}")
    message(WARNING "You may need to manually generate obs.lib using:")
    message(WARNING "  lib /def:\"${DEF_FILE}\" /out:\"${LIB_FILE}\" /machine:x64")
    message(WARNING "Or set OBS_SKIP_IMPORT_LIB=1 to skip linking against OBS")
else()
    message(STATUS "Successfully generated OBS import library: ${LIB_FILE}")
endif()
