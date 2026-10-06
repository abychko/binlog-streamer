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
TARGET_LINK_LIBRARIES(bs-config-tests PRIVATE bs-config bs-net GTest::gtest_main
    OpenSSL::Crypto)
TARGET_INCLUDE_DIRECTORIES(bs-config-tests PRIVATE "${PROJECT_SOURCE_DIR}/config")
TARGET_COMPILE_DEFINITIONS(bs-config-tests PRIVATE
    BINLOG_STREAMER_PACKAGING_DIR="${PROJECT_SOURCE_DIR}/packaging")
GTEST_DISCOVER_TESTS(bs-config-tests)
