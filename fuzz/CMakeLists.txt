add_executable(fuzz_session fuzz_session.cpp)
target_link_libraries(fuzz_session PRIVATE matchbook)
target_compile_options(fuzz_session PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(fuzz_session PRIVATE -fsanitize=fuzzer,address,undefined)
