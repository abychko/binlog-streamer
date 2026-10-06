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
TARGET_INCLUDE_DIRECTORIES(bs-stream-driver PRIVATE "${PROJECT_SOURCE_DIR}/config")

ADD_TEST(NAME StreamIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/stream_integration_test.sh
        $<TARGET_FILE:bs-stream-driver>)
# RESOURCE_LOCK: this test's FLUSH BINARY LOGS races the same live source the
# other integration tests rotate.
SET_TESTS_PROPERTIES(StreamIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")
