#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

// MCP 工具结果 → OpenAI tool 消息：图片压成 JPEG base64，同一份既回喂模型也给前端展示。
// 原先这段只在 AgentOrchestrator.cpp 的匿名 namespace 里；讨论模式要同一份行为，
// 所以提到这里，避免复制一份以后各自漂移。
QJsonObject toolResultMessage(const QString& callId, const QJsonValue& result,
                              const QJsonObject& error, QStringList* images);

// 桥/编辑器瞬断（Unity domain reload、桥重连中）是可恢复的：等一会儿重发通常就好，
// 不该把这类错误回喂给模型当结论。只认「连不上」类信号，真错误不算。
bool isRetryableBridgeError(const QJsonValue& result, const QJsonObject& error);
