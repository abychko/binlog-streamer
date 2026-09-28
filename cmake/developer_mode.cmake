INCLUDE_GUARD(GLOBAL)

# Strict warnings are always on via bs-warnings; developer mode has nothing
# to flip for them, and only reports that fact below.
IF(DEVELOPER_MODE)
    SET(CMAKE_BUILD_TYPE "Debug")
    SET(CMAKE_VERBOSE_MAKEFILE ON)

    MESSAGE(STATUS "<DEVELOPER MODE> -> CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
    MESSAGE(STATUS "<DEVELOPER MODE> -> CMAKE_VERBOSE_MAKEFILE=${CMAKE_VERBOSE_MAKEFILE}")
    MESSAGE(STATUS "<DEVELOPER MODE> -> WARNINGS=always on (bs-warnings: -Wall -Wextra -Wpedantic -Werror)")

    # Directory-scoped, so it reaches src/main.cpp even though this file
    # runs before ADD_SUBDIRECTORY(src): the main target doesn't exist yet
    # here, so TARGET_COMPILE_DEFINITIONS naming it isn't possible.
    ADD_COMPILE_DEFINITIONS(BINLOG_STREAMER_DEVELOPER_MODE)
    MESSAGE(STATUS "<DEVELOPER MODE> -> BINLOG_STREAMER_DEVELOPER_MODE defined (relaxes the settings file owner/group check to the running user/group - see src/main.cpp)")
ENDIF()
