#pragma once

#include <QString>

class AssetLibrary;

// 讨论模式要「看」资产就必须把 .blend 载入 Blender 会话，而 open_mainfile 是全量替换当前场景。
// 这两只函数把风险收在确定性代码里：路径解析与载入脚本都不交给 LLM 决定。
QString resolveBlendSource(const AssetLibrary& library, const QString& assetPath);

// 确认弹窗已明说「会清空当前场景」，用户据此自己去保存；系统不再自动备份。
// open_mainfile 本身就全量替换当前场景，所以这里只负责打开目标。
QString sceneLoadScript(const QString& targetBlend);
