ADD_EXECUTABLE(bs-binlog-tests
    cBinlogFileNameTest.cpp
    cEventHeaderCodecTest.cpp
    cFormatDescriptionEventCodecTest.cpp
    cGtidEventCodecTest.cpp
    cRotateEventCodecTest.cpp
    cSerializationDecoderTest.cpp
    cTransactionBoundaryTrackerTest.cpp
)
TARGET_LINK_LIBRARIES(bs-binlog-tests PRIVATE bs-binlog GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-binlog-tests)
