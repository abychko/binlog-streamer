INCLUDE_GUARD(GLOBAL)

# Minimum x86-64 microarchitecture level the relay's own code is built for;
# ignored on other processors. third_party/ is left out: OpenSSL, zstd and
# zlib-ng pick their instructions at run time.
SET(X86_64_LEVEL "x86-64-v3" CACHE STRING "Minimum x86-64 level to build for")
SET_PROPERTY(CACHE X86_64_LEVEL
    PROPERTY STRINGS <NONE> x86-64 x86-64-v2 x86-64-v3 x86-64-v4)

IF(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$"
        AND NOT X86_64_LEVEL STREQUAL "<NONE>")
    INCLUDE(CheckCXXCompilerFlag)
    CHECK_CXX_COMPILER_FLAG("-march=${X86_64_LEVEL}"
        BINLOG_STREAMER_X86_64_LEVEL_SUPPORTED)
    IF(NOT BINLOG_STREAMER_X86_64_LEVEL_SUPPORTED)
        MESSAGE(FATAL_ERROR
            "-march=${X86_64_LEVEL} is not supported by ${CMAKE_CXX_COMPILER}")
    ENDIF()
    ADD_COMPILE_OPTIONS("-march=${X86_64_LEVEL}")
    MESSAGE(STATUS "x86-64 level: ${X86_64_LEVEL}")
ENDIF()
