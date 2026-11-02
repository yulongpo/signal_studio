#pragma once
#include <QString>
#include "domain/project.h"

namespace signalstudio {
// Run on a temporary decoded Project before legacy detection-slot retirement.
// Failure never installs the candidate or writes the source project.
bool migrateRevisedSourceAnnotations(Project&, QString& error);
}
