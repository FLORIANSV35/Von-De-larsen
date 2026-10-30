add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE vc_core)
add_test(NAME core COMMAND test_core)

add_executable(bench_mask ../tools/bench_mask.cpp)
target_link_libraries(bench_mask PRIVATE vc_core)
