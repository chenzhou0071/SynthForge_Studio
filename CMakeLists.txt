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
    src/core/mcp/StdioTransport.cpp
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

add_executable(mock_mcp_server tests/mock/mock_mcp_server.cpp)
target_link_libraries(mock_mcp_server PRIVATE Qt6::Core)

add_executable(tst_stdio_transport tests/tst_stdio_transport.cpp)
target_link_libraries(tst_stdio_transport PRIVATE synthforge_core Qt6::Test)
# 不传路径宏：Windows 反斜杠在 C++ 字符串字面量里会被当成 \p、\b 等转义序列，
# 测试改用 QCoreApplication::applicationDirPath() 定位同目录的 mock server
add_dependencies(tst_stdio_transport mock_mcp_server)
add_test(NAME tst_stdio_transport COMMAND tst_stdio_transport)
set_tests_properties(tst_stdio_transport PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
