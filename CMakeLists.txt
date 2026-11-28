cmake_minimum_required(VERSION 3.20)
project(crow LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(BUILD_TESTS "Build project tests" ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# libcurl
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
add_subdirectory(third-party/curl EXCLUDE_FROM_ALL)

# libxml2
set(LIBXML2_WITH_PYTHON OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_ICONV OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_LZMA OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/libxml2 EXCLUDE_FROM_ALL)

# sqlitecpp
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/sqlite-cpp EXCLUDE_FROM_ALL)

configure_file(
    ${CMAKE_SOURCE_DIR}/db/schema.sql 
    ${CMAKE_BINARY_DIR}/db/schema.sql 
    COPYONLY 
)


add_executable(crow 
    src/main.cpp
    db/database.cpp
    src/indexer/HTML_Parser.cpp
)

target_link_libraries(crow PRIVATE 
    CURL::libcurl 
    LibXml2::LibXml2 
    SQLiteCpp
)
target_include_directories(crow PRIVATE
        ${CMAKE_SOURCE_DIR}/src/indexer
        ${CMAKE_SOURCE_DIR}/db
    )
# ============================================================
# TESTING
# ============================================================

if(BUILD_TESTS)

    enable_testing()

    # --------------------------------------------------------
    # Database - Crawler Tests
    # --------------------------------------------------------

    add_executable(db_crawler_tests
        tests/db/db_crawler_tests.cpp
        db/database.cpp
    )

    target_link_libraries(db_crawler_tests PRIVATE
        SQLiteCpp
    )

    target_include_directories(db_crawler_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/db
    )

    add_test(
        NAME db_crawler_tests
        COMMAND db_crawler_tests
    )

    set_tests_properties(db_crawler_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    add_executable(html_parser_test
    tests/indexer/main_HTML_Parser_test.cpp
    src/indexer/HTML_Parser.cpp
    )

    target_link_libraries(html_parser_test PRIVATE
    LibXml2::LibXml2    
    )
 
    target_include_directories(html_parser_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/indexer
    )

    add_test(
    NAME html_parser_test
    COMMAND html_parser_test
    )

    set_tests_properties(html_parser_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()


#add_executable(test_name
#    path/to/test.cpp
#    path/to/implementation.cpp
#)

#target_link_libraries(test_name PRIVATE
#    required_libraries
#)

#target_include_directories(test_name PRIVATE
#    required_include_directories
#)

#add_test(
#NAME test_name
#COMMAND test_name
#)

#set_tests_properties(test_name PROPERTIES
#    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
#)
