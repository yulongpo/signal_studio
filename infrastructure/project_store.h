#pragma once

#include "domain/project.h"

#include <QString>
#include <QJsonObject>

namespace signalstudio {

// Native state only: this schema intentionally differs from browser prototype JSON.
class ProjectStore {
public:
    static bool save(const QString& path, const Project& project, QString& error);
    static bool load(const QString& path, Project& target, QString& error);
    static bool decodeDocument(const QJsonObject& document, const QString& assetPath,
                               Project& target, QString& error);
    static QByteArray stateToken(const Project& project);
    static bool encodeDocument(const QString& assetPath, const Project& project,
                               QJsonObject& document, QString& error);
};

} // namespace signalstudio
