ADD_LIBRARY(bs-protocol STATIC
    cAuthMoreDataCodec.cpp
    cAuthSwitchRequestCodec.cpp
    cCachingSha2FullAuthPassword.cpp
    cCachingSha2Scramble.cpp
    cColumnDefinition41Codec.cpp
    cComBinlogDumpGtidCommand.cpp
    cComQueryCommand.cpp
    cComQuitCommand.cpp
    cComRegisterSlaveCommand.cpp
    cEofPacketCodec.cpp
    cErrPacketCodec.cpp
    cHandshakeResponse41Codec.cpp
    cHandshakeV10Codec.cpp
    cLengthEncodedInteger.cpp
    cLengthEncodedString.cpp
    cOkPacketCodec.cpp
    cPacketFramer.cpp
    cPublicKeyRequestCodec.cpp
    cRsaPublicKey.cpp
    cTextRowCodec.cpp
)
TARGET_LINK_LIBRARIES(bs-protocol PUBLIC bs-warnings PRIVATE OpenSSL::Crypto)
ADD_HEADER_CHECK(bs-protocol-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/protocol" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
