ADD_LIBRARY(bs-binlog STATIC
    cBinlogFileName.cpp
    cCrc32.cpp
    cEventHeaderCodec.cpp
    cFormatDescriptionEventCodec.cpp
    cGtidEventCodec.cpp
    cRotateEventCodec.cpp
    cSerializationDecoder.cpp
    cTransactionBoundaryTracker.cpp
)
TARGET_LINK_LIBRARIES(bs-binlog PUBLIC bs-warnings bs-gtid
    PRIVATE bs-protocol)
ADD_HEADER_CHECK(bs-binlog-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/binlog" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
