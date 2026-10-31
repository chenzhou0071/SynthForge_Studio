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
    src/core/mcp/BlenderGate.cpp
    src/core/preflight/PreflightChecker.cpp
    src/core/settings/SettingsStore.cpp
    src/core/settings/UiState.cpp
    src/core/store/SessionStore.cpp
    src/core/tasks/TaskManager.cpp
    src/core/unity/ProjectService.cpp
    src/core/unity/DirectoryWatcher.cpp
    src/core/unity/UnityTools.cpp
    src/core/unity/AssetImporter.cpp
    src/core/ws/ProtocolRouter.cpp
    src/core/ws/WsServer.cpp
    src/core/llm/SseParser.cpp
    src/core/llm/ToolBridge.cpp
    src/core/llm/ToolMessage.cpp
    src/core/llm/LlmGateway.cpp
    src/core/agent/SummarizerAgent.cpp
    src/core/agent/AskReply.cpp
    src/core/agent/ShapePolicy.cpp
    src/core/agent/SceneLoader.cpp
    src/core/agent/DiscussionAgent.cpp
    src/core/agent/AgentOrchestrator.cpp
    src/core/assets/AssetLibrary.cpp
    src/core/cloud3d/Cloud3dProvider.cpp
    src/core/cloud3d/Cloud3dService.cpp
    src/core/cloud3d/CustomProvider.cpp
    src/core/cloud3d/HunyuanProvider.cpp
    src/core/cloud3d/Tc3Signature.cpp
    src/core/cloud3d/TripoProvider.cpp
    src/core/coreapp/AssetCard.cpp
    src/core/coreapp/CoreApp.cpp
)
target_include_directories(synthforge_core PUBLIC src)
target_link_libraries(synthforge_core PUBLIC Qt6::Core Qt6::Sql Qt6::WebSockets Qt6::Network Qt6::Gui)

# 提示词模板编进 core 库；SummarizerAgent 显式 Q_INIT_RESOURCE 以保证静态库资源被链接
qt_add_resources(synthforge_core "prompts"
    PREFIX "/prompts"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer-modify.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer-cloud3d.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer-prop.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/summarizer-open.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/ask.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/ask-prop.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/prompts/ask-open.md")

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

add_executable(tst_project_service tests/tst_project_service.cpp)
target_link_libraries(tst_project_service PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_project_service COMMAND tst_project_service)
set_tests_properties(tst_project_service PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_folder_pick tests/tst_folder_pick.cpp)
target_link_libraries(tst_folder_pick PRIVATE synthforge_core Qt6::Test Qt6::WebSockets)
add_test(NAME tst_folder_pick COMMAND tst_folder_pick)
set_tests_properties(tst_folder_pick PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_watcher tests/tst_watcher.cpp)
target_link_libraries(tst_watcher PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_watcher COMMAND tst_watcher)
set_tests_properties(tst_watcher PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_unity_tools tests/tst_unity_tools.cpp)
target_link_libraries(tst_unity_tools PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_unity_tools COMMAND tst_unity_tools)
set_tests_properties(tst_unity_tools PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_asset_importer tests/tst_asset_importer.cpp)
target_link_libraries(tst_asset_importer PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_asset_importer COMMAND tst_asset_importer)
set_tests_properties(tst_asset_importer PROPERTIES
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

# 应用图标：exe 资源（Windows 任务栏/资源管理器）+ qrc（窗口图标，跨平台）
if(WIN32)
    set(APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/app.ico")
    configure_file(src/resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(synthforge_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()
qt_add_resources(synthforge_app "app_icons"
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/app.png")
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

add_executable(tst_ask_reply tests/tst_ask_reply.cpp)
target_link_libraries(tst_ask_reply PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_ask_reply COMMAND tst_ask_reply)
set_tests_properties(tst_ask_reply PROPERTIES
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

add_executable(tst_summarizer tests/tst_summarizer.cpp
    tests/mock/MockLlmServer.cpp tests/mock/MockLlmServer.h)
target_include_directories(tst_summarizer PRIVATE tests)
target_link_libraries(tst_summarizer PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_summarizer COMMAND tst_summarizer)
set_tests_properties(tst_summarizer PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_orchestrator tests/tst_orchestrator.cpp
    tests/mock/MockLlmServer.cpp tests/mock/MockLlmServer.h)
target_include_directories(tst_orchestrator PRIVATE tests)
target_link_libraries(tst_orchestrator PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_orchestrator COMMAND tst_orchestrator)
set_tests_properties(tst_orchestrator PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_asset_library tests/tst_asset_library.cpp)
target_link_libraries(tst_asset_library PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_asset_library COMMAND tst_asset_library)
set_tests_properties(tst_asset_library PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_headless_ws tests/tst_headless_ws.cpp tests/mock/MockLlmServer.cpp
    tests/mock/MockCloud3dServer.cpp)
target_include_directories(tst_headless_ws PRIVATE tests)
target_link_libraries(tst_headless_ws PRIVATE synthforge_core Qt6::Test)
# 假 Blender 桥接：mock_mcp_server 让 preflight 走真实握手
add_dependencies(tst_headless_ws mock_mcp_server)
add_test(NAME tst_headless_ws COMMAND tst_headless_ws)
set_tests_properties(tst_headless_ws PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_asset_card tests/tst_asset_card.cpp)
target_link_libraries(tst_asset_card PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_asset_card COMMAND tst_asset_card)
set_tests_properties(tst_asset_card PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_shape_policy tests/tst_shape_policy.cpp)
target_link_libraries(tst_shape_policy PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_shape_policy COMMAND tst_shape_policy)
set_tests_properties(tst_shape_policy PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_scene_loader tests/tst_scene_loader.cpp)
target_link_libraries(tst_scene_loader PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_scene_loader COMMAND tst_scene_loader)
set_tests_properties(tst_scene_loader PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_discussion tests/tst_discussion.cpp
    tests/mock/MockLlmServer.cpp tests/mock/MockLlmServer.h)
target_include_directories(tst_discussion PRIVATE tests)
target_link_libraries(tst_discussion PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_discussion COMMAND tst_discussion)
set_tests_properties(tst_discussion PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_ui_state tests/tst_ui_state.cpp)
target_link_libraries(tst_ui_state PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_ui_state COMMAND tst_ui_state)
set_tests_properties(tst_ui_state PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_blender_gate tests/tst_blender_gate.cpp)
target_link_libraries(tst_blender_gate PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_blender_gate COMMAND tst_blender_gate)
set_tests_properties(tst_blender_gate PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_tc3 tests/tst_tc3.cpp)
target_link_libraries(tst_tc3 PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_tc3 COMMAND tst_tc3)
set_tests_properties(tst_tc3 PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(tst_cloud3d tests/tst_cloud3d.cpp tests/mock/MockCloud3dServer.cpp)
target_include_directories(tst_cloud3d PRIVATE tests)
target_link_libraries(tst_cloud3d PRIVATE synthforge_core Qt6::Test)
add_test(NAME tst_cloud3d COMMAND tst_cloud3d)
set_tests_properties(tst_cloud3d PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 无 Key 验收用的可独立运行假云服务（见 tests/mock/mock_cloud3d_main.cpp）
add_executable(mock_cloud3d_server tests/mock/mock_cloud3d_main.cpp)
set_target_properties(mock_cloud3d_server PROPERTIES WIN32_EXECUTABLE OFF)
target_link_libraries(mock_cloud3d_server PRIVATE Qt6::Core Qt6::Network)
