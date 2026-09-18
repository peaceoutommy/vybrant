// Versioned JSON project format with forward migration.
#pragma once
#include "project/Project.h"
#include <QJsonDocument>

namespace mt {

class ProjectSerializer {
public:
    static constexpr int kFormatVersion = 1;
    static QJsonObject toJson(const Project& p);
    static bool fromJson(const QJsonObject& root, Project& out, QString* error = nullptr);
    static bool save(const Project& p, const QString& path, QString* error = nullptr);
    static bool load(const QString& path, Project& out, QString* error = nullptr);
    // Migrate an older document in place to kFormatVersion
    static QJsonObject migrate(QJsonObject root, int fromVersion);
};

}   // namespace mt
