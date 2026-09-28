ADD_EXECUTABLE(bs-gtid-tests
    cGtidSetTest.cpp
    cTagTextTest.cpp
    cUuidTextTest.cpp
)
TARGET_LINK_LIBRARIES(bs-gtid-tests PRIVATE bs-gtid GTest::gtest_main)
# Tests exercise the module's internal parsers directly (bare includes);
# this path is module-scoped, not global.
TARGET_INCLUDE_DIRECTORIES(bs-gtid-tests PRIVATE "${PROJECT_SOURCE_DIR}/gtid")
GTEST_DISCOVER_TESTS(bs-gtid-tests)
