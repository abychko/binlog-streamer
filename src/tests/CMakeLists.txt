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
    # Only meaningful in this configuration: DEVELOPER_MODE relaxes exactly
    # the check this scenario proves is enforced, so it would fail on a true
    # negative in a dev build - not a regression, see the DeveloperScenario below.
    ADD_TEST(NAME ValidateConfigOwnerScenario
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
            -DSCENARIO=owner -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_config_test.cmake)
    SET_TESTS_PROPERTIES(ValidateConfigOwnerScenario PROPERTIES SKIP_REGULAR_EXPRESSION "skipped: running as root")
ENDIF()

ADD_TEST(NAME ValidateConfigBrokenScenario
    COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
        -DSCENARIO=broken -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_config_test.cmake)

IF(DEVELOPER_MODE)
    # Only meaningful in this configuration: the "developer" scenario proves
    # the owner/group check relaxation, which only exists when
    # BINLOG_STREAMER_DEVELOPER_MODE is defined (cmake/developer_mode.cmake).
    ADD_TEST(NAME ValidateConfigDeveloperScenario
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
            -DSCENARIO=developer -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_config_test.cmake)
ENDIF()

ADD_TEST(NAME SourceIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/source_integration_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
# RESOURCE_LOCK: shared with StorageIntegrationTest/StreamIntegrationTest
# (storage/tests/CMakeLists.txt has the full reason) - all three run against
# the same live source, and the other two force rotations on it.
SET_TESTS_PROPERTIES(SourceIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

ADD_TEST(NAME FullAuthIntegrationTest
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/full_auth_integration_test.sh $<TARGET_FILE:${PROJECT_NAME}>)
# RESOURCE_LOCK: same live source as the other integration tests - this one
# flushes and repopulates the account's cached auth hash (via an EXIT trap,
# so it holds even on a partial failure), which the others rely on being populated.
SET_TESTS_PROPERTIES(FullAuthIntegrationTest PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped:"
    RESOURCE_LOCK "binlog_streamer_live_source")

IF(DEVELOPER_MODE)
    ADD_TEST(NAME ValidateConfigCacheMinimum
        COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:${PROJECT_NAME}> -DPACKAGING_DIR=${PROJECT_SOURCE_DIR}/packaging
            -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_cache_size_test.cmake)
ENDIF()
