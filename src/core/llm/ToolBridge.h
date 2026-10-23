#pragma once

#include <QJsonArray>

namespace ToolBridge {

// MCP {name, description, inputSchema} → OpenAI {type:"function", function:{...}}
QJsonArray mcpToolsToOpenAi(const QJsonArray& mcpTools);

// 内部 {id, name, arguments(object)} → assistant 消息要求的 wire 格式（arguments 为字符串）
QJsonArray internalCallsToWire(const QJsonArray& calls);

} // namespace ToolBridge
