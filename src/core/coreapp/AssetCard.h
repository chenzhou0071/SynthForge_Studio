#pragma once

#include <QJsonObject>
#include <QString>

// 资产产出卡的数据整理：纯函数便于单测；落库与广播由 CoreApp 负责。
struct AssetCardInput
{
    QString name;
    QString source;    // lowpoly | cloud3d
    QString provider;  // 云服务商；低模为空
    QString model;     // 云模型/版本；低模为空
    QString prompt;    // 最终提示词，卡片只展示摘要
    QString shape;     // 产出形态键名 prop|diorama|open；空 = diorama（旧资产与旧任务）
    QJsonObject paths; // glb / fbx / blend → 相对路径
    QString dir;       // 相对目录；根目录为空
    QString createdAt;
};

// 取提示词第一段正文，压成一行，最多 limit 个字（超出加省略号）。
QString promptSummary(const QString& prompt, int limit = 20);

// 卡片载荷：名称、来源、服务商/模型、提示词摘要、文件列表、目录、时间；不含面数。
QJsonObject makeAssetCard(const AssetCardInput& input);
