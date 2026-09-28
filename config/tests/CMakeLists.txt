ADD_EXECUTABLE(bs-config-tests
    cAddressRangeMatcherTest.cpp
    cAddressRangeParserTest.cpp
    cByteSizeParserTest.cpp
    cConfigErrorPrinterTest.cpp
    cConfigLoaderTest.cpp
    cConfigurationLoaderTest.cpp
    cConfigurationReloadTest.cpp
    cConfigValidatorTest.cpp
    cConfigValidatorCacheSizeTest.cpp
    cDiskProtectedFileReaderTest.cpp
    cDurationParserTest.cpp
    cIpAddressTextTest.cpp
    cPublicKeyFileCheckTest.cpp
    cPublicKeyFileReaderTest.cpp
    cReplicaConfigLoaderTest.cpp
    cSecretFileCheckTest.cpp
    cSourceConfigLoaderTest.cpp
)
# OpenSSL::Crypto: cSourceConfigLoaderTest.cpp generates its own RSA test
# keys directly. bs-config's own dependency on bs-protocol/OpenSSL::Crypto
# is PRIVATE, so it doesn't propagate the include dirs this target needs.
TARGET_LINK_LIBRARIES(bs-config-tests PRIVATE bs-config bs-net GTest::gtest_main
    OpenSSL::Crypto)
# Tests exercise the module's internal loaders/parsers directly (bare
# includes); this path is module-scoped, not global.
TARGET_INCLUDE_DIRECTORIES(bs-config-tests PRIVATE "${PROJECT_SOURCE_DIR}/config")
# Read by cConfigurationLoaderTest to validate the shipped settings.yml and the
# unfilled source.yml/replica.yml templates without duplicating their content.
TARGET_COMPILE_DEFINITIONS(bs-config-tests PRIVATE
    BINLOG_STREAMER_PACKAGING_DIR="${PROJECT_SOURCE_DIR}/packaging")
GTEST_DISCOVER_TESTS(bs-config-tests)
