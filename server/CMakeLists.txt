ADD_LIBRARY(bs-server STATIC
    cDumpSender.cpp
    cDumpSessionRegistry.cpp
    cDumpStartResolver.cpp
    cQueryResponder.cpp
    cReplicaClientList.cpp
    cReplicaConnection.cpp
    cReplicaListener.cpp
    cServerCertificateFiles.cpp
    cServerUuidFile.cpp
)
TARGET_LINK_LIBRARIES(bs-server PUBLIC bs-warnings bs-net
    PRIVATE bs-protocol bs-config bs-storage bs-gtid bs-binlog bs-status
    OpenSSL::Crypto)
TARGET_INCLUDE_DIRECTORIES(bs-server PRIVATE "${PROJECT_SOURCE_DIR}/server")
ADD_HEADER_CHECK(bs-server-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/server" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
