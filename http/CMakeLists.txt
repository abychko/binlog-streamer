ADD_LIBRARY(bs-http STATIC
    cHtmlDirectory.cpp
    cHttpListener.cpp
    cHttpRequestParser.cpp
    cHttpResponseWriter.cpp
)
TARGET_LINK_LIBRARIES(bs-http PUBLIC bs-warnings bs-net PRIVATE bs-config)
ADD_HEADER_CHECK(bs-http-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/http" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
