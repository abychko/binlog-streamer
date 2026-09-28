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
# bs-net PUBLIC: cReplicaListener.hpp takes a WakeupPipe in its constructor.
# OpenSSL::Crypto PRIVATE: cReplicaConnection.cpp's CRYPTO_memcmp use (a
# constant-time scramble comparison) is not exposed in any public header.
TARGET_LINK_LIBRARIES(bs-server PUBLIC bs-warnings bs-net
    PRIVATE bs-protocol bs-config bs-storage bs-gtid bs-binlog bs-status
    OpenSSL::Crypto)
# Internal headers, not part of this module's public interface, are included
# by bare filename.
TARGET_INCLUDE_DIRECTORIES(bs-server PRIVATE "${PROJECT_SOURCE_DIR}/server")
ADD_HEADER_CHECK(bs-server-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/server" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
