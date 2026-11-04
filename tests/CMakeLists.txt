include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
# keep gtest on the same CRT as us on MSVC
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(matchbook_tests
  test_book.cpp
  test_book_differential.cpp
  test_price.cpp
  test_types.cpp
)
target_link_libraries(matchbook_tests PRIVATE matchbook matchbook_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(matchbook_tests)
