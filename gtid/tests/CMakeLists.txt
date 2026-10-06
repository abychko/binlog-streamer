ADD_EXECUTABLE(bs-gtid-tests
    cGtidSetTest.cpp
    cTagTextTest.cpp
    cUuidTextTest.cpp
)
TARGET_LINK_LIBRARIES(bs-gtid-tests PRIVATE bs-gtid GTest::gtest_main)
TARGET_INCLUDE_DIRECTORIES(bs-gtid-tests PRIVATE "${PROJECT_SOURCE_DIR}/gtid")
GTEST_DISCOVER_TESTS(bs-gtid-tests)
