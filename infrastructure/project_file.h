#pragma once
#include <QString>
#include <functional>
#include "domain/project.h"

namespace signalstudio {
enum class AssetSaveMode { Copy, Reference, Preserve };
class ProjectFileCodec {
 public:
  static bool save(const QString& path, Project& project, QString& error,
                   AssetSaveMode mode = AssetSaveMode::Preserve,
                   std::function<bool()> cancelled = {},
                   std::function<bool()> beforeCommit = {});
  static bool load(const QString& path, Project& target, QString& error,
                   std::function<bool()> cancelled = {});
  static bool importLegacy(const QString& legacy, const QString& destination,
                           Project& target, QString& error,
                           std::function<bool()> cancelled = {});
  static QString assetDirectory(const QString& path, const Project& project);
  static bool relocate(FileMetadata& source, const QString& path, QString& error);
  static QString signature(const QString& path, QString& error);
};
}  // namespace signalstudio
