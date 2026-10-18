cmake_minimum_required(VERSION 3.21)
project(SynthForgeStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Test)

add_library(synthforge_core STATIC
    src/core/mcp/JsonRpcLineCodec.cpp
    src/core/mcp/McpClient.cpp
    src/core/mcp/McpTransport.h
)
target_include_directories(synthforge_core PUBLIC src)
target_link_libraries(synthforge_core PUBLIC Qt6::Core)

enable_testing()

add_executable(tst_smoke tests/tst_smoke.cpp)
target_link_libraries(tst_smoke PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_smoke COMMAND tst_smoke)
set_tests_properties(tst_smoke PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_jsonrpc tests/tst_jsonrpc.cpp)
target_link_libraries(tst_jsonrpc PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_jsonrpc COMMAND tst_jsonrpc)
set_tests_properties(tst_jsonrpc PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_mcpclient tests/tst_mcpclient.cpp)
target_link_libraries(tst_mcpclient PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_mcpclient COMMAND tst_mcpclient)
set_tests_properties(tst_mcpclient PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
