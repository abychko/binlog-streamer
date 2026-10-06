ADD_LIBRARY(bs-net-test-support INTERFACE)
TARGET_INCLUDE_DIRECTORIES(bs-net-test-support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
TARGET_LINK_LIBRARIES(bs-net-test-support INTERFACE bs-net)

ADD_EXECUTABLE(bs-net-tests
    cByteBufferTest.cpp
    cCompressedTransportTest.cpp
    cPacketChannelTest.cpp
    cTcpTransportTest.cpp
    cTlsTransportTest.cpp
    cWakeupPipeTest.cpp
)

TARGET_LINK_LIBRARIES(bs-net-tests PRIVATE bs-net-test-support bs-protocol
    libzstd_static zlib-ng GTest::gtest_main)

GTEST_DISCOVER_TESTS(bs-net-tests)
