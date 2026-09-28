INCLUDE_GUARD(GLOBAL)

SET(YAML_CPP_BUILD_CONTRIB OFF)
SET(YAML_CPP_BUILD_TOOLS OFF)
SET(YAML_CPP_INSTALL OFF)
SET(YAML_BUILD_SHARED_LIBS OFF)
SET(YAML_CPP_BUILD_TESTS OFF)
ADD_SUBDIRECTORY(yaml-cpp)

# zstd builds its library, its command line tools and its tests from one
# CMakeLists; only the first is wanted here. EXCLUDE_FROM_ALL keeps its
# install rules -- which have no option to turn off in 1.5.7 -- out of our
# package: only targets our own targets link against get built at all.
SET(ZSTD_BUILD_SHARED OFF)
SET(ZSTD_BUILD_STATIC ON)
SET(ZSTD_BUILD_PROGRAMS OFF)
SET(ZSTD_BUILD_TESTS OFF)
SET(ZSTD_BUILD_CONTRIB OFF)
SET(ZSTD_LEGACY_SUPPORT OFF)
SET(ZSTD_MULTITHREAD_SUPPORT OFF)
SET(ZSTD_BUILD_DICTBUILDER OFF)
SET(ZSTD_BUILD_DEPRECATED OFF)
ADD_SUBDIRECTORY(zstd/build/cmake EXCLUDE_FROM_ALL)

# zlib-ng stands in for the reference zlib: the algorithm named on
# the wire stays "zlib", and the bytes it produces are the RFC 1950 stream
# MySQL's compress2()/uncompress() speak.
#
# Built in compatibility mode, as Percona Server builds its own bundled copy
# (cmake/zlib.cmake, extra/zlib-ng/CMakeLists.txt): the API is zlib's, so the
# calls here read against mysys/my_compress.cc line for line. Runtime CPU
# detection rather than -march=native, because the package is built on one
# host and installed on others. BUILD_TESTING is zlib-ng's own switch for its
# test binaries and their gtest dependency; set as a plain variable it covers
# this directory only, leaving the root's INCLUDE(CTest) to decide about our
# own tests.
SET(ZLIB_COMPAT ON)
SET(ZLIB_ALIASES OFF)
SET(WITH_GZFILEOP OFF)
SET(WITH_NATIVE_INSTRUCTIONS OFF)
SET(WITH_RUNTIME_CPU_DETECTION ON)
SET(BUILD_TESTING OFF)
SET(BUILD_SHARED_LIBS OFF)
ADD_SUBDIRECTORY(zlib-ng EXCLUDE_FROM_ALL)

# OpenSSL has its own build system (Configure/make), not CMake, so it is
# built out of tree with ExternalProject_Add instead of ADD_SUBDIRECTORY.
INCLUDE(ExternalProject)
INCLUDE(ProcessorCount)

PROCESSORCOUNT(BINLOG_STREAMER_OPENSSL_JOBS)
IF(BINLOG_STREAMER_OPENSSL_JOBS EQUAL 0)
    # ProcessorCount() leaves the result at 0 when it cannot detect the core
    # count; fall back to a serial build rather than pass -j0 to make, which
    # means unlimited parallel jobs.
    SET(BINLOG_STREAMER_OPENSSL_JOBS 1)
ENDIF()

SET(BINLOG_STREAMER_OPENSSL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/openssl")
# Not ".../openssl": with an in-source build the binary directory is the
# source directory, so that name would point at the submodule itself and
# OpenSSL's install step would copy its own headers onto themselves
# ("cp: ... are identical"), failing the build.
SET(BINLOG_STREAMER_OPENSSL_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/openssl-install")

SET(BINLOG_STREAMER_OPENSSL_CONFIGURE_ARGS
    no-shared no-tests no-docs no-apps
    "--prefix=${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}"
    --libdir=lib
    "CC=${CMAKE_C_COMPILER}")
IF(NOT SANITIZER STREQUAL "<NONE>")
    # Keeps OpenSSL instrumented the same way as the rest of the build (see
    # cmake/sanitizers.cmake): a mismatched boundary produces false reports.
    # Configure takes CFLAGS as a plain VAR=value argument.
    LIST(APPEND BINLOG_STREAMER_OPENSSL_CONFIGURE_ARGS
        "CFLAGS=${BINLOG_STREAMER_SANITIZER_FLAG}")
ENDIF()

EXTERNALPROJECT_ADD(openssl_build
    SOURCE_DIR "${BINLOG_STREAMER_OPENSSL_SOURCE_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CONFIGURE_COMMAND perl "${BINLOG_STREAMER_OPENSSL_SOURCE_DIR}/Configure" ${BINLOG_STREAMER_OPENSSL_CONFIGURE_ARGS}
    BUILD_COMMAND make -j${BINLOG_STREAMER_OPENSSL_JOBS} build_libs
    INSTALL_COMMAND make install_dev
    BUILD_BYPRODUCTS "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/lib/libcrypto.a"
                     "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/lib/libssl.a")

# The imported target below is configured at CMake configure time, before
# the ExternalProject's install step has run and created this directory;
# CMake rejects a non-existent path in INTERFACE_INCLUDE_DIRECTORIES.
FILE(MAKE_DIRECTORY "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/include")

# GLOBAL: modules outside this directory (e.g. protocol/) link against this
# target by name, and imported targets are otherwise scoped to the
# directory that defines them.
ADD_LIBRARY(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
SET_TARGET_PROPERTIES(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/lib/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/include")
ADD_DEPENDENCIES(OpenSSL::Crypto openssl_build)

IF(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Static libcrypto calls into pthreads on both platforms; on Linux it
    # additionally dlopen()s engines, which needs libdl (absent on macOS,
    # where dlopen is part of libSystem already linked into every binary).
    FIND_PACKAGE(Threads REQUIRED)
    SET(BINLOG_STREAMER_OPENSSL_SYSTEM_LIBS Threads::Threads)
    IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        LIST(APPEND BINLOG_STREAMER_OPENSSL_SYSTEM_LIBS ${CMAKE_DL_LIBS})
    ENDIF()
    TARGET_LINK_LIBRARIES(OpenSSL::Crypto INTERFACE ${BINLOG_STREAMER_OPENSSL_SYSTEM_LIBS})
ENDIF()

# The same build_libs step produces libssl; only net/ (TlsTransport) links
# it. Linked ahead of libcrypto, which it calls into.
ADD_LIBRARY(OpenSSL::SSL STATIC IMPORTED GLOBAL)
SET_TARGET_PROPERTIES(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/lib/libssl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${BINLOG_STREAMER_OPENSSL_INSTALL_DIR}/include"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
ADD_DEPENDENCIES(OpenSSL::SSL openssl_build)
