INCLUDE_GUARD(GLOBAL)

# On by default on a native Debian-family build host, where the DEB is the
# package being made; off elsewhere, where it cannot be built.
SET(BUILD_DEB_DEFAULT OFF)
IF(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING AND EXISTS "/etc/debian_version")
    SET(BUILD_DEB_DEFAULT ON)
ENDIF()
OPTION(BUILD_DEB "Build a Debian/Ubuntu package" ${BUILD_DEB_DEFAULT})

SET(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
SET(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "MySQL binary log relay for independent replicas")
SET(CPACK_PACKAGE_CONTACT "abychko@gmail.com")
SET(CPACK_PACKAGE_VENDOR "ABYCHKO.EXPERT")
SET(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
SET(CPACK_SET_DESTDIR ON)
SET(CPACK_INSTALL_PREFIX "/usr")
SET(CPACK_STRIP_FILES ON)

# Implicit parent directories would inherit the packaging machine's umask (umask
# 002 ships 0775 instead of 0755).
SET(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE)

IF(BUILD_DEB)
    IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        MESSAGE(FATAL_ERROR "BUILD_DEB requires a native Debian/Ubuntu Linux build")
    ENDIF()
    FIND_PROGRAM(DPKG_EXECUTABLE NAMES dpkg REQUIRED)
    FIND_PROGRAM(DPKG_SHLIBDEPS_EXECUTABLE NAMES dpkg-shlibdeps REQUIRED)

    SET(CPACK_GENERATOR DEB)
    SET(CPACK_DEBIAN_PACKAGE_MAINTAINER "Alexey Bychko <abychko@gmail.com>")
    SET(CPACK_DEBIAN_PACKAGE_SECTION "database")
    SET(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    SET(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    SET(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${PROJECT_SOURCE_DIR}/packaging/deb/conffiles"
        "${PROJECT_SOURCE_DIR}/packaging/deb/postinst"
        "${PROJECT_SOURCE_DIR}/packaging/deb/prerm"
        "${PROJECT_SOURCE_DIR}/packaging/deb/postrm")
    SET(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
    SET(CPACK_DEBIAN_PACKAGE_DEPENDS "adduser")

    # CPack is not included without BUILD_DEB: with CPACK_GENERATOR unset it
    # would fall back to its own archive generators.
    INCLUDE(CPack)
ENDIF()
