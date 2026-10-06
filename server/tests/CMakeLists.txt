ADD_EXECUTABLE(bs-server-tests
    cDumpSenderLoopbackTest.cpp
    cDumpSessionRegistryTest.cpp
    cDumpStartResolverTest.cpp
    cQueryResponderTest.cpp
    hServerVersionTest.cpp
    cReplicaClientListTest.cpp
    cReplicaConnectionTest.cpp
    cReplicaListenerLoopbackTest.cpp
    cServerUuidFileTest.cpp
    cServerCertificateFilesTest.cpp
)
TARGET_LINK_LIBRARIES(bs-server-tests PRIVATE bs-server bs-net-test-support
    bs-protocol bs-config bs-receiver bs-storage bs-gtid bs-binlog GTest::gtest_main)
TARGET_INCLUDE_DIRECTORIES(bs-server-tests PRIVATE "${PROJECT_SOURCE_DIR}/server")
GTEST_DISCOVER_TESTS(bs-server-tests)
