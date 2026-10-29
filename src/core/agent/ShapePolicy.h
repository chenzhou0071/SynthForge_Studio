#pragma once

#include <QString>

// 低模产出形态三档。写死「正方形微缩场景」的根因在三层提示词里，
// 分档后每档有自己的摘要模板、规划器提示词、执行纪律、自检方式与步数预算。
enum class OutputShape
{
    Prop,      // 道具：独立单体资产（或一组可分离小件），无场景布局，可复用
    Diorama,   // 盒式舞台：方形底盘约束的舞台场景，构图限定在方形区域内
    OpenScene, // 开放场景：无方形边界的大空间，支持长轨道、跨区域与时序动画
};

OutputShape shapeFromString(const QString& text); // 未知/空 → Diorama
QString shapeKey(OutputShape shape);              // 落盘用键名
QString shapeLabel(OutputShape shape);            // 界面与提示词用中文名

// 每档的可变参数。纪律与提示词正文用自由函数返回（长文本，塞进聚合初始化没法读）。
struct ShapePolicy
{
    QString summarizeTemplate;
    QString askTemplate;
    int budgetPerGroup = 30;
    int budgetCap = 200;
    int minGroups = 1;
    int maxGroups = 6;
};

const ShapePolicy& shapePolicy(OutputShape shape);
int shapeBudget(OutputShape shape, int groupCount);

QString planSystemPromptFor(OutputShape shape);
QString shapeDiscipline(OutputShape shape);  // 接在通用布局纪律之后
QString shapeSelfCheck(OutputShape shape);   // 截图几次、从哪些机位
