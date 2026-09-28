ADD_EXECUTABLE(bs-status-tests
    cRelayStatusTrackerTest.cpp
    cStatusJsonWriterTest.cpp
    cStreamProgressTest.cpp
)
TARGET_LINK_LIBRARIES(bs-status-tests PRIVATE bs-status GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-status-tests)
