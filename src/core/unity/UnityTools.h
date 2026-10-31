#pragma once

#include <QJsonArray>

// Unity 区工具白名单：只放行编辑器操作类前缀（manage_/create_/read_/find_/import_/
// refresh_/validate_/batch_），挡掉 generate_*、execute_code 等不在本次范围的工具。
QJsonArray filterUnityTools(const QJsonArray& all);
