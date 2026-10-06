ADD_EXECUTABLE(bs-command-line-tests cCommandLineTest.cpp)
TARGET_LINK_LIBRARIES(bs-command-line-tests PRIVATE bs-command-line GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-command-line-tests)

ADD_EXECUTABLE(bs-server-state-tests cStorageServerStateTest.cpp cStorageStatusFactsTest.cpp)
TARGET_INCLUDE_DIRECTORIES(bs-server-state-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
TARGET_LINK_LIBRARIES(bs-server-state-tests PRIVATE bs-server-state GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-server-state-tests)

ADD_EXECUTABLE(bs-host-memory-tests cCacheSizeWarningTest.cpp cHostMemoryTest.cpp)
TARGET_LINK_LIBRARIES(bs-host-memory-tests PRIVATE bs-host-memory bs-warnings GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-host-memory-tests)

ADD_EXECUTABLE(bs-reload-signal-tests cReloadSignalThreadTest.cpp)
TARGET_INCLUDE_DIRECTORIES(bs-reload-signal-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
TARGET_LINK_LIBRARIES(bs-reload-signal-tests PRIVATE bs-reload-signal GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-reload-signal-tests)

ADD_EXECUTABLE(bs-systemd-notify-tests cSystemdNotifyTest.cpp)
TARGET_INCLUDE_DIRECTORIES(bs-systemd-notify-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
TARGET_LINK_LIBRARIES(bs-systemd-notify-tests PRIVATE bs-systemd-notify GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-systemd-notify-tests)

IF(NOT DEVELOPER_MODE)
    # Only meaningful without DEVELOPER_MODE, which relaxes exactly the check
    # this scenario proves.
    ADD_TEST(NAME ValidateConfigOwnerScenario
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
            -DSCENARIO=owner -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_config_test.cmake)
    SET_TESTS_PROPERTIES(ValidateConfigOwnerScenario PROPERTIES SKIP_REGULAR_EXPRESSION "skipped: running as root")
ENDIF()

ADD_TEST(NAME ValidateConfigBrokenScenario
    COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
        -DSCENARIO=broken -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_config_test.cmake)

IF(DEVELOPER_MODE)
    # Only meaningful with DEVELOPER_MODE: proves the relaxed owner/group check.
    ADD_TEST(NAME ValidateConfigDeveloperScenario
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
            -DSCENARIO=developer -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_config_test.cmake)
ENDIF()

ADD_TEST(NAME SourceIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/source_integration_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
# RESOURCE_LOCK: one live source shared with the other integration tests, which
# force rotations on it.
SET_TESTS_PROPERTIES(SourceIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

ADD_TEST(NAME FullAuthIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/full_auth_integration_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
# RESOURCE_LOCK: shared live source; this test repopulates the account's cached
# auth hash, which the others rely on.
SET_TESTS_PROPERTIES(FullAuthIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

IF(DEVELOPER_MODE)
    ADD_TEST(NAME ValidateConfigCacheMinimum
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
            -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_cache_size_test.cmake)
ENDIF()
