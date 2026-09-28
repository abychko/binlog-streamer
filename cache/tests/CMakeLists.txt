ADD_EXECUTABLE(bs-cache-tests cEventCacheTest.cpp cEventCacheConcurrencyTest.cpp cEventCacheWindowTest.cpp cEventCacheWaitTest.cpp)
TARGET_LINK_LIBRARIES(bs-cache-tests PRIVATE bs-cache GTest::gtest_main)
GTEST_DISCOVER_TESTS(bs-cache-tests PROPERTIES TIMEOUT 30)

ADD_EXECUTABLE(cache_contention_bench cEventCacheContentionBench.cpp)
TARGET_LINK_LIBRARIES(cache_contention_bench PRIVATE bs-cache)

ADD_EXECUTABLE(cache_maintenance_bench cEventCacheMaintenanceBench.cpp)
TARGET_LINK_LIBRARIES(cache_maintenance_bench PRIVATE bs-cache)
