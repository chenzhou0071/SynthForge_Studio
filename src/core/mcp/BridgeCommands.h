#pragma once

#include <QString>

#include "core/mcp/StdioTransport.h" // BridgeCommand

// area: "blender" 或 "unity"；未知 area 返回 program 为空的命令。
BridgeCommand defaultBridgeCommand(const QString& area);
