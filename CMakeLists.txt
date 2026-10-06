CMAKE_MINIMUM_REQUIRED(VERSION 3.20)

INCLUDE(cmake/project_version.cmake)
# LANGUAGES includes C (the project itself is C++ only) because
# third_party/CMakeLists.txt configures the OpenSSL submodule with
# CMAKE_C_COMPILER, which CMake leaves empty unless C is enabled here.
PROJECT(binlog-streamer VERSION "${BINLOG_STREAMER_VERSION}" LANGUAGES C CXX)

SET(CMAKE_CXX_STANDARD 20)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)
SET(CMAKE_CXX_EXTENSIONS OFF)

OPTION(DEVELOPER_MODE "Enable developer mode" OFF)
INCLUDE("${PROJECT_SOURCE_DIR}/cmake/developer_mode.cmake")
INCLUDE("${PROJECT_SOURCE_DIR}/cmake/sanitizers.cmake")

ADD_SUBDIRECTORY(third_party)
INCLUDE("${PROJECT_SOURCE_DIR}/cmake/x86_64_level.cmake")

SET(BINLOG_STREAMER_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/include")
# The repository root must not be an include directory: on a case-insensitive
# file system the VERSION file shadows the standard <version> header.
INCLUDE_DIRECTORIES(
    "${BINLOG_STREAMER_INCLUDE_DIR}"
    "${PROJECT_SOURCE_DIR}/src"
)

ADD_LIBRARY(bs-warnings INTERFACE)
TARGET_COMPILE_OPTIONS(bs-warnings INTERFACE -Wall -Wextra -Wpedantic -Werror -Wmissing-declarations -pedantic-errors)
# GCC reports these two from inside libstdc++'s std::vector growth paths on code
# that is correct; kept visible but not fatal. Clang is unaffected.
TARGET_COMPILE_OPTIONS(bs-warnings INTERFACE
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=free-nonheap-object>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=stringop-overflow>")
INCLUDE(cmake/header_check.cmake)
INCLUDE(CTest)
IF(BUILD_TESTING)
    FIND_PACKAGE(GTest REQUIRED)
    INCLUDE(GoogleTest)
ENDIF()

SET(BINLOG_STREAMER_MODULES binlog cache config gtid http protocol net
    receiver server status storage src)
FOREACH(MODULE IN LISTS BINLOG_STREAMER_MODULES)
    ADD_SUBDIRECTORY(${MODULE})
ENDFOREACH()

INCLUDE(cmake/format.cmake)
ADD_FORMAT_TARGETS(include ${BINLOG_STREAMER_MODULES})

INSTALL(FILES README.md DESTINATION share/doc/${PROJECT_NAME})
INSTALL(FILES packaging/zabbix/binlog-streamer.yaml
    DESTINATION share/doc/${PROJECT_NAME}/zabbix)
INSTALL(FILES packaging/settings.yml packaging/source.yml packaging/replica.yml
    DESTINATION /etc/${PROJECT_NAME}
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)
INSTALL(DIRECTORY DESTINATION /etc/${PROJECT_NAME}
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
INSTALL(FILES packaging/html/index.html packaging/html/status.css
    packaging/html/status.js DESTINATION /etc/${PROJECT_NAME}/html
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
INSTALL(DIRECTORY DESTINATION /var/lib/${PROJECT_NAME}
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)

# Unit/preset paths are relative to CPACK_PACKAGING_INSTALL_PREFIX (/usr,
# cmake/packaging.cmake), giving /usr/lib/systemd/system(-preset).
INSTALL(FILES packaging/systemd/binlog-streamer.service DESTINATION lib/systemd/system)
INSTALL(FILES packaging/systemd/90-binlog-streamer.preset DESTINATION lib/systemd/system-preset)
INSTALL(FILES packaging/default/binlog-streamer DESTINATION /etc/default
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

INCLUDE(cmake/packaging.cmake)
