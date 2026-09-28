ADD_EXECUTABLE(bs-receiver-tests
    cEventStreamReaderTest.cpp
    cHeartbeatEventCodecTest.cpp
    cProbeStreamClassifierTest.cpp
    cReplicaSessionTest.cpp
    cSourceClockTest.cpp
    cStartSetResolverTest.cpp
    cStartupSequenceTest.cpp
)
# bs-protocol-test-support (protocol/tests/CMakeLists.txt): RSA key-pair
# generation and RSA-OAEP-plus-XOR decryption, needed by
# cReplicaSessionTest.cpp's full-authentication scenarios.
TARGET_LINK_LIBRARIES(bs-receiver-tests PRIVATE bs-receiver bs-binlog bs-protocol GTest::gtest_main
    bs-protocol-test-support bs-net-test-support)
GTEST_DISCOVER_TESTS(bs-receiver-tests)

ADD_SUBDIRECTORY(driver)
