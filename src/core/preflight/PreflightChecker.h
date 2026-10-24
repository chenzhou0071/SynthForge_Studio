#pragma once

#include <QString>

class SettingsStore;
class McpRuntime;

struct PreflightResult
{
    bool ok = false;
    QString reason; // error.card 的 reason 字段
};

class PreflightChecker
{
public:
    // blenderRuntime 为 nullptr 或未就绪均视为链未就绪。
    PreflightChecker(SettingsStore* settings, McpRuntime* blenderRuntime);

    PreflightResult checkLowpoly() const; // Blender 链就绪
    PreflightResult checkCloud3d() const; // 当前 3D 服务商必填项完整

private:
    SettingsStore* m_settings;
    McpRuntime* m_blenderRuntime;
};
