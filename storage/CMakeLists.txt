ADD_LIBRARY(bs-storage STATIC
    cBinlogFileHeaderReader.cpp
    cBinlogFileWriter.cpp
    cBinlogHeaderBuilder.cpp
    cReadOnlyBinlogFile.cpp
    cBinlogIndexFile.cpp
    cBinlogStorage.cpp
    cBinlogTailScanner.cpp
    cFileCursor.cpp
    cFilePin.cpp
    cPublishedPositionTracker.cpp
    cStorageCatalog.cpp
    cStoragePurger.cpp
    cStorageWriter.cpp
    cStorageEventSink.cpp
    cStorageReader.cpp
    cStorageRecovery.cpp
)
FIND_PACKAGE(Threads REQUIRED)
TARGET_LINK_LIBRARIES(bs-storage PUBLIC Threads::Threads bs-warnings bs-cache PRIVATE bs-binlog)
ADD_HEADER_CHECK(bs-storage-hc
    "${BINLOG_STREAMER_INCLUDE_DIR}/storage" "${BINLOG_STREAMER_INCLUDE_DIR}")
IF(BUILD_TESTING)
    ADD_SUBDIRECTORY(tests)
ENDIF()
