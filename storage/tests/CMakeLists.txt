ADD_EXECUTABLE(bs-storage-tests
    cBinlogFileHeaderReaderTest.cpp
    cBinlogFileWriterTest.cpp
    cBinlogIndexFileTest.cpp
    cBinlogStorageTest.cpp
    cBinlogTailScannerTest.cpp
    cFileCursorTest.cpp
    cReadOnlyBinlogFileTest.cpp
    cFilePinTest.cpp
    cPublishedPositionTrackerTest.cpp
    cStorageWriterTest.cpp
    cStorageCatalogTest.cpp
    cStoragePurgerTest.cpp
    cStorageEventSinkTest.cpp
    cStorageEventSinkCacheTest.cpp
    cStorageReaderTest.cpp
    cStorageReaderCacheTest.cpp
    cStorageRecoveryTest.cpp
)
TARGET_INCLUDE_DIRECTORIES(bs-storage-tests PRIVATE ${PROJECT_SOURCE_DIR}/receiver/tests)
TARGET_LINK_LIBRARIES(bs-storage-tests PRIVATE bs-storage bs-receiver
    bs-protocol bs-net-test-support GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-storage-tests PROPERTIES TIMEOUT 30)

ADD_TEST(NAME StorageIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/storage_integration_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
# Shares RESOURCE_LOCK with the other integration tests: they all force
# rotations on the same live source, so ctest -j must not run two at once.
SET_TESTS_PROPERTIES(StorageIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

ADD_TEST(NAME StorageResumeTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/storage_resume_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
SET_TESTS_PROPERTIES(StorageResumeTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

ADD_TEST(NAME StorageExpiryTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/storage_expiry_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
SET_TESTS_PROPERTIES(StorageExpiryTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

ADD_TEST(NAME StorageSpaceTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/storage_space_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
SET_TESTS_PROPERTIES(StorageSpaceTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

# std::thread needs -lpthread on Linux/libstdc++ (macOS/libc++ links it in
# already); this target does not link bs-protocol, so it needs it explicitly.
FIND_PACKAGE(Threads REQUIRED)
ADD_EXECUTABLE(storage_sink_bench cStorageEventSinkBench.cpp)
TARGET_LINK_LIBRARIES(storage_sink_bench PRIVATE bs-storage bs-receiver Threads::Threads)
ADD_TEST(NAME StorageSinkBenchSmoke COMMAND storage_sink_bench --groups 1000 --readers 1)
ADD_TEST(NAME StorageSinkBenchWindowSmoke COMMAND storage_sink_bench --readers 1 --readers-lag 2 --rate 5 --window 1 --groups 27000)

ADD_EXECUTABLE(storage_writer_bench cStorageWriterBench.cpp)
TARGET_LINK_LIBRARIES(storage_writer_bench PRIVATE bs-storage Threads::Threads)
ADD_TEST(NAME StorageWriterBenchSmoke COMMAND storage_writer_bench 744 1048576)
SET_TESTS_PROPERTIES(StorageWriterBenchSmoke PROPERTIES TIMEOUT 30)
