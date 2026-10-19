#pragma once
#include <QJsonObject>
#include <QString>

#include "domain/project.h"
namespace signalstudio {
// Immutable content-addressed shards are written before the project manifest.
bool writeAnnotationStore(const QString& projectPath, const FileState&,
                          QJsonObject& index, QString& error);
bool readAnnotationStore(const QString& projectPath, const QJsonObject& index,
                         FileState&, QString& error);
}  // namespace signalstudio
