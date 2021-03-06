add_library(memcached_logger SHARED
            logger.h
            logger.cc
            spdlogger.cc
            custom_rotating_file_sink.cc
            custom_rotating_file_sink.h)

target_link_libraries(memcached_logger cJSON platform dirutils spdlog)
set_target_properties(memcached_logger PROPERTIES SOVERSION 1.0.0)
generate_export_header(memcached_logger
                       EXPORT_MACRO_NAME LOGGER_PUBLIC_API
                       EXPORT_FILE_NAME ${PROJECT_BINARY_DIR}/include/logger/visibility.h)

install(TARGETS memcached_logger
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)

if (COUCHBASE_KV_BUILD_UNIT_TESTS)
    add_executable(memcached_logger_test logger_test.cc)
    target_link_libraries(memcached_logger_test gtest gtest_main mcd_util memcached_logger dirutils)
    target_include_directories(memcached_logger_test INTERFACE PRIVATE ${Platform_SOURCE_DIR}/external)
    add_test(NAME memcached-spdlogger-test
             WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
             COMMAND memcached_logger_test)

    add_executable(memcached_logger_benchmark logger_benchmark.cc)
    target_link_libraries(memcached_logger_benchmark benchmark memcached_logger)
endif (COUCHBASE_KV_BUILD_UNIT_TESTS)
