ADD_LIBRARY(bs-status STATIC
    cRelayStatusTracker.cpp
    cReplicaRegistration.cpp
    cStatusJsonWriter.cpp
    cStreamProgress.cpp
)
# Standard library only: receiver/ and server/ feed it and must not pick up a
# dependency on anything above them through it.
TARGET_LINK_LIBRARIES(bs-status PUBLIC bs-warnings)
ADD_HEADER_CHECK(bs-status-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/status" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
