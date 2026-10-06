ADD_LIBRARY(bs-config STATIC
    cAddressRangeMatcher.cpp
    cAddressRangeParser.cpp
    cByteSizeParser.cpp
    cConfigErrorPrinter.cpp
    cConfigLoader.cpp
    cConfigValidator.cpp
    cConfigurationReload.cpp
    cConfigurationLoader.cpp
    cDiskProtectedFileReader.cpp
    cDurationParser.cpp
    cIpAddressText.cpp
    cPublicKeyFileCheck.cpp
    cPublicKeyFileReader.cpp
    cReplicaConfigLoader.cpp
    cSecretFileCheck.cpp
    cSourceConfigLoader.cpp
    cTlsFileLoader.cpp
    cYamlMapReader.cpp
)
TARGET_LINK_LIBRARIES(bs-config PRIVATE yaml-cpp::yaml-cpp bs-protocol bs-net
    PUBLIC bs-warnings)
TARGET_INCLUDE_DIRECTORIES(bs-config PRIVATE "${PROJECT_SOURCE_DIR}/config")
ADD_HEADER_CHECK(bs-config-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/config" "${BINLOG_STREAMER_INCLUDE_DIR}" yaml-cpp::yaml-cpp)
ADD_HEADER_CHECK(bs-config-internal-hc
    "${PROJECT_SOURCE_DIR}/config" "${PROJECT_SOURCE_DIR}/config" yaml-cpp::yaml-cpp)
TARGET_INCLUDE_DIRECTORIES(bs-config-internal-hc PRIVATE "${PROJECT_SOURCE_DIR}/config")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
