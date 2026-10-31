// Updates: a release build asks download.vybrant.studio/latest.json (written by .github/workflows/downloads.yml) on start whether
// there is a newer version, and offers it with its notes (the release tag's message); the user picks. An installed Windows copy runs the new installer, an AppImage replaces
// its own file; anything else (the portable zip, a source build) is pointed at the download page.
#pragma once
#include <QJsonObject>
#include <QString>

class QWidget;

namespace mt::updater {
QString currentVersion();   // this build's version, from the release tag; empty for a development build, which never checks
// The manifest's build for 'platform' ("windows" / "linux") when the manifest's version is newer than 'current'; empty otherwise,
// and for a build without the checksum the download is verified against
QJsonObject newerBuild(const QJsonObject& manifest, const QString& current, const QString& platform);
void check(QWidget* window, bool manual);   // manual (Help menu): also says when there is nothing new, or the check failed
}   // namespace mt::updater
