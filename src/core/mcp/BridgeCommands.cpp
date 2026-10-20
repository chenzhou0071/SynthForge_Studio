#include "core/mcp/BridgeCommands.h"

BridgeCommand defaultBridgeCommand(const QString& area)
{
    BridgeCommand command;
    command.env = QProcessEnvironment::systemEnvironment();

    if (area == QStringLiteral("blender")) {
        command.program = QStringLiteral("uvx");
        command.args = {QStringLiteral("--python"), QStringLiteral("3.11"),
                        QStringLiteral("mcp-for-blender")};
        command.env.insert(QStringLiteral("UV_PYTHON_PREFERENCE"),
                           QStringLiteral("only-managed"));
    } else if (area == QStringLiteral("unity")) {
        command.program = QStringLiteral("uvx");
        command.args = {QStringLiteral("--from"), QStringLiteral("mcpforunityserver"),
                        QStringLiteral("mcp-for-unity"), QStringLiteral("--transport"),
                        QStringLiteral("stdio")};
    }
    return command;
}
