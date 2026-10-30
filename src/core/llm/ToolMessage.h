#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

// MCP 工具结果 → OpenAI tool 消息：图片压成 JPEG base64，同一份既回喂模型也给前端展示。
// 原先这段只在 AgentOrchestrator.cpp 的匿名 namespace 里；讨论模式要同一份行为，
// 所以提到这里，避免复制一份以后各自漂移。
QJsonObject toolResultMessage(const QString& callId, const QJsonValue& result,
                              const QJsonObject& error, QStringList* images);
