INCLUDE_GUARD(GLOBAL)

# format applies .clang-format to the sources, format-check fails on the
# first file that differs. Without clang-format both only say so.
FUNCTION(ADD_FORMAT_TARGETS)
    SET(SOURCES)
    FOREACH(DIRECTORY IN LISTS ARGN)
        FILE(GLOB_RECURSE FOUND CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/${DIRECTORY}/*.hpp"
            "${PROJECT_SOURCE_DIR}/${DIRECTORY}/*.cpp")
        LIST(APPEND SOURCES ${FOUND})
    ENDFOREACH()
    LIST(SORT SOURCES)

    FIND_PROGRAM(CLANG_FORMAT_EXECUTABLE clang-format)
    IF(CLANG_FORMAT_EXECUTABLE)
        ADD_CUSTOM_TARGET(format
            COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${SOURCES}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMAND_EXPAND_LISTS VERBATIM)
        ADD_CUSTOM_TARGET(format-check
            COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${SOURCES}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMAND_EXPAND_LISTS VERBATIM)
    ELSE()
        FOREACH(TARGET format format-check)
            ADD_CUSTOM_TARGET(${TARGET}
                COMMAND "${CMAKE_COMMAND}" -E echo
                    "clang-format not found: ${TARGET} skipped"
                VERBATIM)
        ENDFOREACH()
    ENDIF()
ENDFUNCTION()
