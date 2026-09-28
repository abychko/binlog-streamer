# INTERFACE: an include path plus OpenSSL::Crypto, not a compiled library -
# the test-support headers here are header-only, reused by receiver/tests
# for the same RSA key-pair generation and RSA-OAEP-plus-XOR decryption.
ADD_LIBRARY(bs-protocol-test-support INTERFACE)
TARGET_INCLUDE_DIRECTORIES(bs-protocol-test-support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
TARGET_LINK_LIBRARIES(bs-protocol-test-support INTERFACE OpenSSL::Crypto)

ADD_EXECUTABLE(bs-protocol-tests
    cAuthMoreDataCodecTest.cpp
    cAuthSwitchRequestCodecTest.cpp
    cCachingSha2FullAuthPasswordTest.cpp
    cCachingSha2ScrambleTest.cpp
    cColumnDefinition41CodecTest.cpp
    cComBinlogDumpGtidCommandTest.cpp
    cComQueryCommandTest.cpp
    cComQuitCommandTest.cpp
    cComRegisterSlaveCommandTest.cpp
    cEofPacketCodecTest.cpp
    cErrPacketCodecTest.cpp
    cHandshakeResponse41CodecTest.cpp
    cHandshakeV10CodecTest.cpp
    cLengthEncodedIntegerTest.cpp
    cLengthEncodedStringTest.cpp
    cOkPacketCodecTest.cpp
    cPacketFramerTest.cpp
    cPublicKeyRequestCodecTest.cpp
    cRsaPublicKeyTest.cpp
    cTextRowCodecTest.cpp
)
TARGET_LINK_LIBRARIES(bs-protocol-tests PRIVATE bs-protocol GTest::gtest_main
    bs-protocol-test-support)
GTEST_DISCOVER_TESTS(bs-protocol-tests)
