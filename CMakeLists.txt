cmake_minimum_required(VERSION 3.21)
project(SynthForgeStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Test Sql WebSockets Network WebEngineWidgets)

add_library(synthforge_core STATIC
    src/core/mcp/JsonRpcLineCodec.cpp
    src/core/mcp/McpClient.cpp
    src/core/mcp/McpRuntime.cpp
    src/core/mcp/McpTransport.h
    src/core/mcp/StdioTransport.cpp
    src/core/mcp/BridgeCommands.cpp
    src/core/preflight/PreflightChecker.cpp
    src/core/settings/SettingsStore.cpp
    src/core/settings/UiState.cpp
    src/core/store/SessionStore.cpp
    src/core/tasks/TaskManager.cpp
    src/core/ws/ProtocolRouter.cpp
    src/core/ws/WsServer.cpp
    src/core/llm/SseParser.cpp
    src/core/llm/ToolBridge.cpp
    src/core/llm/LlmGateway.cpp
    src/core/coreapp/CoreApp.cpp
)
target_include_directories(synthforge_core PUBLIC src)
target_link_libraries(synthforge_core PUBLIC Qt6::Core Qt6::Sql Qt6::WebSockets Qt6::Network)

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

add_executable(tst_mcp_runtime tests/tst_mcp_runtime.cpp)
target_link_libraries(tst_mcp_runtime PRIVATE synthforge_core Qt6::Test)
# 同样不传路径宏：测试用 applicationDirPath() 定位同目录的 mock server
add_dependencies(tst_mcp_runtime mock_mcp_server)
add_test(NAME tst_mcp_runtime COMMAND tst_mcp_runtime)
set_tests_properties(tst_mcp_runtime PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_preflight tests/tst_preflight.cpp)
target_link_libraries(tst_preflight PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_preflight COMMAND tst_preflight)
set_tests_properties(tst_preflight PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_stdio_transport tests/tst_stdio_transport.cpp)
target_link_libraries(tst_stdio_transport PRIVATE synthforge_core Qt6::Test)
# 不传路径宏：Windows 反斜杠在 C++ 字符串字面量里会被当成 \p、\b 等转义序列，
# 测试改用 QCoreApplication::applicationDirPath() 定位同目录的 mock server
add_dependencies(tst_stdio_transport mock_mcp_server)
add_test(NAME tst_stdio_transport COMMAND tst_stdio_transport)
set_tests_properties(tst_stdio_transport PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_bridge_commands tests/tst_bridge_commands.cpp)
target_link_libraries(tst_bridge_commands PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_bridge_commands COMMAND tst_bridge_commands)
set_tests_properties(tst_bridge_commands PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(synthforge_cli src/cli/main.cpp)
target_link_libraries(synthforge_cli PRIVATE synthforge_core)

# GUI 壳：web/dist 打进 qrc，app://ui/ 从 :/web/ 取文件
file(GLOB_RECURSE WEB_DIST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/dist/*")
add_executable(synthforge_app
    src/main.cpp
    src/app/MainWindow.cpp
    src/app/AppSchemeHandler.cpp)
target_link_libraries(synthforge_app PRIVATE synthforge_core Qt6::WebEngineWidgets)
# TODO(完工打包时)：set_target_properties(synthforge_app PROPERTIES WIN32_EXECUTABLE TRUE)
# 改成 GUI 子系统后双击不再弹控制台，但 stdout/stderr 也看不到了；开发期先保留控制台。
if(WEB_DIST_FILES)
    qt_add_resources(synthforge_app "web_assets"
        PREFIX "/web"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/web/dist"
        FILES ${WEB_DIST_FILES})
else()
    message(WARNING "web/dist 为空：先运行 cd web && npm run build，否则 synthforge_app 无法加载界面")
endif()

add_executable(tst_reconnect tests/tst_reconnect.cpp)
target_link_libraries(tst_reconnect PRIVATE synthforge_core Qt6::Test)
# 同样不传路径宏：测试用 applicationDirPath() 定位同目录的 mock server
add_dependencies(tst_reconnect mock_mcp_server)
add_test(NAME tst_reconnect COMMAND tst_reconnect)
set_tests_properties(tst_reconnect PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 慢测试：默认 30s 心跳节奏下真等两轮，约 65 秒；ctest -LE slow 可跳过
add_executable(tst_heartbeat_long tests/tst_heartbeat_long.cpp)
target_link_libraries(tst_heartbeat_long PRIVATE synthforge_core Qt6::Test)
add_dependencies(tst_heartbeat_long mock_mcp_server)
add_test(NAME tst_heartbeat_long COMMAND tst_heartbeat_long)
set_tests_properties(tst_heartbeat_long PROPERTIES
    LABELS "slow"
    TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_settings_store tests/tst_settings_store.cpp)
target_link_libraries(tst_settings_store PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_settings_store COMMAND tst_settings_store)
set_tests_properties(tst_settings_store PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_session_store tests/tst_session_store.cpp)
target_link_libraries(tst_session_store PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_session_store COMMAND tst_session_store)
set_tests_properties(tst_session_store PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_task_manager tests/tst_task_manager.cpp)
target_link_libraries(tst_task_manager PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_task_manager COMMAND tst_task_manager)
set_tests_properties(tst_task_manager PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_protocol_router tests/tst_protocol_router.cpp)
target_link_libraries(tst_protocol_router PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_protocol_router COMMAND tst_protocol_router)
set_tests_properties(tst_protocol_router PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_sse_parser tests/tst_sse_parser.cpp)
target_link_libraries(tst_sse_parser PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_sse_parser COMMAND tst_sse_parser)
set_tests_properties(tst_sse_parser PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_tool_bridge tests/tst_tool_bridge.cpp)
target_link_libraries(tst_tool_bridge PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_tool_bridge COMMAND tst_tool_bridge)
set_tests_properties(tst_tool_bridge PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_llm_gateway tests/tst_llm_gateway.cpp
    tests/mock/MockLlmServer.cpp tests/mock/MockLlmServer.h)
target_include_directories(tst_llm_gateway PRIVATE tests)
target_link_libraries(tst_llm_gateway PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_llm_gateway COMMAND tst_llm_gateway)
set_tests_properties(tst_llm_gateway PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_headless_ws tests/tst_headless_ws.cpp)
target_link_libraries(tst_headless_ws PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_headless_ws COMMAND tst_headless_ws)
set_tests_properties(tst_headless_ws PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_ui_state tests/tst_ui_state.cpp)
target_link_libraries(tst_ui_state PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_ui_state COMMAND tst_ui_state)
set_tests_properties(tst_ui_state PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
