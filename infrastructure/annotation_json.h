#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "domain/project.h"

namespace signalstudio {
QJsonObject encodeAnnotation(const Annotation&);
bool decodeAnnotation(const QJsonObject&, const FileMetadata&, Annotation&,
                      QString& error);
QJsonObject encodeSourceSet(const AnnotationSourceSet&);
bool decodeSourceSet(const QJsonObject&, const FileMetadata&,
                     AnnotationSourceSet&, QString& error);
QJsonObject encodeSourceIdentity(const SourceIdentity&);
bool decodeSourceIdentity(const QJsonObject&, SourceIdentity&, QString& error);
QJsonObject encodeAnnotationChannelSnapshot(const AnnotationChannelSnapshot&);
bool decodeAnnotationChannelSnapshot(const QJsonObject&, const FileMetadata&,
                                     AnnotationChannelSnapshot&, QString& error);
QByteArray exportAnnotationsJson(const FileState&);
QByteArray exportAnnotationsCsv(const FileState&);
}  // namespace signalstudio
