ADD_LIBRARY(bs-http STATIC
    cHtmlDirectory.cpp
    cHttpListener.cpp
    cHttpRequestParser.cpp
    cHttpResponseWriter.cpp
)
# bs-net PUBLIC: cHttpListener.hpp takes a WakeupPipe. bs-config: the
# peer's address is named with IpAddressText.
TARGET_LINK_LIBRARIES(bs-http PUBLIC bs-warnings bs-net PRIVATE bs-config)
ADD_HEADER_CHECK(bs-http-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/http" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
