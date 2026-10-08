#pragma once

#include "domain/project.h"

#include <QString>

namespace signalstudio {

// Native state only: this schema intentionally differs from browser prototype JSON.
class ProjectStore {
public:
    static bool save(const QString& path, const Project& project, QString& error);
    static bool load(const QString& path, Project& target, QString& error);
};

} // namespace signalstudio
