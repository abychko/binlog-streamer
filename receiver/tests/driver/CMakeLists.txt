# Test-only: not installed, not part of the shipped relay.
ADD_EXECUTABLE(bs-stream-driver
    cFileEventSink.cpp
    cStreamDriver.cpp
    main.cpp
)
TARGET_LINK_LIBRARIES(bs-stream-driver PRIVATE
    bs-receiver
    bs-binlog
    bs-config
    bs-gtid
    bs-warnings
)
TARGET_COMPILE_DEFINITIONS(bs-stream-driver PRIVATE
    BINLOG_STREAMER_STREAM_DRIVER_VERSION="${PROJECT_VERSION}")
# SourceConfigLoader is config/'s own internal header, reachable here the
# same way config/tests/ reaches module internals.
TARGET_INCLUDE_DIRECTORIES(bs-stream-driver PRIVATE "${PROJECT_SOURCE_DIR}/config")

ADD_TEST(NAME StreamIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stream_integration_test.sh
        $<TARGET_FILE:bs-stream-driver>)
# RESOURCE_LOCK: shared with StorageIntegrationTest/SourceIntegrationTest -
# this test's own FLUSH BINARY LOGS races the same live source they rotate.
SET_TESTS_PROPERTIES(StreamIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")
