#include "core/coreapp/AssetCard.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

namespace {

// 去掉 markdown 标题符号与「主题与视角：」这类节标题，只留下正文
QString firstBodyLine(const QString& prompt)
{
    static const QRegularExpression label(
        QStringLiteral("^\\s*[-*]?\\s*[^：:\\n]{1,12}[：:]\\s*"));
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));

    for (QString line : prompt.split(QLatin1Char('\n'))) {
        if (line.trimmed().startsWith(QLatin1Char('#')))
            continue; // 标题不是正文
        line.remove(label);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        line.replace(whitespace, QStringLiteral(" "));
        return line;
    }
    return {};
}

} // namespace

QString promptSummary(const QString& prompt, int limit)
{
    QString line = firstBodyLine(prompt);
    if (limit > 0 && line.size() > limit)
        line = line.left(limit) + QStringLiteral("…");
    return line;
}

QJsonObject makeAssetCard(const AssetCardInput& input)
{
    QJsonArray files;
    for (const QString& suffix :
         {QStringLiteral("glb"), QStringLiteral("fbx"), QStringLiteral("blend")}) {
        const QString path = input.paths.value(suffix).toString();
        if (path.isEmpty())
            continue;
        files.append(QJsonObject{{QStringLiteral("suffix"), suffix},
                                 {QStringLiteral("path"), path}});
    }

    QJsonObject card{
        {QStringLiteral("name"), input.name},
        {QStringLiteral("source"), input.source},
        {QStringLiteral("prompt"), promptSummary(input.prompt)},
        {QStringLiteral("files"), files},
        {QStringLiteral("dir"), input.dir},
        {QStringLiteral("createdAt"), input.createdAt},
    };
    if (!input.provider.isEmpty())
        card.insert(QStringLiteral("provider"), input.provider);
    if (!input.model.isEmpty())
        card.insert(QStringLiteral("model"), input.model);
    return card;
}
