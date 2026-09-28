ADD_EXECUTABLE(bs-http-tests
    cHtmlDirectoryTest.cpp
    cHttpListenerLoopbackTest.cpp
    cHttpRequestParserTest.cpp
    cHttpResponseWriterTest.cpp
)
TARGET_LINK_LIBRARIES(bs-http-tests PRIVATE bs-http bs-config GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-http-tests)
