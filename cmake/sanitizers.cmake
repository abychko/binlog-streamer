INCLUDE_GUARD(GLOBAL)

# STRING (not BOOL): the choice is one of five named values, not on/off; the
# STRINGS property below turns it into a dropdown in ccmake/cmake-gui.
SET(SANITIZER "<NONE>" CACHE STRING "Sanitizer to build with")
SET_PROPERTY(CACHE SANITIZER PROPERTY STRINGS <NONE> ASAN MSAN TSAN UBSAN)

INCLUDE(CheckCXXCompilerFlag)

IF(SANITIZER STREQUAL "<NONE>")
    SET(BINLOG_STREAMER_SANITIZER_FLAG "")
ELSEIF(SANITIZER STREQUAL "ASAN")
    SET(BINLOG_STREAMER_SANITIZER_FLAG "-fsanitize=address")
ELSEIF(SANITIZER STREQUAL "MSAN")
    SET(BINLOG_STREAMER_SANITIZER_FLAG "-fsanitize=memory")
ELSEIF(SANITIZER STREQUAL "TSAN")
    # On a kernel with 32-bit mmap randomization (vm.mmap_rnd_bits=32,
    # default from 6.x) a TSAN binary dies with "unexpected memory mapping".
    # Run the build and ctest under `setarch x86_64 -R` (ASLR off) instead.
    SET(BINLOG_STREAMER_SANITIZER_FLAG "-fsanitize=thread")
ELSEIF(SANITIZER STREQUAL "UBSAN")
    SET(BINLOG_STREAMER_SANITIZER_FLAG "-fsanitize=undefined")
ELSE()
    MESSAGE(FATAL_ERROR "Unknown SANITIZER value '${SANITIZER}': use one of <NONE> ASAN MSAN TSAN UBSAN")
ENDIF()

IF(NOT SANITIZER STREQUAL "<NONE>")
    # CHECK_CXX_COMPILER_FLAG only tests the compile step; a sanitizer flag
    # also needs its runtime library at link time (undefined ___asan_init
    # otherwise), so it's repeated via CMAKE_REQUIRED_LINK_OPTIONS.
    SET(CMAKE_REQUIRED_LINK_OPTIONS "${BINLOG_STREAMER_SANITIZER_FLAG}")
    CHECK_CXX_COMPILER_FLAG("${BINLOG_STREAMER_SANITIZER_FLAG}" BINLOG_STREAMER_SANITIZER_SUPPORTED)
    UNSET(CMAKE_REQUIRED_LINK_OPTIONS)
    IF(NOT BINLOG_STREAMER_SANITIZER_SUPPORTED)
        MESSAGE(FATAL_ERROR "${SANITIZER} (${BINLOG_STREAMER_SANITIZER_FLAG}) is not supported by ${CMAKE_CXX_COMPILER}")
    ENDIF()

    # Directory-scoped, so it also reaches third_party/: a sanitizer build
    # needs every dependency instrumented the same way, or the boundary
    # with uninstrumented code produces false reports.
    ADD_COMPILE_OPTIONS("${BINLOG_STREAMER_SANITIZER_FLAG}")
    ADD_LINK_OPTIONS("${BINLOG_STREAMER_SANITIZER_FLAG}")

    IF(SANITIZER STREQUAL "ASAN")
        # Recommended alongside ASan: without a frame pointer its stack
        # unwinder can't reliably produce backtraces.
        ADD_COMPILE_OPTIONS(-fno-omit-frame-pointer)
    ENDIF()
ENDIF()

IF(DEVELOPER_MODE)
    MESSAGE(STATUS "<DEVELOPER MODE> -> SANITIZER=${SANITIZER}")
ENDIF()
