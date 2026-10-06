ADD_LIBRARY(bs-net STATIC
    cByteBuffer.cpp
    cCompressedTransport.cpp
    cListenSocket.cpp
    cPacketChannel.cpp
    cTcpTransport.cpp
    cTlsCertificateGenerator.cpp
    cTlsContext.cpp
    cTlsTransport.cpp
    cWakeupPipe.cpp
)

TARGET_LINK_LIBRARIES(bs-net PUBLIC bs-warnings
    PRIVATE bs-protocol libzstd_static zlib-ng OpenSSL::SSL)

ADD_HEADER_CHECK(bs-net-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/net" "${BINLOG_STREAMER_INCLUDE_DIR}")

IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
