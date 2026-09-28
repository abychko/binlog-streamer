ADD_LIBRARY(bs-receiver STATIC
    cDumpProbe.cpp
    cEventCounterSink.cpp
    cEventStreamReader.cpp
    cHeartbeatEventCodec.cpp
    cProbeStreamClassifier.cpp
    cReplicaSession.cpp
    cSessionUuid.cpp
    cSourceClock.cpp
    cStartSetResolver.cpp
    cStartupSequence.cpp
)
TARGET_LINK_LIBRARIES(bs-receiver PUBLIC bs-warnings bs-gtid bs-net PRIVATE bs-binlog bs-protocol bs-status)
ADD_HEADER_CHECK(bs-receiver-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/receiver" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
